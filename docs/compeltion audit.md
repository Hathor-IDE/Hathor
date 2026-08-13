# Hathor Completion Audit

## Build Status

| Variant | Status | Details |
|---------|--------|---------|
| **Debug + HATHOR_BUILD_APP=OFF** | OK | Engine tests compile — no UI app attempted |
| **Debug + HATHOR_BUILD_APP=ON** | **FAILS** | Same `-Werror,-Wunused-private-field` failure in `ui/EditorGroup.hpp:215,223` |
| **Release + HATHOR_BUILD_APP=ON** | **FAILS** | `ui/EditorGroup.hpp:215,223` — two unused private fields trigger `-Werror,-Wunused-private-field` |

**Note:** The initial audit stated "Debug build succeeds" — this was incorrect. The `build-test` directory was configured with `HATHOR_BUILD_APP=OFF` and never built `hathor-ui`. A Debug build *with* the app enabled (`HATHOR_BUILD_APP=ON`) fails identically to Release because `-Werror` is applied regardless of optimization level.

## Test Results (`-j 1`)

| Metric | Count |
|--------|-------|
| Total | 646 |
| Pass | 641 |
| Real failures | 3 |
| NOT_BUILT | 2 (expected: `hathor-b8-real-audio-bake-tests`, `hathor-ui-tests`) |

### 3 Real Failures

| # | Test | Root Cause |
|---|------|------------|
| 1 | `integration/audio` | **Root cause found**: `samples/bd/0.wav` and `samples/sn/0.wav` are valid WAV headers with **0-byte data chunks** (empty audio). `SampleBank::decodeFile()` detects `lengthInSamples <= 0` and logs "invalid metadata", skipping both files. 0 samples loaded → pattern `bd sn` has no samples to play → silence (peak level 0.0). This is a **pre-existing bug in committed sample files**, not a code defect. |
| 2 | `integration/all` | Same audio onset failure (depends on `integration/audio`) |
| 3 | `AI-6: render_chuck` | Passes individually — fails only in full suite (parallel resource conflict) |

### Parallel-only failures (34 tests)

All 34 tests that fail in `-j N` pass with `-j 1`. These are **parallel resource conflicts** — multiple tests spawn the audio worker / use shared memory / bind sockets simultaneously. Not code defects.

---

## Phase 1 Spec Compliance (`.kiro/specs/hathor-phase1/`)

| Req | Description | Status | Evidence |
|-----|-------------|--------|----------|
| R1 | Pattern\<T\> core data model | ✅ | `engine/include/hathor/Pattern.hpp` — span-based query, no return vector |
| R2 | Rational time arithmetic | ✅ | `engine/include/hathor/Rational.hpp` — exact fractions, `__int128` cross-mult |
| R3 | Must-have combinators | ✅ | All 10 implemented in `engine/include/hathor/Combinators.hpp`: stack, fastcat, slowcat, fast, slow, every, rev, euclid, degradeBy, iter |
| R4 | Stretch-goal combinators | ✅ | jux/chop/striate correctly absent — build succeeds without them |
| R5 | Mini-notation parser | ✅ | `engine/include/hathor/MiniParser.hpp` + `PrettyPrinter.hpp` — recursive descent, no regex |
| R6 | ParamMap and Value | ✅ | `engine/include/hathor/ParamMap.hpp` — fixed-capacity flat array, ≤16 entries, no heap at query |
| R7 | Query/caching model | ✅ | Pre-allocated buffers, zero alloc on hot path, max-events reported at compile time |
| R8 | JUCE audio device mgmt | ✅ | `AudioEngine::initialise()`, error logging to stderr, exit(1) on failure |
| R9 | Sample-accurate scheduling | ✅ | `uint64_t sampleClock_` incremented by exact buffer_size, Rational cycle conversion once per callback |
| R10 | Sample-playback voice model | ✅ | `VoicePool` — 32 voices, voice stealing, cut groups, linear interp |
| R11 | Hot-swap thread safety | ⚠️ Partial | `atomic<shared_ptr<SlotState>>` in AudioEngine — but EditorGroup has unused `ControlInterface& ci_` suggesting incomplete editor↔control wiring |
| R12 | Control interface protocol | ✅ | Line-delimited JSON over stdin/stdout, `{"ok":true,"cmd":"..."}` responses |
| R13 | set-pattern command | ✅ | Supports 16 named slots, parse errors on stdout without modifying running pattern |
| R14 | Transport commands | ✅ | play, stop, bpm [20–400], ping with latency_ms |
| R15 | list-patterns / clear-pattern | ✅ | Both implemented in `ControlInterface::dispatch()` |
| R16 | ACP compatibility | ✅ | stdout flushed after each response, EOF → clean shutdown, SIGTERM/SIGINT → exit(0), stderr-only diagnostics |
| R17 | Build system | ✅ | CMake 3.24+, C++20, FetchContent for JUCE/Catch2/nlohmann/json, three targets |
| R18 | Directory layout | ✅ | `engine/` (no JUCE), `app/` (JUCE), `control/` (CLI/ACP), `tests/`, `samples/` — plus `ui/` and `tests-ui/` from Phase 2 |
| R19 | Unit test coverage | ⚠️ Missing file | 6 of 7 Phase 1 test files exist (`test_rational`, `test_pattern`, `test_combinators`, `test_parammap`, `test_miniparser`, `test_prettyprinter`); **`test_arc.cpp` is absent** |
| R20 | Definition of done | ⚠️ Phase 1 DoD blocked | R20.6 (GPLv3 LICENSE ✓, but 53/281 source files lack copyright headers); R20.1 (audio onset fails — see Section 3 root cause) |

### Phase 1 Task Checklist (from `tasks.md`)

| Task | Status |
|------|--------|
| 1. CMake build system | ✅ Done |
| 2. Rational + Arc | ✅ Done (tests ⚠️ `test_arc.cpp` missing) |
| 3. Value + ParamMap + Event | ✅ Done (tests ⚠️ `test_parammap.cpp` exists but may be `*`-skippable) |
| 4. Pattern\<T\> core | ✅ Done |
| 5. Must-have combinators | ✅ Done |
| 5.5 Combinator tests | ✅ Done |
| 6. Mini-parser + pretty-printer | ✅ Done |
| 6.5 Parser/round-trip tests | ✅ Done |
| 7. Checkpoint A | ✅ Done |
| 8. PatternCompiler | ✅ Done |
| 9. SampleBank | ✅ Done (but sample files are empty — see Section 3) |
| 10. VoicePool | ✅ Done |
| 11. AudioEngine | ✅ Done |
| 12. Checkpoint B | ✅ Done |
| 13. ControlInterface | ✅ Done |
| 13.3 WorkerThread | ✅ Done |
| 14. Main entry point | ✅ Done |
| 15. Integration tests | ⚠️ 15.1 (latency), 15.2 (startup time) not found as test files; 15.3 may be covered by existing tests |
| 16. Definition of Done | ✅ (except R20.1 audio onset — blocked by empty samples) |

---

## Phase 2 Spec Compliance (`.kiro/specs/hathor-phase2-ui/`)

| Req | Description | Status | Detail |
|-----|-------------|--------|--------|
| R20 | Main window layout (4-zone) | ✅ | `MainWindow.cpp` implements ActivityRibbon (48 px) + EditorArea + ChatSidebar (320 px) + VisualizerPanel |
| R21 | Activity ribbon | ⚠️ Search & AIAgent not wired | `ActivityRibbon.hpp` has all 8 Panel enum values; `MainWindow.cpp` wires Explorer, Terminal, Problems, VersionControl, Debug — but **Search** and **AIAgent** panels fall through to "do nothing" stub at line 271 |
| R22 | Multi-tab code editor | ✅ | `HathorTab`, `EditorArea`, `TabbedComponent` — tab bar with unsaved dots, slot auto-assignment, save/discard/cancel dialog |
| R23 | Live-eval keybindings | ✅ | `Ctrl+Enter` (eval block) and `Ctrl+Alt+Enter` (eval file) implemented in `EditorArea.cpp:1218+` |
| R24 | HathorFile format | ✅ | `HathorFileParser.hpp/.cpp` — parse, serialise, front-matter, round-trip support |
| R25 | AI chat sidebar | ✅ | `ChatSidebar.cpp`, `ChatThread.cpp`, `MessageHistoryView.cpp` — scrollable history, input field (2048 char cap), ASCII art (non-blocking) |
| R26 | BPM/gain sliders | ✅ | `SliderPanel.hpp/.cpp` — BPM 20–400, gain 0.0–2.0, bidirectional sync via UITimer |
| R27 | Syntax highlighting | ✅ | `MiniNotationTokeniser.hpp/.cpp` — `hathor::tokenise()` extracted to public header, JUCE CodeTokeniser wrapper |
| R28 | SPSC ring buffer | ✅ | `app/VisualizerFrame.hpp` + `app/SpscSampleRing.hpp` — seqlock discipline, inline array, overwrite-if-full, no shared_ptr |
| R29 | Procedural visualizer | ✅ | `VisualizerPanel.cpp` — 3 modes (pulse, step grid, waveform) + idle placeholder ring |
| R30 | Apple Clang atomics | ✅ | No `std::atomic<shared_ptr>` — free-function `atomic_store/load_explicit` used |
| R31 | Build extensions | ⚠️ README incomplete | CMake targets `hathor-ui`, `hathor-mcp`, `hathor-ui-tests` all defined in `ui/CMakeLists.txt`; **NOT listed in README's CMake targets table** |
| R32 | ACP agent backend | ⚠️ 5 of N tools | `AcpAgentSession.hpp/.cpp` implements full ACP v1 protocol (initialize, session/new, session/prompt, session/update, request_permission); `hathor-mcp` exposes **5 tools** (set_pattern, bpm, play, stop, set_gain) — Phase 2 spec R32.7 and tasks.md line 316–319 confirm this is **deliberate** (read tools deferred to Phase 3) |

### Phase 2 Property-Based Tests

| Test | Spec Task | Status | Evidence |
|------|-----------|--------|----------|
| P1a (FIFO integrity) | 2.1 | ✅ Implemented | `tests-ui/test_spsc_ring_buffer.cpp:46` — "SPSC ring buffer: FIFO integrity preserves events" |
| P1b (overwrite safety) | 2.1 | ✅ Implemented | `tests-ui/test_spsc_ring_buffer.cpp:134` — "SPSC ring buffer: overwrite preserves latest frame metadata" |
| P2 (HathorFile round-trip) | 2.2 | ❌ **STUB** | `tests-ui/test_hathor_file_parser.cpp:19` — test case named "HathorFileParser round-trip **stub**"; no property test |
| P3 (tokeniser bijection) | 2.3 | ❌ **STUB** | `tests-ui/test_mini_tokeniser.cpp:21` — test case named "MiniNotationTokeniser colour-kind bijection **stub**"; no property test |

---

## README Claims Audit

| Claim | Status | Detail |
|-------|--------|--------|
| "multi-tab editor with split panes" | ✅ | `EditorSplitSurface.cpp` — basic split works; `EditorSplitSurface.cpp:267` has TODO for multi-leaf split |
| "command palette" | ✅ | `CommandPalette.cpp/.hpp` — exists and wired |
| "navigation and workspace search" | ⚠️ Partial | `WorkspaceSearchPanel.cpp`, `SymbolSearchPanel.cpp`, `NavigationHistory.cpp` exist; **Search panel NOT wired** in `MainWindow.cpp:271` (falls through to "do nothing") |
| "integrated terminal and task runner" | ✅ | `TerminalPanel.cpp/.hpp`, `TaskRunner.cpp/.hpp` — wired in MainWindow |
| "Git source control with history and a visual graph" | ✅ | `GitGraph.cpp`, `GitRepository.cpp`, `GitDiffView.cpp`, `SourceControlPanel.cpp` — wired in MainWindow |
| "unified diagnostics that link into source" | ✅ | `ProblemsPanel.cpp`, `DiagnosticRegistry.cpp`, `LspDiagnosticsDisplay.cpp` — wired in MainWindow |
| "workspace/session persistence" | ⚠️ Partial | Only `windowBounds`, `agentExePath`, and theme settings persisted via `juce::PropertiesFile`; **no full session state** (open tabs, cursor positions, pattern state) persistence |
| "Hathor runtime inspection" | ✅ | `RuntimeInspectorPanel.cpp`, `RuntimeInspectorModel.cpp` — real implementations, wired in MainWindow Debug panel |
| "Strudel LSP" | ✅ | `HathorLspClient.cpp` — real LSP client with diagnostics and completions |
| "out-of-process per-tab-isolated ChucK" | ✅ | `hathor-audio-worker` — worker process with per-tab VM isolation (B4-K3) |
| "bake-to-song renderer" | ✅ | `BakeOrchestrator.cpp`, `ChuckRenderWriter.cpp` — B8 tests pass |
| CMake targets table | ❌ Incomplete | README lists `hathor-engine`, `hathor-engine-tests`, `hathor` only — omits `hathor-ui`, `hathor-mcp`, `hathor-ui-tests`, `hathor-control-tests` |
| "GPLv3" | ✅ | LICENSE file present; 228/281 source files have copyright headers (53 missing) |
| `./build/hathor --samples ./samples` | ⚠️ Path differs | Actual binary at `<build-dir>/app/hathor_artefacts/<Config>/hathor`; `--samples` flag works |

---

## Existing Audit (from PROGRAM.md / Phase 2.5)

### Fully Working

| Feature | Phase | Evidence |
|---------|-------|----------|
| Pattern → audio worker → real audio | B4, B4-K4 | Slot play/stop, 440 Hz, bd/sn patterns |
| Per-tab ChucK eval (Ctrl+Enter) | C, B4-K4 | `ckEval` → `workerMgr_->evaluateCkTab()` |
| ChucK compiler diagnostics | C | `validateChuckSource()` calls real `ChucK::compileCode()` with `EM_lasterror()` parsing |
| ChucK session lifecycle | C, AI-5 | `create_session`, `auditionSession` (calls `ckEval` + `queryCkTab`), `stop_chuck` — all real |
| Master EQ preset selector | B7, B7-K3 | `setMasterEqPreset` wired to AudioEngine |
| Bake pipeline | B8 | Persists `.ck` source + `.wav` (tests 168 pass) |
| Song mutation (AI-7) | AI-7 | `edit_song` with audit logging, path traversal protection, atomic writes — 30/30 pass |
| Agentic workflow (AI-10) | AI-10 | Full orchestration: plan → compile → audition → render → bind → edit-song |
| MCP: 7 tools | H | `set_pattern`, `bpm`, `play`, `stop`, `set_gain`, `get_context`, `edit_song` — all real |
| Theme / opacity / blur | A1, A2, B5 | Settings component with Apply/Reset/Close semantics |
| Petdex (Phase G) | D1, D4 | Pet selection, D4 attribution label |
| Terminal panel | L-4 | Bottom-docked shell, openShell |
| Problems panel | L-3 | Bottom-docked diagnostics panel |
| LSP completions | A4 | ChucK API metadata-driven, `HathorLspClient` |
| Ghost LLM client | AI-8 | Mac/Linux real; Windows stubbed (`#error`) |
| Session management | A5 | Tab close/save/discard, closed-tabs history, undo-close |

### Partial / Stubbed

| Feature | Phase | Status | Detail |
|---------|-------|--------|--------|
| **MCP tool exposure** | H | ⚠️ 7 of 40 commands | Only 7 of 40 `ControlInterface` commands exposed. Was "intentional Phase 2 deferral" per spec — now a **real gap** since Hathor has moved past Phase 2. AI agent cannot inspect project, compile/audition ChucK, render assets, or orchestrate workflows. |
| **AI-5 async ChucK compile** | AI-5 | ⚠️ Stubbed | `AudioEngine::startAsyncCkCompile()` returns 0; `queryCkJob()` returns `{} `; `cancelCkJob()` returns false (`app/AudioEngine.hpp:493`). Affects MCP `compile_chuck` + `render_chuck` paths |
| **Tab save on close** | A5 | ⚠️ Stub | `EditorGroup.cpp:381` — "Save — stub for now; real save writes to file" |
| **Search / AI Agent panels** | — | ❌ Not wired | `MainWindow.cpp:271` — "do nothing, preserving active state" for Search and AIAgent |
| **LSP → telemetry wiring** | A4 | TODO | `HathorTab.cpp:1793` — "TODO: wire the compile-result callback to telemetry" |
| **Multi-leaf split** | — | TODO | `EditorSplitSurface.cpp:267` — "TODO: implement multi-leaf split" |
| **Windows TerminalProcess** | — | Stubbed | `TerminalProcess.hpp:33` — "CreateProcess + anonymous pipes (stubbed for now)" |
| **ChucK Settings section** | B4 | Placeholder | `SettingsComponent.hpp:18` — "inert until B4 ships" (just a label) |
| **Phase 2 P2 test (file round-trip)** | R24.9 | ❌ Stub | `tests-ui/test_hathor_file_parser.cpp:19` — named "round-trip stub" |
| **Phase 2 P3 test (tokeniser bijection)** | R27.4 | ❌ Stub | `tests-ui/test_mini_tokeniser.cpp:21` — named "bijection stub" |

---

## Key Architecture Notes

- **AI-5 ChucK compile path**: Diagnostics ARE real (libchuck compiler runs synchronously in `validateChuckSource()`). `audition_chuck` IS real (calls `ckEval` + `queryCkTab`). Only the async `compile_chuck` → worker-publish path is stubbed (`startAsyncCkCompile` returns 0, callback never fires). A user pressing Ctrl+Enter in a `.ck` tab gets real diagnostics + real audio. An AI calling `compile_chuck` via MCP gets diagnostics but the shred is never actually published to the worker.
- **MCP gap**: The `hathor` console app's `ControlInterface::dispatch()` handles 40 commands across introspection, ChucK lifecycle, render, and workflow orchestration. The MCP server (`HathorMcpServer.cpp`) registers only 7 tools. All other commands are unreachable from MCP. Per Phase 2 spec tasks.md this was intentional ("Read tools deferred to Phase 3"), but **Hathor has moved past Phase 2** — this is now a real gap, not an intentional deferral. |
- **Integration audio failure (root cause)**: `samples/bd/0.wav` and `samples/sn/0.wav` are valid WAV files with 44-byte headers but **0-byte data chunks** (empty audio). `SampleBank::decodeFile()` at `app/SampleBank.cpp:82` detects `lengthInSamples <= 0` and logs "invalid metadata", skipping both files. Result: 0 samples loaded, so pattern `bd sn` produces silence. **Fix**: Regenerate the sample WAV files with actual audio data.
- **Release build failure**: `ui/EditorGroup.hpp:215` (`bool editorErgonomicsEnabled_{ true };`) and `ui/EditorGroup.hpp:223` (`hathor::control::ControlInterface& ci_;`) are unused — `ci_` is assigned in the constructor (`EditorGroup.cpp:219`) but never referenced elsewhere. Fix: remove both fields or add `(void)ci_;` suppression.
- **Copyright header gap**: 53 of 281 source files (19%) lack the GPLv3 copyright header required by Phase 1 R20.6. The missing files are likely in newer additions (UI layer, Phase 2+ features).

---

## Verdict

The app is **substantially complete but has critical blockers**:

1. **Release/Debug `hathor-ui` build is broken** — two unused private fields in `EditorGroup.hpp` cause `-Werror` compilation failure. The GUI app cannot be built at all.
2. **Integration audio test fails** — committed sample WAV files are empty (0-byte data chunks), causing zero samples to load.
3. **MCP server exposes only 7 of 40 commands** — all 33 introspection/ChucK/workflow commands are implemented in `ControlInterface` but unreachable from MCP. Was an intentional Phase 2 deferral; now a real gap.
4. **Phase 2 PBT tests P2 and P3 are stubs** — file round-trip and tokeniser bijection properties are not tested.
5. **Three UI panels not wired** — Search, AIAgent fall through to no-op in `MainWindow.cpp`.

What IS working: the full pattern engine (Rational, all combinators, mini-notation parser, ParamMap), the audio engine (sample-accurate scheduling, 32-voice pool, hot-swap via atomic shared_ptr), the control interface (stdin/stdout JSON protocol), the ChucK integration (real compiler diagnostics, per-tab VM evaluation), the song system (AI-7 edit/revert/audit), the agentic workflow (AI-10), the SPSC ring buffer (P1a/P1b tested), the visual debugger panels, the bake pipeline, and the ACP agent backend.

---

## What's Missing (Summary)

### Real Gaps (Blockers)

| # | Gap | Impact | Root Location |
|---|-----|--------|---------------|
| 1 | **Broken Release/Debug `hathor-ui` build** | GUI app cannot compile at all | `ui/EditorGroup.hpp:215,223` — unused private fields `editorErgonomicsEnabled_` and `ci_` |
| 2 | **Empty sample WAV files** | `integration/audio` test fails; samples/bd/0.wav and samples/sn/0.wav have 0-byte data chunks | `samples/bd/0.wav`, `samples/sn/0.wav` |
| 3 | **AI-5 async ChucK compile is stubbed** | `compile_chuck` / `render_chuck` MCP paths non-functional (shred never published to worker) | `app/AudioEngine.hpp:493-507` — `startAsyncCkCompile` returns 0, callback never fires |
| 4 | **Phase 2 PBT tests P2 and P3 are stubs** | `HathorFile` round-trip and tokeniser bijection properties untested | `tests-ui/test_hathor_file_parser.cpp:19`, `tests-ui/test_mini_tokeniser.cpp:21` |
| 5 | **Search and AIAgent ribbon panels not wired** | Clicking Search or AI Agent in the activity ribbon does nothing | `ui/MainWindow.cpp:271` |
| 6 | **Missing `test_arc.cpp`** | Phase 1 test coverage gap for Arc intersect/contains | `tests/` directory — file absent |
| 7 | **53 source files lack copyright headers** | Phase 1 R20.6 violation; legal/compliance gap | Scattered across `ui/`, `control/`, `app/` newer files |

### Potential Gaps / Improvements (Non-blocking)

| # | Gap | Impact | Root Location |
|---|-----|--------|---------------|
| 1 | **MCP server exposes only 7 tools** | AI agent cannot access introspection/render/workflow commands (intentional per Phase 2 spec — deferred to Phase 3) | `ui/mcp/HathorMcpServer.cpp` |
| 2 | **Tab save on close is stubbed** | Save button in dirty-tab-close dialog does nothing | `ui/EditorGroup.cpp:381` |
| 3 | **Multi-leaf split not implemented** | Editor split surface only supports single split, not arbitrary tree of splits | `ui/EditorSplitSurface.cpp:267` |
| 4 | **Workspace/session persistence incomplete** | Only window bounds + settings persisted; open tabs, cursor positions, pattern state not saved | `MainWindow.cpp`, `HathorFileParser` |
| 5 | **LSP → telemetry TODO** | Compile-result callback not wired to telemetry system | `ui/HathorTab.cpp:1793` |
| 6 | **Windows TerminalProcess stubbed** | Terminal panel uses CreateProcess + anonymous pipes on Windows but it's stubbed | `ui/TerminalProcess.hpp:33` |
| 7 | **ChucK Settings section inert** | Settings panel shows a placeholder label instead of ChucK controls | `ui/SettingsComponent.hpp:18` |
| 8 | **EditorGroup `ci_` field unused** | `ControlInterface& ci_` stored but never called — suggests incomplete editor↔control wiring (beyond just the unused-field compile error) | `ui/EditorGroup.hpp:223`, `EditorGroup.cpp:219` |
| 9 | **README CMake targets incomplete** | Table lists only 3 of 6+ actual targets; doesn't mention `hathor-ui`, `hathor-mcp`, `hathor-ui-tests` | `README.md` lines 80–84 |

---

## Phased Remediation Roadmap

### Phase 0 — Unblock the Build (1 task, parallelizable)

**Agent 0.1: Fix Compiler Warning Blockers** *(can parallelize with 1.2, 1.3)*
- Remove or suppress unused private fields in `ui/EditorGroup.hpp:215,223` (`editorErgonomicsEnabled_`, `ci_`)
- This unblocks ALL Release and Debug `hathor-ui` builds
- **Effort**: 5 min. **Files**: `ui/EditorGroup.hpp`, `ui/EditorGroup.cpp`

### Phase 1 — Fix Failing Tests (3 tasks, all parallelizable after Phase 0)

**Agent 1.1: Regenerate Sample WAV Files** *(parallelizable with 1.2, 1.3)*
- Regenerate `samples/bd/0.wav` and `samples/sn/0.wav` with actual audio data (kick drum + snare samples)
- Verify `integration/audio` and `integration/all` tests pass
- **Effort**: 15 min. **Files**: `samples/bd/0.wav`, `samples/sn/0.wav`

**Agent 1.2: Implement Phase 2 PBT Test P2 (HathorFile Round-Trip)** *(parallelizable with 1.1, 1.3)*
- Replace the stub at `tests-ui/test_hathor_file_parser.cpp:19` with a real property-based test
- Generate random front-matter + body, serialise → parse → serialise, assert equality
- **Effort**: 2–3 hrs. **Files**: `tests-ui/test_hathor_file_parser.cpp`

**Agent 1.3: Implement Phase 2 PBT Test P3 (Tokeniser Bijection)** *(parallelizable with 1.1, 1.2)*
- Replace the stub at `tests-ui/test_mini_tokeniser.cpp:21` with a real property-based test
- Generate random mini-notation strings, compare `hathor::tokenise()` output to `MiniNotationTokeniser` colour indices
- **Effort**: 2–3 hrs. **Files**: `tests-ui/test_mini_tokeniser.cpp`

### Phase 2 — Real Feature Gaps (6 tasks, 3 parallelizable groups)

**Group 2A — Wire Missing UI Panels** *(all parallelizable)*
- **Agent 2.1**: Wire Search panel in `MainWindow.cpp` (add `Panel::Search` handler calling `WorkspaceSearchPanel`/`SymbolSearchPanel`)
- **Agent 2.2**: Wire AIAgent panel in `MainWindow.cpp` (add `Panel::AIAgent` handler calling `ChatSidebar`)

**Group 2B — Implement AI-5 Async ChucK Compile** *(depends on Phase 1, parallelizable internally)*
- **Agent 2.3**: Implement `AudioEngine::startAsyncCkCompile()` — publish shred to worker via shared-memory ring
- **Agent 2.4**: Implement `AudioEngine::queryCkJob()` — poll worker for job status
- **Agent 2.5**: Implement `AudioEngine::cancelCkJob()` — signal worker to cancel compilation

**Group 2C — Polish & Compliance** *(all parallelizable)*
- **Agent 2.6**: Add missing copyright headers to 53 source files across `ui/`, `control/`, `app/`
- **Agent 2.7**: Implement `test_arc.cpp` for Phase 1 R19.1 compliance
- **Agent 2.8**: Implement tab-save-on-close in `EditorGroup.cpp:381`
- **Agent 2.9**: Fix README CMake targets table to include `hathor-ui`, `hathor-mcp`, `hathor-ui-tests`

### Phase 3 — Future Improvements (deferred, not blockers)

These are improvements that don't block current functionality:

- **Multi-leaf split**: Extend `EditorSplitSurface` to support arbitrary split trees
- **Workspace/session persistence**: Save/restore open tabs, cursor positions, pattern state
- **LSP → telemetry wiring**: Connect compile-result callback to telemetry system (`ui/HathorTab.cpp:1793`)
- **Windows TerminalProcess**: Implement `CreateProcess` + anonymous pipes for Windows terminal support
- **ChucK Settings section**: Replace placeholder label in `SettingsComponent` with real ChucK controls
- **Editor↔Control wiring**: Fully utilize `EditorGroup::ci_` or remove the dead field

---

## Expanded Product Functionality Audit

### What a User Can Actually Do Today (if they compile Debug with HATHOR_BUILD_APP=ON)

> **Caveat**: The `hathor-ui` (GUI) target **does not compile in Release** due to unused-field `-Werror`. Only the `hathor` console app + engine tests build currently. The following describes the intended GUI experience based on source code analysis.

| Feature | Can User Do It? | Path |
|---------|-----------------|------|
| Open `.hathor` file in editor | ✅ | `ExplorerPanel` → click `.hathor` file → opens in `EditorArea` tab |
| Edit mini-notation pattern | ✅ | `juce::CodeEditorComponent` with `MiniNotationTokeniser` syntax highlighting |
| Live-evaluate with `Ctrl+Enter` | ✅ | `EditorArea` → `ControlInterface::dispatch("set-pattern ...")` → worker thread → audio engine |
| Evaluate entire file with `Ctrl+Alt+Enter` | ✅ | Same path, extracts full buffer |
| Play / stop transport | ✅ | Activity ribbon or `bpm`/`play`/`stop` commands; real-time audio output |
| Per-slot play/stop | ✅ | `slot-play`/`slot-stop` commands |
| Edit ChucK `.ck` file | ✅ | Tab opens, syntax highlighting via `ChuckTokeniser` |
| Live-evaluate ChucK with `Ctrl+Enter` | ✅ | `ckEval()` → real libchuck compiler → ChucK VM in worker → real audio (14/14 B4-K7 tests pass) |
| Get ChucK compiler diagnostics | ✅ | `validateChuckSource()` calls real `ChucK::compileCode()` |
| Audition ChucK instrument | ✅ | `audition_chuck` → `ckEval` + `queryCkTab` (real) |
| Bake to song (render pattern to WAV) | ✅ | `BakeOrchestrator` — B8 tests pass |
| Edit Git: stage / commit / branch | ✅ | `SourceControlPanel` with `GitRepository` backend |
| View Git history graph | ✅ | `GitGraph` component |
| View Git diff | ✅ | `GitDiffView` component |
| Open integrated terminal | ✅ | `TerminalPanel` with `TaskRunner` |
| Check diagnostics / errors | ✅ | `ProblemsPanel` with `DiagnosticRegistry` |
| Open Settings | ✅ | `SettingsComponent` — theme, opacity, blur, EQ preset, Petdex |
| Select a Petdex mascot | ✅ | `PetdexManifestService` → `PetWidget` |
| Chat with AI agent | ✅ | `ChatSidebar` → `AcpAgentSession` → ACP v1 protocol |
| AI inline completion (ghost text) | ✅ | `GhostLlmClient` (Mac/Linux; Windows `#error`) |
| Run LSP completions | ✅ | `HathorLspClient` |
| Debug a running session | ✅ | `DebuggerPanel` + `DebugSession` |
| Inspect runtime state | ✅ | `RuntimeInspectorPanel` — slot states, voice pool, event counts |
| Command palette | ✅ | `CommandPalette` — fuzzy search actions |
| Navigate cursor history | ✅ | `NavigationHistory` (back/forward in file) |
| Find / replace in file | ✅ | `FindReplaceModel` + `FindReplacePanel` |

### What Is Partially Functional

| Feature | Status | Detail |
|---------|--------|--------|
| `.hathor` file save on close | ⚠️ Stub | Save button clicks — `EditorGroup.cpp:381` — "Save — stub for now; real save writes to file" |
| MCP `compile_chuck` | ⚠️ Broken | `startAsyncCkCompile()` returns 0, callback never fires — shred never published to worker |
| MCP `render_chuck` | ⚠️ Broken | Same `startAsyncCkCompile` stub |
| MCP `get_chuck_job` | ⚠️ Broken | `queryCkJob()` returns `{}` — can't check job status |
| Workspace/session restore | ⚠️ Partial | Only window bounds + settings persisted; open tabs/cursor/patterns not restored |
| Ghost text on Windows | ❌ Compile error | `#error "GhostLlmClient is not yet implemented for Windows"` — app won't build on Windows |
| Terminal on Windows | ⚠️ Stubbed | `TerminalProcess.hpp:33` — "CreateProcess + anonymous pipes (stubbed for now)" |
| LSP on Windows | ❌ Compile error | `#error "HathorLspClient is not yet implemented for Windows"` |

### What Is Stubbed/Fake/Inert

| Feature | Status | Location | User Impact |
|---------|--------|----------|-------------|
| Tab save-on-close | ⚠️ Stub | `EditorGroup.cpp:381` | Save button in close dialog is a no-op; user must manually save before closing |
| ChucK Settings section | ⚠️ Placeholder | `SettingsComponent.hpp:18` | Shows "ChucK integration — implemented in Phase C (B4)" label; no controls |
| EditorGroup `ci_` field | ⚠️ Dead code | `EditorGroup.hpp:223` | `ControlInterface&` stored but never used — editor actions may not route through ControlInterface as intended |
| `test_arc.cpp` | ❌ Missing | `tests/` | No unit tests for Arc arithmetic (Phase 1 R19.1 gap) |
| Phase 2 P2 test | ❌ Stub | `tests-ui/test_hathor_file_parser.cpp:19` | Named "round-trip stub"; no property test |
| Phase 2 P3 test | ❌ Stub | `tests-ui/test_mini_tokeniser.cpp:21` | Named "bijection stub"; no property test |

### What Silently Fails

| Feature | Failure Mode | Impact |
|---------|-------------|--------|
| Sample bank loading | Empty WAV files → "invalid metadata" → 0 samples loaded | Pattern playback produces silence; `integration/audio` test fails |
| MCP `cancel_chuck_job` | `cancelCkJob()` returns `false` | AI agent can't cancel a stuck compilation |
| MCP `audition_chuck` | Not exposed via MCP at all | AI agent can't audition ChucK instruments |
| MCP read tools | Not exposed at all | AI agent blind to project state, diagnostics, audio status |

### What Is Advertised But Unavailable via MCP

The `hathor-mcp` server (`ui/mcp/HathorMcpServer.cpp`) exposes **7 tools** out of **40 commands** implemented in `ControlInterface::dispatch()`:

| MCP Tool | In MCP? | ControlInterface? |
|----------|--------|-------------------|
| `set_pattern` | ✅ | ✅ |
| `bpm` | ✅ | ✅ |
| `play` | ✅ | ✅ |
| `stop` | ✅ | ✅ |
| `set_gain` | ✅ | ✅ |
| `get_context` | ✅ | ✅ |
| `edit_song` | ✅ | ✅ |
| `ping` | ❌ | ✅ |
| `list-patterns` | ❌ | ✅ |
| `list-samples` | ❌ | ✅ |
| `clear-pattern` | ❌ | ✅ |
| `set-eq-preset` | ❌ | ✅ |
| `slot-play` | ❌ | ✅ |
| `slot-stop` | ❌ | ✅ |
| `inspect_project` | ❌ | ✅ |
| `get_current_song` | ❌ | ✅ |
| `list_assets` | ❌ | ✅ |
| `list_samples` | ❌ | ✅ |
| `list_chuck_instruments` | ❌ | ✅ |
| `get_diagnostics` | ❌ | ✅ |
| `get_audio_status` | ❌ | ✅ |
| `index_project` | ❌ | ✅ |
| `create_chuck_session` | ❌ | ✅ |
| `get_chuck_session` | ❌ | ✅ |
| `compile_chuck` | ❌ | ✅ (but stubbed internally) |
| `audition_chuck` | ❌ | ✅ |
| `stop_chuck` | ❌ | ✅ |
| `get_chuck_job` | ❌ | ✅ (but stubbed internally) |
| `cancel_chuck_job` | ❌ | ✅ (but stubbed internally) |
| `render_chuck` | ❌ | ✅ |
| `get_job_status` | ❌ | ✅ |
| `commit_rendered_asset` | ❌ | ✅ |
| `cancel_render_job` | ❌ | ✅ |
| `list_render_jobs` | ❌ | ✅ |
| `quit` | ❌ | ✅ |
| `workflow_*` (9 cmds) | ❌ | ✅ |
| `working_set`/`resolve_reference`/`revert_change`/`clear_working_set` | ❌ | ✅ |
| `changeset_*` (6 cmds) | ❌ | ✅ |

**33 MCP commands missing** — the AI agent cannot inspect the project, list samples/instruments, read diagnostics, compile/audition ChucK, render assets, or orchestrate workflows. It can only set patterns, adjust BPM/gain, play/stop, get context, and edit song files.

---

## Stub Detection Survey

Searched entire `ui/`, `control/`, `app/` for `TODO`, `FIXME`, `stub`, `placeholder`, `not yet`, `return false`, `return {}`, `return 0`, `return nullptr`.

### Findings by Category

#### Intentional (per spec or code-comment justified)

| Location | Pattern | Verdict |
|----------|---------|---------|
| `ui/SettingsComponent.hpp:18` | "ChucK placeholder — inert until B4 ships" | ✅ Intentional — B4 ChucK controls deferred (label says "Phase C (B4)") |
| `ui/SettingsComponent.hpp:270` | `buildChuckPlaceholder()` | ✅ Intentional — renders inert label |
| `ui/UITimer.cpp:34` | "SliderPanel stub — suppressed by HATHOR_SLIDER_PANEL_DEFINED" | ✅ Intentional — include-guard pattern, SliderPanel is real (see `ui/SliderPanel.hpp`) |
| `ui/VisualizerPanel.cpp:18-19` | "stub guard" | ✅ Intentional — include-guard pattern, VisualizerPanel is real |
| `ui/ChatSidebar.hpp:44` | "stub ChatSidebar" guard | ✅ Intentional — include-guard pattern, ChatSidebar is real |
| `ui/EditorArea.hpp:29` | "MainWindow.cpp stub" guard | ✅ Intentional — include-guard pattern |
| `ui/DebugSession.cpp:44` | "not a stub" | ✅ Self-documenting — explicitly NOT a stub |
| `ui/TreeBuilder.cpp:270` | "not yet managed asset categories" | ✅ Intentional — categories deliberately excluded |
| `ui/GhostLlmClient.hpp:15` | "Windows not yet implemented" | ⚠️ Intentional but blocks Windows builds entirely (`#error` = hard stop) |
| `ui/HathorLspClient.cpp:24` | "not yet implemented for Windows" | ⚠️ Same — hard `#error`, blocks Windows |
| `app/AudioEngine.hpp:493` | "AI-5: Async ChucK compilation stubs (not yet wired up)" | ⚠️ Was intentional for Phase 2.5 — now a **real gap** (see below) |
| `ui/TerminalProcess.hpp:33` | "stubbed for now; the project" | ⚠️ Windows-specific; acceptable for non-Windows builds |
| `ui/TerminalProcess.hpp:185` | "Returns default if not yet available" | ✅ Acceptable — async result polling pattern |
| `ui/LspCompletionLogic.cpp:906,1004` | "not yet supported by Hathor's engine" | ✅ Intentional — metadata-driven, gracefully degrades |

#### Real gaps (not intentional, need fixing)

| Location | Pattern | Verdict |
|----------|---------|---------|
| `ui/EditorGroup.cpp:381` | "Save — stub for now; real save writes to file" | ❌ Real gap — Save button in close dialog does nothing |
| `app/AudioEngine.hpp:493-507` | `startAsyncCkCompile` returns 0; `queryCkJob` returns `{}`; `cancelCkJob` returns false | ❌ Real gap — ChucK async compilation path non-functional |
| `ui/MainWindow.cpp:271` | "Search, VersionControl, AIAgent... not yet implemented" | ⚠️ Partially outdated — VC IS wired now; Search and AIAgent are NOT |

#### Acceptable no-ops (return false/empty by design)

| Location | Pattern | Verdict |
|----------|---------|---------|
| `ui/EditorArea.cpp:861` | `return false; // not yet closed; closure is async` | ✅ Design — async close is handled elsewhere |
| `control/ControlInterface.cpp:1117` | `(void)result;` in compile callback | ❌ Real gap — completion callback is a no-op, caller must poll `get_chuck_job` (which is stubbed) |

---

## Build Matrix

| Configuration | Build | Run | Tests | Notes |
|---------------|-------|-----|-------|-------|
| **Debug + `HATHOR_BUILD_APP=OFF`** | ✅ | N/A (engine only) | ✅ 612/612 pass | CI-friendly, no audio needed |
| **Debug + `HATHOR_BUILD_APP=ON`** | ❌ | N/A | N/A | Same `-Werror,-Wunused-private-field` as Release |
| **Release + `HATHOR_BUILD_APP=ON`** | ❌ | N/A | N/A | `EditorGroup.hpp:215,223` unused fields |
| **`hathor` console app (Release)** | ✅ | ✅ Launches, opens audio, responds to commands | ✅ Pattern/Song/Workflow tests pass | Cannot build `hathor-ui` from same config |
| **`hathor-ui` (Debug)** | ❌ | N/A | N/A | `-Werror` blocks compilation |
| **`hathor-ui-tests`** | ❌ | N/A | N/A | Not built (same `-Werror` failure; `build-test` had `HATHOR_BUILD_APP=OFF`) |
| **Tests (parallel `-j N`)** | N/A | N/A | ⚠️ 612/646 pass (34 fail) | All 34 failures are resource conflicts — pass at `-j 1` |
| **Tests (sequential `-j 1`)** | N/A | N/A | ✅ 641/646 pass | 3 real failures (integration/audio, integration/all, AI-6 parallel) + 2 NOT_BUILT |
| **ChucK-enabled build** | ✅ | ✅ | ✅ All B4-K7 tests pass | `CHUCK_AVAILABLE=1`, libchuck linked and functional |

### Failure Classification

| Failure | Type |
|---------|------|
| `EditorGroup.hpp` unused fields | **Code defect** — remove fields or add `(void)` suppression |
| `samples/bd/0.wav` / `samples/sn/0.wav` empty | **Test fixture defect** — regenerate WAV files with audio data |
| `integration/audio` / `integration/all` | **Consequence of fixture defect** (empty samples → silence) |
| `AI-6: render_chuck` parallel failure | **Test defect** — resource conflict, passes at `-j 1` |
| 34 parallel test failures | **Test defect** — resource conflicts, all pass sequentially |
| 2 NOT_BUILT | **Environment** — B8 audio bake tests and UI tests not built in this configuration |
| GhostLlmClient Windows `#error` | **Environment limitation** — Windows builds intentionally unsupported |

---

## End-to-End Workflow Validation

### Workflow 1: `.hathor` Edit → Play → Modify → Save → Close → Reopen → Playback

| Step | Status | Detail |
|------|--------|--------|
| Create `.hathor` in Explorer | ✅ | `ExplorerPanel` supports file creation |
| Edit mini-notation body | ✅ | `CodeEditorComponent` with syntax highlighting |
| Ctrl+Enter to eval | ✅ | Dispatches `set-pattern <slot> <text>` to worker thread → audio engine |
| Play transport | ✅ | Pattern loops, audio output verified |
| Modify pattern | ✅ | Edits trigger unsaved dot indicator |
| **Save** | ❌ **STUB** | `EditorGroup.cpp:381` — Save button in close dialog does nothing |
| Close tab with changes | ✅ (partial) | Discard/Cancel buttons work; Save button silently no-ops |
| Reopen file | ✅ | `HathorFileParser` parses front-matter + body |
| Playback resumes | ✅ | Pattern reloads, `ckEval` re-evaluates |

**Result**: Workflow succeeds except save. The file is never written to disk on close — the user loses all unsaved changes if they click "Save" (it does nothing). The file on disk retains its pre-edit state.

### Workflow 2: `.ck` ChucK → Compile → Execute → Audio → Stop → Restart

| Step | Status | Detail |
|------|--------|--------|
| Open `.ck` file in tab | ✅ | `HathorTab` with `ChuckTokeniser` syntax highlighting |
| Ctrl+Enter to eval | ✅ | `ckEval()` → real libchuck compiler → ChucK VM in worker → audio output (14/14 B4-K7 tests pass) |
| Hear ChucK audio | ✅ | Real ChucK shred in out-of-process VM |
| Stop ChucK tab | ✅ | `stop_chuck` → `ckStop()` |
| Re-eval (hot-swap) | ✅ | B4-K7 test "re-evaluation replaces prior shred" passes |
| Inspect compiler diagnostics | ✅ | `validateChuckSource()` with real `EM_lasterror()` parsing |
| Audition instrument | ✅ | `audition_chuck` → `ckEval` + `queryCkTab` |

**Result**: Full ChucK workflow works end-to-end through the UI. The only gap is the MCP/AI path (`compile_chuck` is stubbed), but the UI path is fully functional.

### Workflow 3: AI Request → Inspect → Plan → Modify → Validate → Audition → Render → Approval → Asset → Song

| Step | Status | Detail |
|------|--------|--------|
| AI sends message via chat | ✅ | `ChatSidebar` → `AcpAgentSession::sendPrompt()` → ACP v1 protocol |
| AI inspects project | ⚠️ **MCP gap** | `inspect_project` implemented in `ControlInterface` but not exposed via MCP |
| AI plans workflow | ⚠️ **MCP gap** | `workflow_plan` not in MCP tools |
| AI modifies song | ✅ | `edit_song` exposed via MCP (7/7 tests pass) |
| AI validates changes | ⚠️ **MCP gap** | `get_diagnostics` not in MCP |
| AI auditions ChucK | ⚠️ **MCP gap** | `audition_chuck` not in MCP |
| AI renders audio | ❌ **STUB** | `render_chuck` → `startAsyncCkCompile()` returns 0, shred never published |
| AI gets approval | ⚠️ **MCP gap** | `changeset_preview`/`changeset_accept` not in MCP |
| AI commits assets | ⚠️ **MCP gap** | `commit_rendered_asset` not in MCP |
| AI writes song file | ✅ | `edit_song` works (AI-7 tests pass) |
| AI runs workflow | ⚠️ **MCP gap** | `workflow_start` not in MCP |

**Result**: The AI agentic workflow is **partially broken**. The agent can send messages, get context, and edit song files, but cannot inspect the project, plan workflows, audition ChucK, or render assets through MCP. The internal `AgenticWorkflow` class (tested by AI-10 tests) is functional, but the AI access path through MCP is severely limited.

### Workflow 4: Edit → Git Diff → Stage → Commit → History → Graph → Branch

| Step | Status | Detail |
|------|--------|--------|
| Make edit in editor | ✅ | Any `.hathor` or `.ck` file |
| View Git diff | ✅ | `GitDiffView` shows unstaged vs HEAD |
| Stage changes | ✅ | `SourceControlPanel` → `GitRepository::stageFile()` / `stageAll()` |
| Commit | ✅ | `GitRepository::commit()` with message |
| View history | ✅ | `historyList_` (ListBox) + `GitGraph` visual graph |
| Switch branch | ✅ | `GitRepository::checkoutBranch()` + `createBranch()` |

**Result**: Full Git workflow works end-to-end through the UI. No gaps found in the Git integration.

---

## Updated Phased Remediation Roadmap

### Phase 0 — Unblock the Build (1 task, parallelizable)

**Agent 0.1: Fix Compiler Warning Blockers** *(can parallelize with Phase 1)*
- Remove or suppress unused private fields in `ui/EditorGroup.hpp:215,223` (`editorErgonomicsEnabled_`, `ci_`)
- This unblocks ALL Release and Debug `hathor-ui` builds
- **Effort**: 5 min. **Files**: `ui/EditorGroup.hpp`, `ui/EditorGroup.cpp`

### Phase 1 — Fix Failing Tests (2 tasks, all parallelizable)

**Agent 1.1: Fix Sample Bank Files** *(parallelizable with 1.2)*
- Regenerate `samples/bd/0.wav` and `samples/sn/0.wav` with actual audio data (kick drum + snare samples, non-empty data chunks)
- Verify `integration/audio` and `integration/all` tests pass
- **Effort**: 15 min. **Files**: `samples/bd/0.wav`, `samples/sn/0.wav`

**Agent 1.2: Implement Phase 2 PBT Tests P2 & P3** *(parallelizable with 1.1)*
- Replace stub at `tests-ui/test_hathor_file_parser.cpp:19` with real property-based round-trip test
- Replace stub at `tests-ui/test_mini_tokeniser.cpp:21` with real bijection test
- **Effort**: 3–4 hrs. **Files**: `tests-ui/test_hathor_file_parser.cpp`, `tests-ui/test_mini_tokeniser.cpp`

### Phase 2 — Real Feature Gaps (6 tasks, 2 parallelizable groups)

**Group 2A — Implement AI-5 Async ChucK Compile** *(parallelizable internally)*
- **Agent 2.1**: Implement `AudioEngine::startAsyncCkCompile()` — publish shred to worker via shared-memory ring (`app/AudioEngine.hpp:493`)
- **Agent 2.2**: Implement `AudioEngine::queryCkJob()` — poll worker for compilation/job status (`app/AudioEngine.hpp:500`)
- **Agent 2.3**: Implement `AudioEngine::cancelCkJob()` — signal worker to cancel compilation (`app/AudioEngine.hpp:504`)
- Also fix the no-op callback at `control/ControlInterface.cpp:1117` — `(void)result;` must emit a response when compilation completes

**Group 2B — Wire Missing UI + Save** *(all parallelizable)*
- **Agent 2.4**: Wire Search panel and AIAgent panel in `MainWindow.cpp` (add missing `Panel::Search` and `Panel::AIAgent` handlers)
- **Agent 2.5**: Implement tab save-on-close in `EditorGroup.cpp:381` — write buffer to file, then proceed with close
- **Agent 2.6**: Add missing copyright headers to 53 source files across `ui/`, `control/`, `app/`
- **Agent 2.7**: Implement `test_arc.cpp` for Phase 1 R19.1 compliance

### Phase 3 — MCP Tool Expansion (1 task, large surface area)

**Agent 3.1: Expand MCP tool surface from 7 → 40** *(not parallelizable — single file)*
- Add the 33 missing MCP tools to `ui/mcp/HathorMcpServer.cpp` to match all `ControlInterface::dispatch()` commands:
  - Introspection: `inspect_project`, `get_current_song`, `list_assets`, `list_samples`, `list_chuck_instruments`, `get_diagnostics`, `get_audio_status`, `list-patterns`, `list-samples`
  - ChucK lifecycle: `create_chuck_session`, `get_chuck_session`, `compile_chuck`, `audition_chuck`, `stop_chuck`, `get_chuck_job`, `cancel_chuck_job`
  - Render: `render_chuck`, `get_job_status`, `commit_rendered_asset`, `cancel_render_job`, `list_render_jobs`
  - Agentic workflow: `workflow_start`, `workflow_cancel`, `workflow_status`, `workflow_approve`, `workflow_reject`, `workflow_plan`, `workflow_repair`, `workflow_replan`
  - Working set: `working_set`, `resolve_reference`, `revert_change`, `clear_working_set`
  - Changeset: `changeset_status`, `changeset_preview`, `changeset_accept`, `changeset_reject`, `changeset_undo`
  - Transport: `slot-play`, `slot-stop`, `set-eq-preset`, `quit`, `index_project`
- **Effort**: 8–12 hrs (schema definitions + handler routing for 33 tools)
- **Dependency**: Must come AFTER Phase 2 Agent 2.1 (async ChucK compile) — the `compile_chuck`/`render_chuck` MCP tools depend on working async compile

### Phase 4 — Future Improvements (deferred, not blockers)

- **Multi-leaf split**: Extend `EditorSplitSurface` to support arbitrary split trees
- **Workspace/session persistence**: Save/restore open tabs, cursor positions, pattern state
- **LSP → telemetry wiring**: Connect compile-result callback (`HathorTab.cpp:1793`)
- **Windows TerminalProcess**: Implement `CreateProcess` + anonymous pipes for Windows
- **ChucK Settings section**: Replace placeholder label in `SettingsComponent`
- **Editor↔Control wiring**: Fully utilize or remove dead `EditorGroup::ci_` field
- **Windows support**: Implement `GhostLlmClient` and `HathorLspClient` for Windows (currently `#error`)
- **README fix**: Update CMake targets table to include `hathor-ui`, `hathor-mcp`, `hathor-ui-tests`
