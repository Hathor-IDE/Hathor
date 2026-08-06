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

set -euo pipefail

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
HATHOR_BIN="${1:-}"
SAMPLES_PATH="${2:-}"
PING_COUNT=100
MAX_MEDIAN_MS=100

if [[ -z "$HATHOR_BIN" ]]; then
    echo "[FAIL] Usage: $0 <hathor_binary> [<samples_path>]" >&2
    exit 1
fi

if [[ ! -x "$HATHOR_BIN" ]]; then
    echo "[FAIL] hathor binary not found or not executable: $HATHOR_BIN" >&2
    exit 1
fi

# Default samples path relative to this script's location (repo root/samples)
if [[ -z "$SAMPLES_PATH" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    SAMPLES_PATH="$(cd "$SCRIPT_DIR/../.." && pwd)/samples"
fi

if [[ ! -d "$SAMPLES_PATH" ]]; then
    echo "[FAIL] samples directory not found: $SAMPLES_PATH" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# JSON field extraction helper (prefers jq, falls back to grep+sed)
# ---------------------------------------------------------------------------
extract_field() {
    local json="$1"
    local field="$2"
    if command -v jq &>/dev/null; then
        echo "$json" | jq -r ".$field // empty" 2>/dev/null
    else
        # Portable fallback: extract numeric or string value after "field":
        echo "$json" | sed -n "s/.*\"${field}\":[[:space:]]*\([0-9.]*\).*/\1/p" | head -1
    fi
}

# ---------------------------------------------------------------------------
# Launch hathor and wait for the ready event
# ---------------------------------------------------------------------------
TMPDIR_WORK="$(mktemp -d)"
FIFO_IN="$TMPDIR_WORK/stdin.fifo"
FIFO_OUT="$TMPDIR_WORK/stdout.fifo"
mkfifo "$FIFO_IN"
mkfifo "$FIFO_OUT"

cleanup() {
    # Kill hathor child process if still running
    if [[ -n "${HATHOR_PID:-}" ]] && kill -0 "$HATHOR_PID" 2>/dev/null; then
        kill "$HATHOR_PID" 2>/dev/null || true
        wait "$HATHOR_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR_WORK"
}
trap cleanup EXIT

# Launch hathor with stdin/stdout connected to named pipes
"$HATHOR_BIN" --samples "$SAMPLES_PATH" < "$FIFO_IN" > "$FIFO_OUT" 2>/dev/null &
HATHOR_PID=$!

# Open the write-end of the stdin pipe (keeps it open so hathor doesn't see EOF)
exec 3>"$FIFO_IN"

# Wait for the {"event":"ready"} line (timeout = 5 seconds)
READY=0
TIMEOUT=5
START_WAIT=$SECONDS
while IFS= read -r -t 1 line 2>/dev/null <"$FIFO_OUT" || true; do
    if echo "$line" | grep -q '"event".*"ready"'; then
        READY=1
        break
    fi
    if (( SECONDS - START_WAIT >= TIMEOUT )); then
        break
    fi
done

if [[ "$READY" -ne 1 ]]; then
    echo "[FAIL] hathor did not emit {\"event\":\"ready\"} within ${TIMEOUT}s" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Send 100 ping commands and collect latency values
# ---------------------------------------------------------------------------
declare -a LATENCIES

for (( i = 1; i <= PING_COUNT; i++ )); do
    # Write ping to hathor's stdin
    echo "ping" >&3

    # Read the response line (timeout 2 s per ping)
    response=""
    if IFS= read -r -t 2 response <"$FIFO_OUT" 2>/dev/null; then
        :
    else
        echo "[FAIL] Timed out waiting for ping response #${i}" >&2
        exit 1
    fi

    # Validate ok:true
    ok_val="$(extract_field "$response" "ok")"
    if [[ "$ok_val" != "true" ]]; then
        echo "[FAIL] ping #${i} returned ok=false: $response" >&2
        exit 1
    fi

    # Extract latency_ms
    lat="$(extract_field "$response" "latency_ms")"
    if [[ -z "$lat" ]]; then
        echo "[FAIL] ping #${i} missing latency_ms field: $response" >&2
        exit 1
    fi

    LATENCIES+=("$lat")
done

# Close stdin pipe — tells hathor EOF
exec 3>&-

# ---------------------------------------------------------------------------
# Compute median of collected latencies
# ---------------------------------------------------------------------------
N=${#LATENCIES[@]}
if [[ "$N" -eq 0 ]]; then
    echo "[FAIL] No latency samples collected" >&2
    exit 1
fi

# Sort numerically and pick the median (integer arithmetic via awk)
MEDIAN=$(printf '%s\n' "${LATENCIES[@]}" | sort -n | awk -v n="$N" '
    BEGIN { mid = int(n/2) }
    NR == mid+1 { if (n % 2 == 1) { print $1; exit } else { val = $1 } }
    NR == mid+2 { print (val + $1) / 2; exit }
    NR == n && n == 1 { print $1 }
')

# Truncate to integer for comparison
MEDIAN_INT=$(echo "$MEDIAN" | awk '{print int($1)}')

echo "[INFO] Sent ${PING_COUNT} pings — median latency: ${MEDIAN_INT} ms (limit: ${MAX_MEDIAN_MS} ms)"

if (( MEDIAN_INT < MAX_MEDIAN_MS )); then
    echo "[PASS] ACP latency test passed"
    exit 0
else
    echo "[FAIL] Median latency ${MEDIAN_INT} ms >= ${MAX_MEDIAN_MS} ms limit" >&2
    exit 1
fi
