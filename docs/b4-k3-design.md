# B4-K3: Per-tab ChucK VM Isolation — Design

## Overview

B4-K3 implements per-tab ChucK VM isolation: each active `.ck` tab gets its own
`ChuckVM` instance, running on a dedicated OS thread, with a lifecycle state
machine and bounded resource policy. This document describes the architecture,
state machine, mapping, and integration with the existing audio engine.

## Architecture

### Process model

```
┌─────────────────────────────────────────────────────────┐
│  JUCE main process (hathor-ui)                          │
│  ┌──────────┐  ┌──────────┐  ┌─────────────────┐       │
│  │ HathorTab│  │ AudioEngine│ │ AudioWorkerManager │    │
│  │ (UI)     │  │ (slots)   │ │ (IPC client)      │    │
│  └──────────┘  └──────────┘  └────────┬────────┘       │
│                              Unix socket IPC           │
│                                    │                   │
│                                    ▼                   │
│  ┌─────────────────────────────────────────────────┐   │
│  │  hathor-audio-worker (companion process)         │   │
│  │  ┌──────────────┐  ┌──────────────┐             │   │
│  │  │ Control plane │  │ Placeholder  │             │   │
│  │  │ thread        │  │ production    │             │   │
│  │  │ (socket)      │  │ loop          │             │   │
│  │  ├──────────────┤  └──────────────┘             │   │
│  │  │ VMManager     │  ┌──────────────┐  ┌─────────┐ │   │
│  │  │ (per-tab VMs) │  │ ChuckVM 0    │  │ ChucK   │ │   │
│  │  │               │  │ thread       │  │ thread  │ │   │
│  │  │               │  ├──────────────┤  └─────────┘ │   │
│  │  │               │  │ ChuckVM 1    │  ┌─────────┐ │   │
│  │  │               │  │ thread       │  │ ChucK   │ │   │
│  │  │               │  └──────────────┘  └─────────┘ │   │
│  │  └──────────────┘                               │   │
│  │  ┌──────────────┐                               │   │
│  │  │ Shared mem    │  Audio ring buffer (RT-safe) │   │
│  │  │ ring          │                               │   │
│  │  └──────────────┘                              │   │
│  └─────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

### Key components

| Component | File | Role |
|-----------|------|------|
| `ChuckVM` | `ChuckVm.hpp/.cpp` | Per-tab VM: lifecycle, thread, suspend/resume |
| `VMManager` | `VMManager.hpp/.cpp` | TabId→VM registry, LRU, ceiling enforcement |
| `ResourcePolicy` | `ResourcePolicy.hpp/.cpp` | Data-driven resource config |
| `VmLifecycle` | `VmLifecycle.hpp/.cpp` | K4 compile handoff table (generation tracking) |
| `AudioWorkerManager` | `AudioWorkerManager.hpp/.cpp` | Main-process IPC client + per-tab VM control API |
| `audio_ipc.h` | `audio_ipc.h` | IPC protocol + shared constants |

## State Machine

```
               ┌──────────┐
    activate() │          │
    (new VM)   │ Inactive │
    ──────────►│          │
               └────┬─────┘
                    │ state_.store(Active)
                    ▼
               ┌──────────┐
               │  Active  │◄──────────┐
               │          │           │
               │  thread: │     resume()
               │  render()│           │
               └────┬─────┘           │
                    │                 │
          deactivate(true)  ┌─────────┘
                    │       │
                    ▼       │
               ┌──────────┐ │
               │Suspended │ │
               │          │ │
               │ thread   │ │
               │ paused   │ │
               │ (CV wait)│ │
               └────┬─────┘ │
                    │       │
                    │ destroy()
                    │ vmDestroy()
                    ▼       │
               ┌──────────┐ │
               │Destroyed │ │
               │          │ │
               │ thread   │ │
               │ exited   │ │
               └────┬─────┘ │
                    │       │
                    │ reactivate()
                    │ (new thread)
                    └───────┘
                    ▼
               ┌──────────┐
               │  Active  │
               └──────────┘
```

**States:**
- `Inactive` — VM never created, or destroyed and cleared. No thread.
- `Active` — VM running, thread executing render callback. Has a watchdog.
- `Suspended` — VM paused, thread blocked on condition variable. No watchdog.
- `Destroyed` — VM being torn down or fully destroyed. Thread joined.

**Transitions:**
| From | To | Trigger | Side effects |
|------|----|---------|--------------|
| Inactive → Active | `activate()` | Create OS thread |
| Active → Suspended | `deactivate(true)` | `suspendRequested_` → thread pauses on CV |
| Active → Destroyed | `deactivate(false)` or `destroy()` | Thread joined, ChuckVM cleared |
| Suspended → Active | `resume()` | CV notify, thread resumes |
| Suspended → Destroyed | `destroy()` | Thread joined |
| Destroyed → Active | `activate()` | New thread created |
| Any → Error | Thread crash or compile failure | Recovery: reactivation creates fresh VM |

## TabId → VM → Thread → Watchdog Mapping

### Stable identity
- `TabId` = slot index in `[0, 15]` (matches `AudioEngine::kNumSlots`).
- The mapping `TabId → ChuckVM*` is stored in `VMManager::vms_` (a
  `std::unordered_map<TabId, std::unique_ptr<ChuckVM>>`).
- After a worker restart, the mapping is rebuilt: the main process calls
  `vm_activate` for each tab that should have a live VM. No raw pointers
  cross the process boundary — only `TabId` and state.

### Per-VM resources
Each `ChuckVM` owns:
1. **One OS thread** (`std::thread chucKThread_`) — runs `chucKThreadLoop()`.
2. **One mutex** (`mutex_`) — guards state transitions.
3. **One condition variable** (`suspendCv_`) + `suspendMtx_` — for suspend/resume.
4. **Heartbeat counter** (`heartbeat_.atomic<uint64_t>`) — incremented per block.
5. **Block counter** (`blocksProduced_.atomic<uint64_t>`) — for diagnostics.

### Watchdog lifecycle
- **Active VM:** has a watchdog. The worker's watchdog checker (B4-K5) polls
  `heartbeat_` periodically; if it hasn't advanced in ~2s, the VM is restarted.
- **Suspended VM:** NO watchdog. The thread is paused; heartbeat is frozen.
- **Destroyed VM:** NO watchdog. Thread has exited.

## Resource Policy

Configuration-driven via `ResourcePolicy` (Decision #24):

```json
{
  "maxConcurrentLiveVMs": 16,
  "maxThreads": 32,
  "maxVmMemoryMb": 256,
  "idleSuspendTimeoutSec": 30,
  "preferSuspendOverDestroy": true,
  "ceilingBehavior": "LRU_SUSPEND"
}
```

### Ceiling behaviors
| Behavior | At ceiling | When ceiling is reached |
|----------|------------|------------------------|
| `RejectWithError` | New activation returns HTTP 429 | Ceiling cannot be relieved |
| `LRUSuspend` | Evict least-recently-used **suspended** VM | A suspended VM is destroyed to make room |
| `LRUDestroy` | Evict least-recently-used VM (suspend or active) | LRU VM is destroyed |

Note: Active VMs are never silently destroyed — only suspended VMs are
candidates for eviction under LRU behaviors.

### "Active" definition (PROGRAM.md policy #1)
A tab is "active" (gets a live VM) when:
- `SlotState::running` is `true` — the user pressed Play for this tab, OR
- The tab is being eval'd (Ctrl+Enter) — transient activation.

Open-but-not-playing tabs get **no VM** (policy #1: "not auto-created merely
because a .ck file is open").

## Integration with AudioEngine

### Current flow (pre-B4-K3)
```
User clicks Play tab 3
  → HathorTab::onPlayStopClicked
  → ControlInterface::handleSlotPlayStop(3, start=true)
  → AudioEngineFacade::slotPlay(3)
  → SlotState::running.store(true)
```

### Post-B4-K3 flow
```
User clicks Play tab 3
  → HathorTab::onPlayStopClicked
  → ControlInterface::handleSlotPlayStop(3, start=true)
  → AudioEngineFacade::slotPlay(3)
  → SlotState::running.store(true)
  → AudioEngine::activateTabVM(3)  [NEW]
    → AudioWorkerManager::activateTabVM(3)
    → sendControlCommand("vm_activate 3 ...")
    → worker: VMManager::activateVM(3)
      → ChuckVM::activate() → spawns ChucK thread
```

When the tab is stopped:
```
User clicks Stop tab 3
  → slotStop(3)
  → SlotState::running.store(false)
  → AudioEngine::deactivateTabVM(3)  [NEW]
    → AudioWorkerManager::deactivateTabVM(3, suspend=true)
    → worker: VMManager::deactivateVM(3, suspend=true)
      → ChuckVM::deactivate(true) → thread pauses, state = Suspended
```

### AudioEngine changes
- `AudioEngine` holds an `std::unique_ptr<AudioWorkerManager> workerMgr_`.
- `slotPlay(int)` calls `workerMgr_->activateTabVM(tabId)`.
- `slotStop(int)` calls `workerMgr_->deactivateTabVM(tabId, true)`.
- `initialise()` spawns the worker process.
- Destructor sends "stop" and joins.

## K0.5 Integration

K0.5 (Decision #24 / `docs/b4-k0-5-decision.md`) forbids concurrent
compile/run. B4-K3 enforces this:

- `ChuckVM::compileCode()` returns immediately with "deferred to VM thread"
  when the VM is Active (the actual compile will happen via B4-K4's
  compile-dispatcher request pipe).
- When VM is Suspended, compile is safe (thread is paused).
- The worker's control plane is single-threaded for commands, so
  compile requests are naturally serialized.

## Worker Restart Recovery

After a worker crash:
1. `AudioWorkerManager::isWorkerAlive()` detects death via `waitpid(WNOHANG)`
   + heartbeat staleness + `workerAlive` flag.
2. `AudioWorkerManager::restart()` spawns a new worker with a new generation.
3. The `SlotState::running` bits are preserved in the main process.
4. For each slot with `running == true`, `AudioEngine` calls
   `activateTabVM(slotId)` to rebuild the mapping.
5. The new worker starts with a fresh `VMManager` — no stale VM state.

## Failure Isolation

### Thread crash
If one `ChuckVM`'s thread crashes (segfault in libchuck, etc.):
- The thread terminates, `VMState` remains Active.
- B4-K5 watchdog detects heartbeat stall (~2s).
- `VMManager::checkHeartbeats()` calls `destroy()` + `activate()` to restart.
- Other VMs are unaffected — they have independent threads.

### VM hang (shred with infinite `now +=>`)
- Same as thread crash but the thread is alive (spinning).
- Watchdog detects heartbeat stall → restart that tab's VM only.

### Worker process death
- Detected by `waitpid(WNOHANG)` in `AudioWorkerManager`.
- All per-tab VMs die (they're in the worker process).
- Next audio read returns silence (generation mismatch).
- `audio_` thread is never blocked — `tryReadAudioBlock` returns false.

## Testing

Tests in `tests/test_b4_k3_vm_isolation.cpp`:
1. Open tab → no VM created (acceptance #1)
2. Single VM activation (acceptance #2)
3. Two independent active VMs (acceptance #3)
4. Stopping one VM doesn't affect other (acceptance #4)
5. Failure isolation — destroying one VM (acceptance #5)
6. Suspension pauses VM (acceptance #6)
7. Resume restores VM (acceptance #7)
8. Resource ceiling enforcement (policy)
9. Ceiling eviction via LRU suspend (LRU)
10. Worker restart allows re-activation (recovery)
11. Repeated VM lifecycle cycles (stress)
12. Resource policy JSON serialization (unit)
13. ChuckVM state machine unit test (unit)
