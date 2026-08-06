# Hathor Integration Tests

Integration tests for the Hathor Phase 1 binary (`hathor`). Unlike the unit
tests in `tests/`, these scripts exercise the full application end-to-end:
they spawn the `hathor` process, communicate with it over stdin/stdout, and
assert on JSON responses.

## Prerequisites

- The `hathor` binary must be built (`cmake --build build --target hathor`).
- A sample bank directory must exist with at least `bd/0.wav` and `sn/0.wav`
  (the committed `samples/` directory in the repository root satisfies this).
- `bash` (≥ 3.2) is required. [`jq`](https://stedolan.github.io/jq/) is
  optional but recommended for reliable JSON parsing; the scripts fall back
  to `sed`/`grep` if `jq` is not available.
- An audio output device must be available (or a virtual device such as
  BlackHole on macOS / PulseAudio dummy sink on Linux in CI).

## Running manually

From the repository root:

```bash
# Build first
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hathor

# Run all integration tests
bash tests/integration/run_all.sh ./build/hathor ./samples

# Or run individual tests
bash tests/integration/test_startup.sh  ./build/hathor ./samples
bash tests/integration/test_latency.sh  ./build/hathor ./samples
bash tests/integration/test_protocol.sh ./build/hathor ./samples
```

Each script prints `[PASS]` / `[FAIL]` lines and exits 0 on success or 1 on
failure.

## Running via CTest

```bash
# Build everything
cmake --build build --target hathor

# Run only integration tests
ctest --test-dir build -L integration -V

# Or run the full suite (unit + integration)
ctest --test-dir build -V
```

CTest automatically passes the correct binary path via CMake generator
expressions (`$<TARGET_FILE:hathor>`), so no manual path adjustment is needed.

## Test descriptions

### `test_startup.sh` (Task 15.2 — Req 20.5)

Spawns `hathor --samples <path>`, polls stdout until
`{"event":"ready","version":"0.1.0"}` appears, measures elapsed wall time,
and asserts it is under **2000 ms**.

### `test_latency.sh` (Task 15.1 — Req 20.3)

Sends **100 `ping` commands** via a stdin pipe, collects the `latency_ms`
field from each JSON response, and asserts that the **median is under 100 ms**.

### `test_protocol.sh` (Task 15.3 — Req 12.3, 12.4, 14.4, 15.3, 16.5)

Runs a complete command sequence and verifies protocol compliance:

| # | Command | Expected |
|---|---------|----------|
| 1 | `ping` | `ok:true`, `cmd:"ping"`, `latency_ms` present |
| 2 | `bpm 140` | `ok:true`, `cmd:"bpm"`, `bpm:140` |
| 3 | `set-pattern d1 bd sn` | `ok:true`, `cmd:"set-pattern"`, `slot:"d1"` |
| 4 | `list-patterns` | `ok:true`, `cmd:"list-patterns"`, `patterns` array |
| 5 | `clear-pattern d1` | `ok:true`, `cmd:"clear-pattern"`, `slot:"d1"` |
| 6 | `clear-pattern nonexistent` | `ok:false` (slot not found) |
| 7 | `bpm 5` | `ok:false` (out of range [20, 400]) |
| 8 | `set-pattern d1 [unclosed` | `ok:false` with parse error message |
| 9 | `quit` | `ok:true`, `cmd:"quit"` then process exits |

### `run_all.sh`

Convenience wrapper that executes `test_startup.sh`, `test_latency.sh`, and
`test_protocol.sh` in sequence and prints a combined summary table.

## Adding new integration tests

1. Create a new `test_<name>.sh` file in this directory.
2. Make the script accept `<hathor_binary>` as `$1` and `<samples_path>` as
   `$2`, exit 0 on pass and non-zero on failure.
3. Register it in `CMakeLists.txt` using the `add_integration_test` macro.
4. Add it to the `TESTS` array in `run_all.sh`.
