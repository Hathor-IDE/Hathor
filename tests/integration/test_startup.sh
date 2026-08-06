#!/usr/bin/env bash
# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test_startup.sh — Startup-time test (Task 15.2)
#
# Spawns "hathor --samples <path>", reads stdout until {"event":"ready"} is
# seen, measures elapsed wall time, and asserts it is < 2000 ms.
#
# Requirements: 20.5
#
# Usage:
#   ./test_startup.sh <hathor_binary> <samples_path>
#   ./test_startup.sh /path/to/hathor /path/to/samples
#
# Exit codes:
#   0  — test passed (startup < 2000 ms)
#   1  — test failed (too slow, no ready event, or binary not found)

set -euo pipefail

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
HATHOR_BIN="${1:-}"
SAMPLES_PATH="${2:-}"
MAX_STARTUP_MS=2000
TIMEOUT_S=10   # hard timeout to avoid hanging forever

if [[ -z "$HATHOR_BIN" ]]; then
    echo "[FAIL] Usage: $0 <hathor_binary> [<samples_path>]" >&2
    exit 1
fi

if [[ ! -x "$HATHOR_BIN" ]]; then
    echo "[FAIL] hathor binary not found or not executable: $HATHOR_BIN" >&2
    exit 1
fi

# Default samples path
if [[ -z "$SAMPLES_PATH" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    SAMPLES_PATH="$(cd "$SCRIPT_DIR/../.." && pwd)/samples"
fi

if [[ ! -d "$SAMPLES_PATH" ]]; then
    echo "[FAIL] samples directory not found: $SAMPLES_PATH" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Portable high-resolution timestamp (milliseconds)
# ---------------------------------------------------------------------------
now_ms() {
    if command -v python3 &>/dev/null; then
        python3 -c "import time; print(int(time.time() * 1000))"
    elif command -v perl &>/dev/null; then
        perl -MTime::HiRes=time -e 'printf "%d\n", time() * 1000'
    elif date --version 2>&1 | grep -q GNU; then
        date +%s%3N
    else
        echo $(( $(date +%s) * 1000 ))
    fi
}

# ---------------------------------------------------------------------------
# Spawn hathor and measure time to ready
# ---------------------------------------------------------------------------
TMPDIR_WORK="$(mktemp -d)"
STDOUT_FILE="$TMPDIR_WORK/stdout.txt"
touch "$STDOUT_FILE"

# Use a background 'sleep' process as stdin to keep hathor from seeing EOF.
# This is more portable than FIFO on macOS (avoids FIFO blocking-open issues).
STDIN_HOLDER_PID=""
cleanup() {
    [[ -n "${STDIN_HOLDER_PID:-}" ]] && kill "$STDIN_HOLDER_PID" 2>/dev/null || true
    [[ -n "${HATHOR_PID:-}" ]] && kill -0 "$HATHOR_PID" 2>/dev/null && {
        kill "$HATHOR_PID" 2>/dev/null || true
        wait "$HATHOR_PID" 2>/dev/null || true
    }
    rm -rf "$TMPDIR_WORK"
}
trap cleanup EXIT

# Use process substitution to provide infinite stdin without a FIFO
# (keeps stdin open; hathor blocks waiting for commands but sees no EOF)
T_START="$(now_ms)"
"$HATHOR_BIN" --samples "$SAMPLES_PATH" < <(sleep 30) > "$STDOUT_FILE" 2>/dev/null &
HATHOR_PID=$!

# Poll for the ready line, up to TIMEOUT_S seconds
FOUND_READY=0
DEADLINE=$(( $(now_ms) + TIMEOUT_S * 1000 ))
while (( $(now_ms) < DEADLINE )); do
    if grep -q '"event".*"ready"' "$STDOUT_FILE" 2>/dev/null; then
        FOUND_READY=1
        T_READY="$(now_ms)"
        break
    fi
    # Check that hathor is still alive
    if ! kill -0 "$HATHOR_PID" 2>/dev/null; then
        echo "[FAIL] hathor exited before emitting ready event" >&2
        exit 1
    fi
    sleep 0.05
done

if [[ "$FOUND_READY" -ne 1 ]]; then
    echo "[FAIL] hathor did not emit {\"event\":\"ready\"} within ${TIMEOUT_S}s" >&2
    exit 1
fi

ELAPSED_MS=$(( T_READY - T_START ))
echo "[INFO] Startup time: ${ELAPSED_MS} ms (limit: ${MAX_STARTUP_MS} ms)"

if (( ELAPSED_MS < MAX_STARTUP_MS )); then
    echo "[PASS] Startup-time test passed"
    exit 0
else
    echo "[FAIL] Startup took ${ELAPSED_MS} ms >= ${MAX_STARTUP_MS} ms limit" >&2
    exit 1
fi
