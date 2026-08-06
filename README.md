# Hathor

Hathor is an open-source (GPLv3), non-commercial, desktop-only live-coding audio application
written in C++20 using JUCE. It is a from-scratch port of the TidalCycles pattern language for
music into C++, augmented with an AI chat layer that drives the pattern engine via a
terminal/CLI interface using Zed's Agent Client Protocol (ACP).

## Project Status

Phase 1 in development — core pattern engine, JUCE-based sample playback, and CLI/ACP control
interface.

## Directory Structure

```
Hathor/
├── engine/       Pure pattern engine (static lib, no JUCE dependency)
├── app/          JUCE audio application shell
├── control/      CLI/ACP control interface
├── tests/        Unit tests (Catch2, no JUCE dependency)
└── samples/      Minimal sample bank for CI (bd/0.wav, sn/0.wav)
```

## Dependencies

All dependencies are fetched automatically via CMake FetchContent — no manual setup required.

| Library | Version | License | Fetch method |
|---|---|---|---|
| [JUCE](https://github.com/juce-framework/JUCE) | 7.0.9 | GPLv3 / JUCE licence | FetchContent from GitHub |
| [Catch2](https://github.com/catchorg/Catch2) | v3.5.2 | BSL-1.0 | FetchContent from GitHub |
| [nlohmann/json](https://github.com/nlohmann/json) | v3.11.3 | MIT | FetchContent from GitHub (single-header) |

**Why FetchContent over submodules?**  FetchContent keeps the repository lightweight (no
submodule init step), pins exact versions in the CMakeLists, and integrates cleanly with
CMake's standard dependency model. The trade-off is that a network connection is needed on
first configure; subsequent builds use the CMake download cache.

## CMake Targets

| Target | Description | JUCE dep? |
|---|---|---|
| `hathor-engine` | Pure pattern engine (static library) | No |
| `hathor-engine-tests` | Unit test runner | No |
| `hathor` | Full audio application | Yes |

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

### Running Hathor

```sh
./build/hathor --samples ./samples
```

Send patterns via stdin:

```sh
printf 'set-pattern d1 bd sn\n' | ./build/hathor --samples ./samples
```

## License

Copyright (C) 2024 Hathor Contributors

This program is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of
the License, or (at your option) any later version.

See [LICENSE](LICENSE) for the full license text.
