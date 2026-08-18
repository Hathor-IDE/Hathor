# Hathor

**The first agentic audio IDE.** An open-source (GPLv3), desktop-native live-coding and
music-production environment where you write music in code — and an in-editor agent writes,
repairs, and evolves that code with you.

Hathor is a from-scratch C++20 port of [TidalCycles](https://tidalcycles.org)'s pattern language
into C++, wrapped in a professional JUCE IDE. It pairs a deterministic Tidal-style pattern engine
with a layered AI authoring system, so agents and editors share **one canonical model** of the
music, the language, and the project — not a bolted-on chat window.

## What Hathor is

- **A native music IDE**, not a web editor. All UI is real JUCE components — multi-tab editor with
  split panes, command palette, navigation and workspace search, integrated terminal and task
  runner, Git source control with history and a visual graph, unified diagnostics that link into
  source, workspace/session persistence, and Hathor runtime inspection — all off the real-time audio
  thread.
- **A music engine, not a sequencer toy.** A from-scratch Tidal pattern engine with per-slot
  play/stop, out-of-process per-tab-isolated ChucK, a real EQ, and a bake-to-song renderer that
  commits auditioned audio assets and a send/return/LFO sync layer.
- **A deterministic core.** No other AI-first music tool treats the *language* as a first-class,
  versioned, auditable artifact. Hathor's Strudel/mini-notation and ChucK intelligence comes from a
  real parser + real compiler diagnostics, never hallucinated syntax.
- **An agentic workflow.** Hathor's agent plans → edits → validates → compiles → auditions →
  repairs → renders → *then* asks permission to mutate your song. Destructive steps always cross a
  confirmation gate. It is a safety-tooled, out-of-process, per-tab-isolated music agent.
- **AI with real tools.** The AI authoring system runs on the same canonical application contract as
  the UI and tests. Project intelligence, MCP tool namespaces, deterministic completion, project-aware
  context, ghost-writing, and the agentic loop all share one model.
- **Three complementary authoring layers:**

  | Layer | What it does |
  |---|---|
  | **Deterministic** | Strudel LSP, real ChucK compiler, versioned language metadata, indexing |
  | **Ghost-writing** | Cursor-style FIM inline completion with project-aware context |
  | **Agentic** | Conversational workflow: read → edit → validate → audition → repair → commit |
  | **Contextual IDE actions** | Right-click any diagnostic, selection, instrument, or Git change for deterministic + AI actions through one contract |

## Why "first agentic audio IDE"

Most music software gives you an AI prompt box bolted onto some sequencer. Hathor instead
gives the agent **real tools**: it can inspect the song, list and render assets, create and
audition ChucK instruments, read compiler diagnostics, and repair its own output — all through a
canonical contract that is shared by the UI, the tests, and the agent alike. That combination —
deterministic language authority, an auditable plan loop, and a desktop-native IDE — is what makes
Hathor the first *agentic* audio IDE rather than an AI-enabled toy.

## Directory structure

```
Hathor/
├── engine/       Pure pattern engine (static lib, no JUCE dependency)
├── app/          JUCE audio application shell (audio engine, voice pools, sample bank)
├── control/      Control plane — socket/MCP server, command interface, worker threads
├── ui/           JUCE IDE (editor, explorer, chat, panels, MCP client, activity ribbon)
├── tests/        Unit tests (Catch2, no JUCE dependency)
├── tests-ui/     UI-level tests
├── samples/      Sample bank for CI (bd/0.wav, sn/0.wav)
└── docs/         Program specification (docs/PROGRAM.md)
```

## Dependencies

All dependencies are fetched automatically via CMake FetchContent — no manual setup required.

| Library | Version | License | Fetch method |
|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 7.0.9 | GPLv3 / JUCE licence | FetchContent from GitHub |
| [Catch2](https://github.com/catchorg/Catch2) | v3.5.2 | BSL-1.0 | FetchContent from GitHub |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | MIT | FetchContent from GitHub (single-header) |

**Why FetchContent over submodules?** FetchContent keeps the repository lightweight (no submodule
init step), pins exact versions in the CMakeLists, and integrates cleanly with CMake's standard
dependency model. The trade-off is that a network connection is needed on first configure;
subsequent builds use the CMake download cache.

## CMake targets

All top-level targets below are defined unconditionally unless noted. The `hathor`,
`hathor-ui`, and `hathor-mcp` targets, and the `tests/integration` tests, are gated
behind the `HATHOR_BUILD_APP` option (default `ON`).

| Target | Type | Description | JUCE dep? |
|---|---|---|---|
| `hathor-engine` | Static library | Pure pattern engine (Tidal-style language, no JUCE) | No |
| `hathor-engine-tests` | Test executable | Engine unit tests (Catch2, golden fixtures) | No |
| `hathor` | Executable (console app) | Full audio application: audio engine, voice pool, sample bank, live-jam, control + engine linkage | Yes |
| `hathor-ui` | GUI app | Full JUCE IDE (editor, explorer, chat, panels, LSP, ghost text, Git, debugger, Petdex) — requires `HATHOR_BUILD_APP=ON` | Yes |
| `hathor-mcp` | Executable | Standalone, JUCE-free MCP server (links `hathor-engine` only) — requires `HATHOR_BUILD_APP=ON` | No |
| `hathor-ui-tests` | Test executable | Headless UI-level tests (ring buffer, file parser, tokenisers, LSP, ghost, Petdex) — no audio device | No |
| `hathor-control-tests` | Test executable | Control plane + MCP socket accept-loop tests (control interface, worker, render, agentic workflow) | No |
| `hathor-control` | Object library | JUCE-free control plane (control interface, worker thread, socket server, project facade, services) | No |
| `hathor-chuck-diagnostics` | Object library | JUCE-free ChucK source validation (links vendored `libchuck` when available) | No |
| `hathor-audio-worker-lib` | Static library | JUCE-free audio-worker manager + VM lifecyle (ChucK compile/run, watchdog, resource policy) | No |
| `hathor-audio-worker` | Executable | JUCE-free companion process spawned by the app/tests to run per-tab ChucK VMs | No |
| `acp_spike` | Executable (test) | Task 0.1 ACP transport spike — requires `HATHOR_BUILD_APP=ON` | No |

Additional `tests/` Catch2 targets (all JUCE-free): `hathor-audio-transport-tests`,
`hathor-b7-k1-filter-tests`, `hathor-b7-k2-eq-tests`, `hathor-b4-k6-event-queue-tests`,
`hathor-audio-worker-tests`, `hathor-b4-k3-vm-isolation-tests`, `hathor-b4-k5-watchdog-tests`,
`hathor-vm-lifecycle-tests`, `hathor-b4-k7-ck-eval-tests`, `hathor-b4-k7-async-compile-tests`,
`hathor-b4-k4-ckpt-compile-job-tests`, `hathor-b4-k4-execution-tests`, `hathor-b4-k8-hard-gate-tests`,
`hathor-b8-k1-asset-target-tests`, `hathor-b8-k2-render-writer-tests`, `hathor-b8-k3-vm-shutdown-tests`,
`hathor-b8-k4-sample-registration-tests`, `hathor-b8-real-audio-bake-tests`, and the standalone
verification program `hathor-b7-k4-verification`.

## Building

### Prerequisites

- CMake 3.24 or later
- A C++20-capable compiler (Clang 14+, GCC 11+, MSVC 19.29+)
- macOS 12+ or Ubuntu 22.04+ (Windows is best-effort)

### Engine tests only (no audio hardware needed)

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target hathor-engine-tests
ctest --test-dir build -V
```

### Full build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

On macOS, the build produces a **universal binary** (x86_64 + arm64) by default,
running natively on both Intel and Apple Silicon Macs.  To build for a single
architecture, set `CMAKE_OSX_ARCHITECTURES` explicitly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64
# or
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64
```

### Running Hathor

```sh
./build/app/hathor_artefacts/Release/hathor --samples ./samples
```

Send patterns via stdin:

```sh
printf 'set-pattern d1 bd sn\n' | ./build/hathor --samples ./samples
```

## Contributing

This is an active open-source project. The program and its definitions of done are maintained in
[`docs/PROGRAM.md`](docs/PROGRAM.md). Contributions are welcome — from engine work and IDE polish to
the AI authoring system. Please open an issue or PR; GPLv3 applies (see below).

## License

Copyright (C) 2026 Hathor Contributors

Hathor is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License as published by the Free Software Foundation, version 3 of the License
(see [LICENSE](LICENSE)).