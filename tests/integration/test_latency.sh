#!/usr/bin/env bash
# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test_latency.sh — ACP latency test (Task 15.1)
#
# Sends 100 "ping" commands to hathor via stdin pipe, collects latency_ms
# values from JSON responses, and asserts that the median is < 100 ms.
#
# Requirements: 20.3
#
# Usage:
#   ./test_latency.sh <hathor_binary> <samples_path>
#   ./test_latency.sh /path/to/hathor /path/to/samples
#
# Exit codes:
#   0  — test passed (median latency < 100 ms)
#   1  — test failed (median too high, parse error, or binary not found)

set -eu

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
HATHOR_BIN="${1:-}"
SAMPLES_PATH="${2:-}"
PING_COUNT=100
MAX_MEDIAN_MS=100

if [ -z "$HATHOR_BIN" ]; then
    echo "[FAIL] Usage: $0 <hathor_binary> [<samples_path>]" >&2
    exit 1
fi

if [ ! -x "$HATHOR_BIN" ]; then
    echo "[FAIL] hathor binary not found or not executable: $HATHOR_BIN" >&2
    exit 1
fi

# Default samples path relative to this script's location (repo root/samples)
if [ -z "$SAMPLES_PATH" ]; then
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    SAMPLES_PATH="$(cd "$SCRIPT_DIR/../.." && pwd)/samples"
fi

if [ ! -d "$SAMPLES_PATH" ]; then
    echo "[FAIL] samples directory not found: $SAMPLES_PATH" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Use Python for reliable bidirectional subprocess I/O
# ---------------------------------------------------------------------------
python3 - "$HATHOR_BIN" "$SAMPLES_PATH" "$PING_COUNT" "$MAX_MEDIAN_MS" <<'PYEOF'
import sys, subprocess, json, time, os

hathor_bin   = sys.argv[1]
samples_path = sys.argv[2]
ping_count   = int(sys.argv[3])
max_median   = int(sys.argv[4])

proc = subprocess.Popen(
    [hathor_bin, "--samples", samples_path],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
)
os.set_blocking(proc.stdout.fileno(), False)

def readline_timeout(proc, timeout=5.0):
    """Read a line from proc.stdout with a wall-clock timeout (non-blocking)."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        try:
            ch = proc.stdout.read(1)
            if ch is None:
                time.sleep(0.001)
                continue
            if not ch:
                break
            buf += ch
            if ch == b"\n":
                break
        except BlockingIOError:
            time.sleep(0.001)
    return buf.decode(errors="replace").rstrip("\n")

# Wait for ready event
ready_line = readline_timeout(proc, 5.0)
try:
    ev = json.loads(ready_line)
    if ev.get("event") != "ready":
        print(f"[FAIL] Unexpected first line: {ready_line}", file=sys.stderr)
        proc.kill()
        sys.exit(1)
except json.JSONDecodeError:
    print(f"[FAIL] Could not parse ready event: {ready_line!r}", file=sys.stderr)
    proc.kill()
    sys.exit(1)

# Send ping_count pings and collect latency_ms values
latencies = []
for i in range(1, ping_count + 1):
    proc.stdin.write(b"ping\n")
    proc.stdin.flush()
    resp_line = readline_timeout(proc, 2.0)
    if not resp_line:
        print(f"[FAIL] Timed out waiting for ping response #{i}", file=sys.stderr)
        proc.kill()
        sys.exit(1)
    try:
        resp = json.loads(resp_line)
    except json.JSONDecodeError:
        print(f"[FAIL] ping #{i} response is not valid JSON: {resp_line!r}", file=sys.stderr)
        proc.kill()
        sys.exit(1)
    if resp.get("ok") is not True:
        print(f"[FAIL] ping #{i} returned ok=false: {resp_line}", file=sys.stderr)
        proc.kill()
        sys.exit(1)
    lat = resp.get("latency_ms")
    if lat is None:
        print(f"[FAIL] ping #{i} missing latency_ms field: {resp_line}", file=sys.stderr)
        proc.kill()
        sys.exit(1)
    latencies.append(float(lat))

# Send quit
proc.stdin.write(b"quit\n")
proc.stdin.flush()
try:
    proc.wait(timeout=3)
except subprocess.TimeoutExpired:
    proc.kill()

# Compute median
latencies.sort()
n = len(latencies)
if n % 2 == 1:
    median = latencies[n // 2]
else:
    median = (latencies[n // 2 - 1] + latencies[n // 2]) / 2.0

median_int = int(median)
print(f"[INFO] Sent {ping_count} pings — median latency: {median_int} ms (limit: {max_median} ms)")

if median_int < max_median:
    print("[PASS] ACP latency test passed")
    sys.exit(0)
else:
    print(f"[FAIL] Median latency {median_int} ms >= {max_median} ms limit", file=sys.stderr)
    sys.exit(1)
PYEOF
