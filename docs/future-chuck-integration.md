# Future ChucK Integration — Documentation (A6)

> **Phase 2.5, item A6 (Status: `[ ]`.** This document is the **single source of truth** for the *deferred* ChucK integration strategy. It is documentation only — it writes no implementation code, adds no dependencies, wires no runtime, and changes no UI behaviour. The actual ChucK runtime/evaluation work is owned by **B4** (Phase C), which implements everything this document *describes* but does *not* do itself.
>
> Source of truth: `docs/PROGRAM.md` (decisions #6, #9–#12, #18–#22; the "V2 Architecture" section; §Phase C (B4); B4-K0.5 … B4-K8). Where this document and `PROGRAM.md` differ, `PROGRAM.md` wins.

---

## 1. Scope and decision boundaries

This document draws a sharp line between documentation (A6) and implementation (B4). It is deliberately **not** an implementation spec — it preserves the architectural decisions so B4 does not have to rediscover them.

| Item | Scope | Delivers | Does **not** deliver |
|------|-------|----------|----------------------|
| A5 | ChucK grammar → tokeniser | `.ck` file recognition + syntax highlighting via an adapted public ChucK grammar (`forrcaho/vscode-chuck`, itself a port of Atom `cjwilburn/language-chuck`). Tokeniser is a `juce::CodeTokeniser` subclass mapping ChucK keywords/classes/strings/comments to the existing colour scheme. | ChucK execution, semantics tracking, runtime. |
| **A6 (this)** | Deferred ChucK integration strategy | This document: SWOT, `libchuck` precedent, sandbox risks, clock-sync risks, the intended B4 real-time eval path, and clear boundaries. | Any ChucK execution, runtime, worker, IPC, or UI behaviour. |
| B4 | Real ChucK (Phase C, V2) | The real-time eval path, out-of-process worker, per-tab VM isolation, timestamped event queue, sandboxing/safety, and verification (B4-K0.5 … B4-K8). | Nothing documented *here* is implemented by A6. |

### Current state vs. deferred state

**Current (audited @ `5ba7710`):** the repository has **no ChucK runtime, no `libchuck` dependency, and no `.ck` execution path.**

- `.ck` files are *recognised* by the file-type classifier but are **inert**:
  - `ui/ExplorerFileTypes.hpp:64` — `SongChuck` is "recognized but eval not yet wired".
  - `ui/ExplorerTreeItems.cpp:66-68` — the icon is cosmetic; no eval wiring.
  - `ui/SettingsComponent.cpp:348-376` + `ui/SettingsComponent.hpp:194-195` — the Settings tab holds an explicit **ChucK placeholder** reading "ChucK integration — implemented in Phase C (B4).".
  - `ui/HathorTab.cpp` + `ui/EditorArea.cpp` always select `MiniNotationTokeniser` — there is no `.ck` extension recognizer on the tab path yet (that lands in A5).
- There is **no `libchuck` import, vendored build, or ChucK header** anywhere in `app/`, `engine/`, `control/`, or `ui/`. The only `.ck` mention in the tree is a test fixture string (`tests-ui/test_tree_builder.cpp:114`).
- Transport is a **single global clock** (decision #11's per-tab VM isolation is the *future* model, not the present one): `app/AudioEngine.hpp:183-186` (`sampleClock_`, `bpm_{120.0}`, `running_{true}`, `sampleRate_{44100}`) plus `ControlInterface::handlePlay/Stop` (`control/ControlInterface.cpp:124-126,198-208`) — global only, no per-slot play/stop yet (that lands in A3).

**Deferred (B4) — not present, not running, not a no-op stub:** B4 is expected to implement a *real* ChucK eval path (decision #9 overrides the earlier safe-default scope). A `.ck` tab that cannot execute **must say why** (tooltip/status) — it must never silently do nothing, and it must never be a disabled-with-tooltip stub that pretends to work.

---

## 2. ChucK SWOT

Relevant only to Hathor's architecture and intended use as a music/live-coding environment (per decision #12: executing arbitrary, often AI-generated code near real-time audio).

### Strengths

| Strength | Why it matters to Hathor |
|----------|--------------------------|
| **Strong real-time audio + precise timing model.** ChucK has a well-defined sample-accurate clock (`now`, `next_dur`, shred scheduling). | Matches Hathor's own sample-accurate engine clock (`AudioEngine::sampleClock_`, `cycleStart`/`cycleEnd` math in `app/AudioEngine.cpp:349-373`). The two can be bridged rather than fought. |
| **Compositional, UGen-graph synthesis.** `=>` audio-graph construction is native, not an library call. | Fits Hathor's pattern → voice → audio chain; a `.ck` instrument slots in as a voice-level producer (cf. `VoicePool`). |
| **On-the-fly recompilation / hot-swap of shreds.** | Mirrors Hathor's `set-pattern` hot-swap model (`WorkerThread.cpp:124-171`); eval can replace a shred per-tab without stopping the world. |
| **Existing embed story.** `libchuck` exposes a C/C++ API for hosting. | B4 does not have to treat ChucK as a dumb subprocess; it can be embedded *with the safety boundaries this document insists on*. |

### Weaknesses

| Weakness | Risk to Hathor |
|----------|----------------|
| **Hostile to the calling process on a real-time thread.** A runaway shred (`while(true) {}` with no `now +=>`) blocks its thread; a native crash in `libchuck` can corrupt the host if embedded in-process. | This is the central danger the V2 architecture avoids (decision #12: per-tab VM + out-of-process worker + watchdog). |
| **No built-in resource limits or per-VM CPU capping.** | A single `.ck` tab can saturate a core. B4's bounded-resource policy (decision #24) is mandatory, not optional. |
| **Concurrency model is under-documented / an unverified assumption.** | `libchuck`'s `compileCode()`/`run()` thread-safety is **not** assumed safe (B4-K0.5 spike); embedding must not rely on it. |
| **Garbage-collected / stop-the-world pauses are not airtight for RT.** | Reinforces the need for out-of-process execution + lock-free sample transport (decision #23). |

### Opportunities

| Opportunity | Hathor's path |
|-------------|---------------|
| **AI-generated instruments.** The V2 "Cursor for DJs" trust model (decision #12) is the *reason* this is hard and must be sandboxed. | B4's isolation makes AI-authored `.ck` safer than a single shared VM ever could. |
| **Bake-to-Song lifecycle (B8).** A live VM exists only while auditioning; baking renders to `.wav` and shuts the VM down. | Turns ChucK from a *permanent residency* into an *instrument workshop* — most instruments don't linger as live processes (V2 Architecture §6). |
| **`.ck` as first-class alongside `.hathor`.** With A5 recognition + syntax highlighting, the UX matches the existing Strudel mini-notation flow. | The eval path (B4) parallels `set-pattern` rather than inventing a third workflow. |
| **Baked assets feed SampleBank + `.hathor` autocomplete.** | `d1 $ s "acid_bass"` resolves to an ordinary sample asset, no live VM required (B8-K4). |

### Threats

| Threat | Mitigation lives in |
|--------|---------------------|
| **Arbitrary code execution against local project data.** AI-authored `.ck` runs with the user's privileges. | Out-of-process worker (B4-K2), per-VM isolation (B4-K3), capability audit (AI-1). |
| **Filesystem / system access from ChucK.** ChucK can read files and, depending on the host API, touch the filesystem. | Sandbox strategy documented in §4; filesystem access must be mediated/restricted before B4 ships. |
| **Real-time audio thread hangs/crashes.** A bad `.ck` must never hang/crash the JUCE audio thread or Hathor. | The hard gate: B4-K8 tests a hung shred and a native-crash/worker-death case (DoD §6.2 is non-optional). |
| **Clock drift / two timing systems fighting.** Hathor's `sampleClock_` vs. ChucK's `now`. | Timestamped cross-process events (decision #22, B4-K6); Tidal-as-master, not polling BPM. |
| **`libchuck` not vendored / integrated yet.** Nothing is embedded; B4 must stand up the worker + libchuck wiring. | A6 documents intent; B4-K0.5/K0.6 spikes validate the assumptions this document records. |

---

## 3. Architecture (current vs. deferred)

### Current architecture (audited)

```
Hathor main process
├── UI (JUCE)              ui/  (.hathor tabs, MiniNotationTokeniser, Settings placeholder)
├── control plane          control/  (socket server + ControlInterface: set-pattern/play/stop/bpm/set-gain/slot-play/slot-stop)
├── audio engine           app/      (AudioEngine: single global clock sampleClock_, bpm_, running_; VoicePool; SpscRingBuffer in VisualizerFrame.hpp)
└── engine runtime         engine/   (Pattern<hathor::ParamMap>, MiniAst, MiniParser, Event<ParamMap>)
```

- Transport = **one global clock** (`AudioEngine.hpp:183-185`). Per-slot play/stop (A3) is not yet wired despite `handleSlotPlayStop` existing as a handler shell (`ControlInterface.hpp:115-116`, `ControlInterface.cpp:159-161`).
- `.ck` is **recognised but completely inert** — no compilation, no VM, no audio path. The Settings tab placeholder (`ui/SettingsComponent.cpp:365`) states this explicitly.

### Deferred architecture (B4 implements, not A6)

```
Hathor main process                        hathor-audio-worker (companion process, owns libchuck)
├── Tidal / Strudel pattern scheduler    ──┐
├── UI / Project / SampleBank            ──┼──▶ timestamped musical event queue (SAMPLE_TS)
└── AudioEngine (master clock)           ──┘
                                           │ lock-free SPSC audio/event transport
                                           ▼
                                    per-tab Chuck_VM(A)   per-tab Chuck_VM(B)
                                    own thread · own watchdog · own lifecycle
```

This is the **V2 Architecture** from `PROGRAM.md` §"V2 Architecture" (decision #12). It is the authoritative future design — **B4** turns it into code; **A6** only documents it so B4 doesn't have to re-derive it. Key properties documented here for B4:

1. **Per-tab `Chuck_VM` isolation** — one VM, one dedicated OS thread, one watchdog, one lifecycle per active `.ck` tab; one tab's failure never silences another (decision #12).
2. **Out-of-process `hathor-audio-worker`** — all ChucK VMs live in a companion process so a native `libchuck` crash cannot take down the main process (decision #12, B4-K2).
3. **`B4-K0.5` libchuck concurrency spike** — `compileCode()`/`run()` thread-safety is an unverified assumption; the spike records a GO/NO-GO before any K2+ code builds on it.
4. **`B4-K0.6` cross-process audio IPC spike** — the shared-memory ring must *demonstrate* RT-safety (the spike lists 14 explicit tests) before B4-K2 treats the transport as real-time-safe.
5. **Tidal = master conductor** — the pattern scheduler emits **explicitly timestamped** ChucK events; the worker schedules by the event's authoritative timestamp, not by IPC-arrival instant (decision #22, B4-K6).

---

## 4. The `libchuck` embedding precedent

### What the precedent is

`libchuck` is the C/C++ embedding interface to the ChucK compiler/runtime. It exposes programmatic hooks (e.g. `compileCode`, `run`, VM + shred management) so a host can compile and execute ChucK source *inside* a process rather than driving the `chuck` CLI as a black box. It is the standard path used by other hosts that embed ChucK for plugin/scripting integration.

### Why it matters for Hathor's future architecture

Hathor is **not** treating ChucK as a dumb external command-line process. Per decision #12, the V2 design embeds `libchuck` **inside** the companion `hathor-audio-worker` process. That embedding precedent is the foundation for:

- **Per-VM isolation:** `libchuck` lets the worker create multiple independent `Chuck_VM` instances (one per active `.ck` tab), each on its own thread — matching the "one tab's failure never silences another" requirement.
- **Hot-swap semantics:** `compileCode`/`run` map naturally onto Hathor's existing pattern hot-swap flow (`WorkerThread.cpp:124-171`), so `.ck` eval parallels `set-pattern` rather than re-inventing an audition path.
- **Crash containment:** because `libchuck` is embedded in a *separate process* (not in the JUCE audio thread / main process), a native crash is contained to the worker — the main process survives and restarts it (decision #12, B4-K2, B4-K8).

### What this document does **not** claim

- It does **not** claim Hathor currently embeds `libchuck`. There is no vendored `libchuck` in the repo today — A6 is documentation; B4 vendors + embeds it.
- It does **not** claim `libchuck`'s threading model is safe. `compileCode()`/`run()` thread-safety is an unverified assumption that **B4-K0.5** must spike before relying on it; if unsafe, B4 must serialize the compile path.
- It does **not** select a `libchuck` version or pin an API. Per `PROGRAM.md` §"Verification-first" (decision #18), B4/AI-5 must inspect the *actual* vendored compiler API before coding; A6 records only that the real compiler is the authoritative diagnostic source.

---

## 5. Sandbox / safety risks and safeguards

Hathor's product model (`docs/PROGRAM.md` decision #12) is explicitly "arbitrary, often AI-generated code near real-time audio" — stronger isolation is required than for a conventional trusted-human ChucK host. These risks are **not** hypothetical for the planned use; the safeguards are **B4's** to implement.

### Risk register

| Risk | Description | B4 safeguard(s) | Current state |
|------|-------------|-----------------|---------------|
| **Filesystem access** | ChucK can read/write files via `File` and related APIs. | Mediate or restrict the working directory / file APIs available to the VM; confine to project sandboxes. Document the surface actually exposed (AI-1 capability model). | **No sandbox exists.** A6 documents; B4 builds. |
| **Process / system access** | ChucK can spawn processes (`Machine`) and touch system state. | Run in an out-of-process worker with a bounded capability set; no shell exec exposed to scripts. | **Not contained.** |
| **Untrusted / AI-generated code** | `.ck` may be AI-authored and semantically hostile. | Out-of-process `hathor-audio-worker` (decision #12); per-tab VM isolation so one bad tab cannot silence others; capability audit per AI-1. | **No trust boundary today.** |
| **Resource exhaustion** | A single VM can saturate a core or leak memory. | Bounded per-tab live-VM policy (decision #24): max concurrent VMs, LRU suspend, CPU/RAM ceilings, suspend-without-state-loss. Resource policy is changeable without rewriting per-tab isolation. | **Unbounded (no VMs exist yet).** |
| **Runaway computation** | `while(true){}` with no `now +=>` blocks a VM thread. | Per-VM watchdog + heartbeat (`chuckHeartbeat_` in B4-K5); on stall (~2 s) tear down + restart **that tab's** VM only; never the whole worker. | **No watchdog.** |
| **Lifecycle / cleanup** | VM threads / shared memory must not leak on eval, stop, or worker death. | Explicit VM lifecycle maps tabs↔VMs; worker liveness + generation token; shared-memory reinit on death; clean shutdown of baked VM threads (B8-K3). | **No lifecycle management.** |
| **Local project data** | A hostile/AI `.ck` must not corrupt `.hathor_assets/`, `.hathor`, or samples. | Asset writes confined to the B8 bake pipeline (committed on confirmation); `.ck` eval reads through a restricted surface; overwrite of existing assets requires confirmation (AI-1/AI-6). | **No mediation.** |

### Architectural safeguards B4 must establish (not A6)

1. **Process boundary (decision #12, B4-K2).** ChucK VMs live in `hathor-audio-worker`, not in the JUCE main/audio process. A native crash is contained + recoverable.
2. **Per-tab VM isolation (decision #12, B4-K3).** Independent VM + thread + watchdog + lifecycle per active tab; tab↔VM mapping is rebuildable after a worker restart.
3. **Bounded live-VM policy (decision #24, B4-K3).** No VM is auto-created just because a `.ck` is open; only actively-playing/eval'd tabs get one; inactive tabs suspend/destroy per a configurable ceiling.
4. **Heartbeat + watchdog (B4-K5).** A low-frequency checker flags a stalled tab (~2 s) and restarts *that tab only*; the JUCE audio thread never hangs.
5. **Shared-memory generation token (PART C, B4-K2).** The worker and each audio transport carry a generation/liveness identity so the main process distinguishes a *dead* worker from *stale shared-memory* left by a prior lifecycle — never read stale memory as valid audio.
6. **Control plane vs. audio plane separation (`B4-K0.6`).** Commands, status, lifecycle = socket/IPC; audio samples = shared-memory ring. Do not conflate them.
7. **Hard gate tests (B4-K8, DoD §6.2).** A hung shred and a natively-crashing `.ck`/worker must neither hang nor crash the JUCE audio thread or the rest of Hathor; recovery must be observable to the user.

> **No sandbox exists in A6's scope.** A6 documents these risks and their intended mitigation locations so B4 can implement them without rediscovery. Do not claim a complete sandbox is present.

---

## 6. Clock-synchronization risks and safeguards

This is where the two timing systems meet, and where the V2 design's strongest opinion lives (decisions #22, #23; B4-K6). A6 records the risks; **B4 implements + verifies** the synchronization.

### The two clocks

| Clock | Owner | Source (current code) | Unit |
|-------|-------|-----------------------|------|
| **Hathor audio/engine clock** | `AudioEngine` (`app/AudioEngine.hpp:183-186`) | Global transport; `sampleClock_` incremented by `bufferSize` per JUCE audio callback (`app/AudioEngine.cpp:334-336`). BPM scaled to int64 as `bpm/60` for cycle math (`app/AudioEngine.cpp:360-373`). | samples (per-callback advance) |
| **ChucK time** | `Chuck_VM` (not yet embedded) | ChucK's own `now` / `next_dur` / shred scheduler. | ChucK time (`now`), independent base |

### Risks

| Risk | Why it is dangerous for Hathor |
|------|---------------------------------|
| **Independent clocks cannot be reconciled.** Two free-running clocks drift; "aligned to the next 16 bars at BPM X" only works if ChucK trusts Hathor's musical time. | Without a shared reference, a `.ck` instrument scheduling `now => 1::bar` drifts from Hathor's `cycleStart/cycleEnd` grid. |
| **Polling BPM is insufficient.** Reading Hathor's `bpm_` periodically does not convey per-event sample timestamps. | IPC latency becomes the *de facto* timing reference — exactly what decision #22 forbids. |
| **"Within one buffer" is a silent downgrade.** If IPC delivery is used as the timing reference, "sample-accurate" becomes "best-effort within a buffer" without anyone noticing. | Decision #22: sample-accurate behaviour must be **demonstrated, or its measured limit explicitly documented** — never silently downgraded. |
| **Late arrivals break musical alignment.** An event that arrives after its sample timestamp is musically wrong if fired immediately. | Must define: drop + report, or reschedule aligned to the next boundary (B4-K6 chooses one and documents it). |
| **Early arrivals block.** Holding an event "until its timestamp" is correct — but must not block the real-time audio callback. | Worker holds against its audio timeline; the audio plane stays lock-free (`SpscRingBuffer` pattern, `VisualizerFrame.hpp`). |
| **Buffer-boundary misalignment** causes sub-sample jitter on instrument triggers/gate. | Must defer to the buffer boundary for sample-alignment, or compensate. |
| **Drift over long sessions** between master and worker sample clocks. | Must detect (running offset) + correct (continuous compensation / shared timebase). |

### Intended synchronization strategy (B4's to implement)

Per `PROGRAM.md` §V2 Architecture step 4 and B4-K6:

1. **Tidal = master conductor.** The Phase-1 pattern scheduler (`engine/`, `app/`) is the authoritative musical clock — **not** ChucK's, **not** a BPM poll.
2. **Timestamped cross-process events (decision #22).** Each musical event carries an explicit audio/sample timeline timestamp:
   ```text
   Event { type, payload, musical timestamp, sample/frame timestamp }
   ```
   Structure follows the engine's existing clock model (`sampleClock_`, cycle math in `app/AudioEngine.cpp:349-373`).
3. **Worker schedules by timestamp, not by arrival.** `Tidal/Hathor master clock → timestamped event → IPC transport → ChucK worker → schedule against timestamp → audio output`. IPC latency becomes a *transport* problem, not a musical-timing reference.
4. **Shared timeline / offset compensation.** The worker holds enough shared-clock info to compensate for cross-process transit and to map a timestamp onto its own audio timeline.
5. **Late/early/boundary/drift rules** are explicitly defined in B4-K6 and **demonstrated or measured** (B4-K8 + DoD §6.2) — including what "sample-accurate" means *operationally* in this implementation.

> **No synchronization exists in A6's scope.** A6 records that the two clocks are independent today (one global transport in `app/AudioEngine`; no ChucK VM embedded) and that B4 owns the timestamped-queue + sample-accurate bridging. Do not claim synchronization is present.

---

## 7. The real-time evaluation path (B4's to build)

This is the high-level flow A6 expects B4 to realise. It is **described, not implemented, here.**

```text
.ck editor   (tab recognised by A5; eval wired by B4)
     ↓
evaluation request (Ctrl+Enter / Ctrl+Alt+Enter, parallel to .hathor set-pattern)
     ↓
ChucK runtime / embedded integration (libchuck inside hathor-audio-worker; per B4-K0.5 spike)
     ↓
controlled real-time execution (per-tab Chuck_VM; isolated thread + watchdog; B4-K3/B4-K5)
     ↓
Hathor audio/runtime environment (lock-free SPSC audio transport; sample-accurate via B4-K6)
```

### What is known (from `PROGRAM.md`)

- **Eval is real.** Decision #9: ChucK is "REAL, functioning" — not a stub, not a disabled-with-tooltip. Per decision #6, "eval cannot work, it must say why; otherwise it executes." A6 documents this invariant; B4 honours it.
- **Eval entry is `.ck`-tab-local.** `B4-K7`: `Ctrl+Enter`/`Ctrl+Alt+Enter` compiles via the worker and hands the shred to *that tab's* VM, parallel to how `.hathor` `set-pattern` compiles a pattern for a slot (`control/WorkerThread.cpp:124-171`, `control/ControlInterface.cpp:346-349`).
- **Compile errors surface in the status bar** (same channel as `.hathor` parse errors — see `control/WorkerThread.cpp:94-100` parse-error reporting).
- **A fresh eval replaces the prior shred; a stop path is available** (B4-K5 restart or explicit stop).
- **Per-tab isolation is non-negotiable.** One hung/crashing `.ck` must not affect another tab (`hathor` `SlotState` model, `app/SlotState.hpp:32-38`).

### What remains to be designed / validated by B4

| Area | Known | To be validated by B4 |
|------|-------|-----------------------|
| `libchuck` threading | Not assumed safe (B4-K0.5 must GO/NO-GO). | `compileCode()`/`run()` thread-safety; if NO-GO, a serialized command path. |
| Audio transport | Must be RT-safe shared-memory, not a pipe/socket (B4-K0.6). | 14 explicit spike tests; only on PASS does B4-K2 treat it real-time-safe. |
| Worker IPC | Control plane (socket) vs audio plane (shared-memory ring) is separate (B4-K0.6). | Concrete IPC mechanism, generation/liveness token, crash mid-write recovery. |
| Per-VM resources | Policy is configurable, not auto-per-file (decision #24, B4-K3). | Concrete live-vs-suspend policy, measured CPU/RAM per VM, ceiling, LRU. |
| Timestamping | Events carry sample/frame timestamps; schedule by timestamp not arrival (decision #22, B4-K6). | How timestamps transfer across the process boundary; late/early/boundary/drift rules; operational definition of "sample-accurate". |
| Diagnostics authorship | ChucK diagnostics come from the real compiler (`PROGRAM.md` decision #18, AI-5). | Inspect the actual vendored `libchuck` API before coding diagnostics; do **not** hard-code an assumed `checkSyntax`. |

### The non-negotiable invariants (A6 records; B4 must satisfy)

1. `.ck` evaluation must be **real** — it must produce live, audible, per-tab-isolated ChucK.
2. It must **not** be a silent no-op.
3. It must **not** be a fake/stub execution path.
4. It must respect the audio engine's real-time constraints (off the JUCE audio thread; lock-free audio plane — see A6's own hard gate, DoD §6.2).
5. It must address **sandboxing** (§5) and **clock synchronization** (§6) before being considered production-ready — i.e. the B4-K8 tests must pass.

---

## 8. Verification checklist (this document, not the code)

- [x] File exists at `docs/future-chuck-integration.md` (repository `docs/` directory exists — see `docs/PROGRAM.md` sibling).
- [x] Covers ChucK SWOT.
- [x] Covers the `libchuck` embedding precedent — and explicitly does **not** claim it is already embedded.
- [x] Covers sandbox/safety risks + safeguards, with explicit "no sandbox exists" statement.
- [x] Covers clock-synchronization risks + safeguards, with explicit "no synchronization exists yet" statement.
- [x] Covers the intended B4 real-time evaluation path and explicitly distinguishes known (from `PROGRAM.md`) vs. to-be-validated-by-B4.
- [x] Documents A5 / A6 / B4 decision boundaries — no blurring of responsibilities.
- [x] Contains no implementation code and makes no runtime/UI/build changes.
- [x] Contains no claim that deferred B4 functionality already exists.

### Repository facts used by this document

- `docs/PROGRAM.md` — the source of truth; decisions #6, #9–#12, #18–#22; V2 Architecture section; §Phase C (B4); B4-K0.5…B4-K8; §"Verification-first"; DoD §6.2.
- `ui/ExplorerFileTypes.hpp:64` — `SongChuck` "recognized but eval not yet wired".
- `ui/ExplorerTreeItems.cpp:66-68` — cosmetic `.ck` icon only.
- `ui/SettingsComponent.cpp:348-376` + `ui/SettingsComponent.hpp:194-195` — explicit ChucK placeholder ("implemented in Phase C (B4)").
- `ui/HathorTab.cpp:21` / `ui/EditorArea.cpp:371` — `MiniNotationTokeniser` always selected; save filter `.hathor` only (no `.ck` eval on the tab path yet).
- `app/AudioEngine.hpp:183-186` / `app/AudioEngine.cpp:334-373` — single global transport clock (`sampleClock_`, `bpm_{120.0}`, `running_{true}`, `sampleRate_{44100}`) and the existing cycle-position sample math B4 bridges.
- `app/SlotState.hpp:32-38` — per-slot state (the isolation model `.ck` eval must follow).
- `control/ControlInterface.hpp:115-116,125-128` / `control/ControlInterface.cpp:124-126,159-161,198-208` — global `play`/`stop` (no per-slot play/stop yet — A3's work).
- `control/WorkerThread.cpp:94-100,124-171` — the pattern compile/hot-swap + parse-error path that `.ck` eval parallels.
- `app/VisualizerFrame.hpp` — the existing `SpscRingBuffer` lock-free sample-transport pattern B4 reuses for the audio plane.
- No `libchuck`, no ChucK header, no `.ck` execution path anywhere in `app/`, `engine/`, `control/`, or `ui/` — confirmed by source search.

### No implementation code was changed by A6

This document is documentation only. No build configuration, runtime hooks, UI behaviour, dependency additions, or placeholder execution paths were introduced. The existing ChucK placeholder in `ui/SettingsComponent.cpp:365` already states "implemented in Phase C (B4)" — A6 does not alter it.

### Assumptions / open questions left for B4

1. **`libchuck` version + vendoring location.** A6 assumes B4 will vendor `libchuck`; it does not pin a version or path (per `PROGRAM.md` §"Verification-first", B4/AI-5 must inspect the actual API first).
2. **`libchuck` concurrency model.** Assumed unsafe until B4-K0.5 proves otherwise; the serialized fallback is B4's call.
3. **Concrete IPC transport.** A6 mandates "shared-memory ring, RT-safe, validated by B4-K0.6" but does not select `mmap`+`futex`, `SHM`+`semaphore`, `JUCE SharedMemory`, or another primitive — that is B4-K0.6's spike output.
4. **Timestamp transfer format across the process boundary.** A6 records the *shape* (sample/frame timestamp) but leaves the wire format / shared-clock offset-compensation math to B4-K6.
5. **Operational meaning of "sample-accurate"** in Hathor's worker model — A6 insists it be *demonstrated or measured* (decision #22) and left for B4-K6/B4-K8 + DoD §6.2.
6. **Exact filesystem/process access restrictions** exposed to a `.ck` VM — A6 enumerates the risks but the concrete sandbox surface is B4 (+ AI-1 capability model).
7. **Concrete resource-policy numbers** (max concurrent VMs, CPU/RAM ceiling, suspend threshold) — A6 references decision #24's *requirement* but leaves the measured values to B4-K3.

---

*End of A6 — `docs/future-chuck-integration.md`. The ChucK integration itself is unimplemented; this document exists so the B4 implementation agent can proceed from decided architecture rather than re-deciding it.*
