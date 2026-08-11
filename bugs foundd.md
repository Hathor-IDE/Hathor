#  Debug Findings — Bug Investigation Handoff

## Task
Implement AI-10.4: Observable progress/explanation event stream for the agentic workflow.

## What Went Wrong

I spent 3+ hours debugging a test crash (hang in `inspectProject()` → `listSamples()`) i

## Root Cause: Vtable Layout Mismatch

### Symptom
- `test_ai10_4_workflow_events.cpp` tests hang (SIGTERM timeout, exit code 124)
- Crash/hang occurs inside `audio_.listSamples()` in `ProjectReadFacade::inspectProject()`
- `FakePlanFacade::listSamples()` override is never entered
- `listSlots()` "works" but likely calls wrong function too
- Under lldb, SIGSEGV on main thread in `basic_string` copy constructor

### Vtable Analysis

**`FakePlanFacade` vtable** (at binary address `__ZTV14FakePlanFacade`):
```
vtable[0]  = 0x0000000000000000  (offset to top)
vtable[1]  = typeinfo pointer
vtable[2]  = 0x1001a5e90  → FakePlanFacadeD1Ev  (complete object destructor)
vtable[3]  = 0x1001a5f20  → FakePlanFacadeD0Ev  (deleting destructor) ← EXTRA ENTRY
vtable[4]  = 0x1001a5f50  → play
vtable[38] = 0x1001a6990  → shutdownRender      ← should be listSamples
vtable[39] = 0x1001a69a0  → registerBakedAsset  ← should be listSlots
vtable[40] = 0x1001a69c0  → listSamples         ← actually at 40, not 38
vtable[41] = 0x1001a69f0  → listSlots           ← actually at 41, not 39
```

The deleting destructor (D0) is at vtable[3], right after the complete object destructor (D1). In the standard Itanium C++ ABI, the deleting destructor should be placed at the END of the vtable (after all virtual functions), NOT immediately after the complete object destructor. This extra entry at vtable[3] shifts ALL subsequent virtual function entries by 1 position.

### Disassembly Evidence

In `ProjectReadFacade::inspectProject()`:

```asm
; listSlots() call:
callq *0x138(%rax)    ; offset 0x138 = vtable[39] → hits registerBakedAsset (WRONG)

; listSamples() call:
movq 0x130(%rax), %rax  ; offset 0x130 = vtable[38] → hits shutdownRender (WRONG)
callq *%rax
```

The compiler generates calls using vtable indices that assume the standard Itanium ABI layout (destructor at vtable[2] only, no extra D0 entry at vtable[3]). But the actual vtable has the extra D0 entry, causing all virtual calls to be off by 1-2 positions.

### Why This Causes a Hang (Not Always a Crash)

- `listSlots()` → calls `registerBakedAsset()` which returns `bool` but caller expects `std::vector<SlotInfo>`. The return value is left uninitialized. This might "work" by coincidence (garbage looks like empty vector).
- `listSamples()` → calls `shutdownRender()` which is `void`/`noexcept` and does nothing. The expected `std::vector<std::string>` return value is never initialized, leading to garbage data and undefined behavior (hang or crash).

### Contradiction: Ai8FakeFacade Works Fine

`Ai8FakeFacade` (in `test_ai_fakes.hpp`) has the SAME vtable layout with D1/D0 at [2]/[3]. But j5 tests that use `Ai8FakeFacade` through `CompletionContextProvider` → `inspectProject()` PASS. Need to verify whether j5 tests actually exercise the `listSamples()` and `listSlots()` calls, or if they only call `inspectProject()` which may not reach those code paths.

### Compiler

```
Apple clang version 16.0.0 (clang-1600.0.26.4)
Target: x86_64-apple-darwin24.0.0
```

Build type: Debug (-g flag only, no optimizations).

## Debug Artifacts in Source Code

These `fopen("/tmp/ai104_dbg.log")` debug prints are in the ORIGINAL code (not added by me) and need cleanup:

- `control/AgenticWorkflow.cpp`: lines 231, 275, 446, 459
- `control/IntentPlanner.cpp`: lines 118, 127, 182, 594, 599
- `tests/test_ai10_4_workflow_events.cpp`: line 173 (in `waitForTerminal`)

## Files

- `control/AgenticWorkflow.hpp` — EventType, ProgressEvent, Step enums
- `control/AgenticWorkflow.cpp` — runWorkflow(), emitEvent(), getState()
- `control/IntentPlanner.cpp` — discoverReuseCandidates(), planFromRequest()
- `control/ProjectReadFacade.cpp` — inspectProject(), listSamples()
- `tests/test_ai10_4_workflow_events.cpp` — 4 test cases
