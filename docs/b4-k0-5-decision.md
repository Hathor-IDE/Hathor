# B4-K0.5 Decision: libchuck compileCode()/run() Thread Safety

**Status:** DECIDED — NO-GO (unsafe for concurrent B4 handoff pattern)

**Date:** 2026-08-08

**Decision Makers:** Spike test analysis (experimental + source review)

**Context:** B4-K4 (per-tab VM isolation, out-of-process worker) requires a
concurrency pattern where Thread A (VM owner) continuously calls
`ChucK::run()` while Thread B (worker) calls `ChucK::compileCode()` to
compile and spork shreds. This document records the experimental findings
from `spikes/b4-k0-5/`.

## TL;DR

**ThreadSanitizer detected data races in both the deferred (`immediate=FALSE`)
and immediate (`immediate=TRUE`) spork paths.** Even the lock-free
`FinalRingBuffer` deferred path is not thread-safe for the B4 handoff
pattern as currently implemented in libchuck. The B4-K4 architecture
must serialize compile and VM execution, or use an out-of-process
isolation boundary.

## Experimental Setup

### Test Harness

`spikes/b4-k0-5/spike_test.cpp` runs six test cases against libchuck
(ccrma/chuck, `main` branch at v1.5.5.9-dev):

| Case | Description | Iterations |
|------|-------------|------------|
| A    | Baseline VM lifecycle (no concurrent compile) | N/A |
| B    | Single compile from Thread B while VM runs (both modes) | 1 |
| C    | Repeated compilation (both modes) | 1000 |
| D    | Sustained concurrency (both modes) | 10,000 |
| E    | Rapid handoff — high-frequency, no sleep (both modes) | 20,000 |
| F    | Shutdown race — compile while VM shutting down (both modes) | ~200 |

### Build Configurations

1. **ASan + UBSan** (`-fsanitize=address,undefined`) — memory safety + UB
2. **TSan** (`-fsanitize=thread`) — data race detection
3. **Debug** (no sanitizer) — baseline behavior

### Concurrency Pattern

```
Thread A (VM owner):        Thread B (compile worker):
  loop:                       loop:
    chuck->run(input, output)   chuck->compileCode(code, ...)
    sleep(100us)              sleep(500us or none)
```

## Results

### AddressSanitizer + UndefinedBehaviorSanitizer (ASan+UBSan)

```
=== GO/NO-GO DECISION ===
Deferred (immediate=FALSE): SAFE (no crashes)
Immediate (immediate=TRUE): SAFE (no crashes)
B4-K0.5 DECISION: GO (with caveat)
```

- **Zero ASan errors** — no buffer overflows, use-after-free, or memory
  corruption detected in any test case.
- **Zero UBSan errors in spike code** — however, libchuck itself produces
  UBSan findings (pre-existing type mismatches in `ck_begin_class` calls,
  `Chuck_Object` ↔ `Chuck_Type` casts in `chuck_oo.cpp:295`). These are
  ChucK library bugs, not spike code issues.
- All 20,000 compile attempts succeeded in Cases C-E; 100% success rate
  across all iterations.
- Zero VM crashes detected.

### ThreadSanitizer (TSan)

**Critical finding:** TSan detected **multiple data races** starting from
Case B (the first concurrent test):

```
WARNING: ThreadSanitizer: data race
  Write at chuck_vm.cpp:1290 by Thread A (VM owner, in Chuck_VM::spork)
  Read at chuck.cpp:1325 by main thread (compileCode, in Chuck_VM::spork)

WARNING: ThreadSanitizer: data race
  chuck_vm.cpp:1299 in Chuck_VM::spork(Chuck_VM_Shred*)
  chuck_vm.cpp:589 in Chuck_VM::compute()

WARNING: ThreadSanitizer: data race
  chuck_vm.cpp:2357 in Chuck_VM_Shred::run(Chuck_VM*)
  chuck_vm.cpp:2360 in Chuck_VM_Shred::run(Chuck_VM*)

WARNING: ThreadSanitizer: data race
  chuck_vm.cpp:2779 in Chuck_VM_Shreduler::shredule(Chuck_VM_Shred*, double)

WARNING: ThreadSanitizer: data race
  chuck_oo.cpp:104 in Chuck_VM_Object::add_ref()
  chuck_vm.cpp:1144 in Chuck_VM::next_id(Chuck_VM_Shred const*)

Summary: 20+ data race reports across chuck_vm.cpp, chuck_vm.cpp, chuck_oo.cpp,
chuck_instr.cpp, chuck_compile.cpp, chuck_carrier.cpp
```

Key races identified:

1. **`Chuck_VM::spork(Chuck_VM_Shred*)` at `chuck_vm.cpp:1290`** — The
   `FinalRingBuffer` write in the deferred path is not protected by any
   synchronization primitive visible to TSan. The write to the ring
   buffer's head/tail pointers races with the read in `compute()`.

2. **`Chuck_VM_Shred::run()` at `chuck_vm.cpp:2357/2360`** — Shred state
   (instr_ptr, etc.) is read/written concurrently by `run()` (Thread A)
   and object instantiation (Thread B via the deferred spork).

3. **`Chuck_VM_Shreduler::shredule()` at `chuck_vm.cpp:2779`** — Shred
   scheduling list mutations race with `compute()`.

4. **`Chuck_VM_Object::add_ref()` at `chuck_oo.cpp:104`** — Reference
   counting on `Chuck_VM_Object` uses atomic operations, but the
   surrounding object state (vtable, members) is not synchronized.

5. **`Chuck_VM::next_id()` at `chuck_vm.cpp:1144`** — Static shred ID
   counter is accessed without synchronization.

### Interpretation

The fact that **both** `immediate=FALSE` and `immediate=TRUE` produce
TSan races indicates the deferred path is **not** as thread-safe as
documented. The `FinalRingBuffer` appears lock-free but lacks proper
memory ordering guarantees for the consumer (`compute()` running on
Thread A). The shred objects created by `compileCode()` are immediately
accessed by `run()` on the VM thread, creating shared mutable state
without synchronization.

The source-level analysis from `future-chuck-integration.md` predicted
that `immediate=TRUE` would be unsafe (direct shred-list mutation).
However, **TSan shows the `immediate=FALSE` deferred path is also
unsafe** — the "lock-free" ring buffer implementation has races in
its consumption path within `compute()`.

## Decision

**NO-GO for direct concurrent use of `compileCode()` + `run()` across
threads.**

The B4-K4 architecture must use one of the following approaches:

1. **Out-of-process isolation** (preferred, matches B4-K4 design):
   Worker process compiles code in a separate `ChucK` instance, then
   atomically hands off the compiled shred to the VM process. This
   provides a hard isolation boundary — the VM process is never exposed
   to cross-thread races.

2. **Serialize compile and run within the VM process**: Call
   `compileCode()` only from the VM owner thread (Thread A), between
   `run()` calls. This eliminates concurrency but introduces latency
   in the compile path.

3. **Mutex around all compile/run operations**: Use `ChucK::lock()` /
   `ChucK::unlock()` APIs (if available) to serialize access. Check
   whether libchuck provides a lock API that protects both compile and
   compute paths — this would make the API safe within a single
   process.

### Rationale

- ASan+UBSan cannot verify thread safety (they check for memory errors,
  not data races). The "GO (with caveat)" result is misleading.
- TSan is the authoritative tool for this question, and it finds races
  in **both** spork paths.
- The races are in libchuck's core VM/shred scheduler code, not in
  our spike application code. This is a library-level issue.
- The lock-free `FinalRingBuffer` deferred path provides no safety
  guarantee when the VM is actively computing (`run()` / `compute()`).

## Recommendation

Proceed with the B4-K4 out-of-process worker design as specified in
`docs/PROGRAM.md`. The VM must run in its own process with no direct
cross-thread access from the compile worker. Use IPC (e.g., shared
memory shred handoff, or message passing) for the compile→VM boundary.

## Artifacts

- Spike source: `spikes/b4-k0-5/`
- Build configs: `spikes/b4-k0-5/CMakeLists.txt`
- Full TSan output: see build-tsan run logs
- libchuck version: `1.5.5.9-dev (chai)` from `ccrma/chuck` `main`
- Platform: macOS 15, Apple Clang 16.0.0
