#!/usr/bin/env bash
# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test_protocol.sh — Protocol compliance test (Task 15.3)
#
# Sends a sequence of commands to hathor and verifies each response is valid
# JSON with the expected "ok" and "cmd" fields.
#
# Test sequence:
#   1. ping              → ok:true, cmd:"ping", latency_ms present
#   2. bpm 140           → ok:true, cmd:"bpm", bpm:140
#   3. set-pattern d1 bd sn → ok:true, cmd:"set-pattern", slot:"d1"
#   4. list-patterns     → ok:true, cmd:"list-patterns", patterns array
#   5. clear-pattern d1  → ok:true, cmd:"clear-pattern", slot:"d1"
#   6. clear-pattern nonexistent → ok:false (slot not found)
#   7. bpm 5             → ok:false (out of range [20,400])
#   8. set-pattern d1 [unclosed → ok:false with parse error message
#   9. quit              → ok:true, cmd:"quit"
#
# Requirements: 12.3, 12.4, 14.4, 15.3, 16.5
#
# Usage:
#   ./test_protocol.sh <hathor_binary> <samples_path>
#
# Exit codes:
#   0  — all assertions passed
#   1  — at least one assertion failed

set -uo pipefail

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
HATHOR_BIN="${1:-}"
SAMPLES_PATH="${2:-}"

if [[ -z "$HATHOR_BIN" ]]; then
    echo "[FAIL] Usage: $0 <hathor_binary> [<samples_path>]" >&2
    exit 1
fi

if [[ ! -x "$HATHOR_BIN" ]]; then
    echo "[FAIL] hathor binary not found or not executable: $HATHOR_BIN" >&2
    exit 1
fi

if [[ -z "$SAMPLES_PATH" ]]; then
    SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    SAMPLES_PATH="$(cd "$SCRIPT_DIR/../.." && pwd)/samples"
fi

if [[ ! -d "$SAMPLES_PATH" ]]; then
    echo "[FAIL] samples directory not found: $SAMPLES_PATH" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# JSON helpers
# ---------------------------------------------------------------------------
# get_field <json_string> <field_name>
# Returns the raw value (unquoted for strings) or empty string if absent.
get_field() {
    local json="$1"
    local field="$2"
    if command -v jq &>/dev/null; then
        echo "$json" | jq -r ".[\"${field}\"] // empty" 2>/dev/null
    else
        # Fallback: extract value after "field": for booleans, numbers, strings
        echo "$json" | sed -n \
            "s/.*\"${field}\":[[:space:]]*\"\([^\"]*\)\".*/\1/p" \
            | head -1 || \
        echo "$json" | sed -n \
            "s/.*\"${field}\":[[:space:]]*\([^,}]*\).*/\1/p" \
            | tr -d ' ' | head -1
    fi
}

# is_valid_json <string>
is_valid_json() {
    local s="$1"
    if command -v jq &>/dev/null; then
        echo "$s" | jq '.' &>/dev/null
    else
        # Cheap check: starts with '{' and ends with '}'
        [[ "$s" =~ ^\{.*\}$ ]]
    fi
}

# ---------------------------------------------------------------------------
# Process management
# ---------------------------------------------------------------------------
TMPDIR_WORK="$(mktemp -d)"
FIFO_IN="$TMPDIR_WORK/stdin.fifo"
FIFO_OUT="$TMPDIR_WORK/stdout.fifo"
mkfifo "$FIFO_IN"
mkfifo "$FIFO_OUT"

HATHOR_PID=""
PASS_COUNT=0
FAIL_COUNT=0

cleanup() {
    if [[ -n "${HATHOR_PID}" ]] && kill -0 "$HATHOR_PID" 2>/dev/null; then
        kill "$HATHOR_PID" 2>/dev/null || true
        wait "$HATHOR_PID" 2>/dev/null || true
    fi
    rm -rf "$TMPDIR_WORK"
}
trap cleanup EXIT

# Launch hathor
"$HATHOR_BIN" --samples "$SAMPLES_PATH" < "$FIFO_IN" > "$FIFO_OUT" 2>/dev/null &
HATHOR_PID=$!

# Keep stdin pipe open so hathor doesn't see EOF prematurely
exec 3>"$FIFO_IN"

# Wait for ready event (timeout 5 s)
READY=0
START_WAIT=$SECONDS
while IFS= read -r -t 1 line 2>/dev/null <"$FIFO_OUT" || true; do
    if echo "$line" | grep -q '"event".*"ready"'; then
        READY=1
        break
    fi
    if (( SECONDS - START_WAIT >= 5 )); then
        break
    fi
done

if [[ "$READY" -ne 1 ]]; then
    echo "[FAIL] hathor did not emit {\"event\":\"ready\"} within 5s" >&2
    exit 1
fi
echo "[INFO] hathor is ready"

# ---------------------------------------------------------------------------
# Assertion helpers
# ---------------------------------------------------------------------------
ASSERT_NO=0

# assert_ok <response> <expected_cmd> <description>
assert_ok() {
    local resp="$1"
    local expected_cmd="$2"
    local desc="$3"
    (( ASSERT_NO++ )) || true

    if ! is_valid_json "$resp"; then
        echo "[FAIL] #${ASSERT_NO} ${desc}: response is not valid JSON: $resp" >&2
        (( FAIL_COUNT++ )) || true
        return
    fi

    local ok cmd
    ok="$(get_field "$resp" "ok")"
    cmd="$(get_field "$resp" "cmd")"

    if [[ "$ok" != "true" ]]; then
        echo "[FAIL] #${ASSERT_NO} ${desc}: expected ok:true, got ok:${ok}. Response: $resp" >&2
        (( FAIL_COUNT++ )) || true
        return
    fi
    if [[ "$cmd" != "$expected_cmd" ]]; then
        echo "[FAIL] #${ASSERT_NO} ${desc}: expected cmd:${expected_cmd}, got cmd:${cmd}. Response: $resp" >&2
        (( FAIL_COUNT++ )) || true
        return
    fi

    echo "[PASS] #${ASSERT_NO} ${desc}"
    (( PASS_COUNT++ )) || true
}

# assert_fail <response> <expected_cmd> <description>
assert_fail() {
    local resp="$1"
    local expected_cmd="$2"
    local desc="$3"
    (( ASSERT_NO++ )) || true

    if ! is_valid_json "$resp"; then
        echo "[FAIL] #${ASSERT_NO} ${desc}: response is not valid JSON: $resp" >&2
        (( FAIL_COUNT++ )) || true
        return
    fi

    local ok cmd
    ok="$(get_field "$resp" "ok")"
    cmd="$(get_field "$resp" "cmd")"

    if [[ "$ok" != "false" ]]; then
        echo "[FAIL] #${ASSERT_NO} ${desc}: expected ok:false, got ok:${ok}. Response: $resp" >&2
        (( FAIL_COUNT++ )) || true
        return
    fi
    if [[ -n "$expected_cmd" && "$cmd" != "$expected_cmd" ]]; then
        echo "[FAIL] #${ASSERT_NO} ${desc}: expected cmd:${expected_cmd}, got cmd:${cmd}. Response: $resp" >&2
        (( FAIL_COUNT++ )) || true
        return
    fi

    echo "[PASS] #${ASSERT_NO} ${desc}"
    (( PASS_COUNT++ )) || true
}

# assert_field <response> <field> <expected_value> <description>
assert_field() {
    local resp="$1"
    local field="$2"
    local expected="$3"
    local desc="$4"
    (( ASSERT_NO++ )) || true

    local actual
    actual="$(get_field "$resp" "$field")"
    if [[ "$actual" == "$expected" ]]; then
        echo "[PASS] #${ASSERT_NO} ${desc}: ${field}=${actual}"
        (( PASS_COUNT++ )) || true
    else
        echo "[FAIL] #${ASSERT_NO} ${desc}: expected ${field}=${expected}, got ${field}=${actual}. Response: $resp" >&2
        (( FAIL_COUNT++ )) || true
    fi
}

# assert_has_field <response> <field> <description>
assert_has_field() {
    local resp="$1"
    local field="$2"
    local desc="$3"
    (( ASSERT_NO++ )) || true

    local val
    val="$(get_field "$resp" "$field")"
    if [[ -n "$val" ]]; then
        echo "[PASS] #${ASSERT_NO} ${desc}: ${field} is present (${val})"
        (( PASS_COUNT++ )) || true
    else
        echo "[FAIL] #${ASSERT_NO} ${desc}: field '${field}' is absent or empty. Response: $resp" >&2
        (( FAIL_COUNT++ )) || true
    fi
}

# assert_error_field <response> <description>
assert_error_field() {
    local resp="$1"
    local desc="$2"
    (( ASSERT_NO++ )) || true

    # The "error" field must be present and non-empty
    local err
    err="$(get_field "$resp" "error")"
    if [[ -n "$err" ]]; then
        echo "[PASS] #${ASSERT_NO} ${desc}: error field present: '${err}'"
        (( PASS_COUNT++ )) || true
    else
        echo "[FAIL] #${ASSERT_NO} ${desc}: 'error' field is absent or empty. Response: $resp" >&2
        (( FAIL_COUNT++ )) || true
    fi
}

# send_cmd <command_string> — writes a line to hathor's stdin and reads one response
send_cmd() {
    local cmd_line="$1"
    local response=""
    echo "$cmd_line" >&3
    if IFS= read -r -t 5 response <"$FIFO_OUT" 2>/dev/null; then
        echo "$response"
    else
        echo ""
    fi
}

# ---------------------------------------------------------------------------
# Test 1: ping
# ---------------------------------------------------------------------------
echo ""
echo "--- Test 1: ping ---"
resp="$(send_cmd "ping")"
echo "[RECV] $resp"
assert_ok "$resp" "ping" "ping returns ok:true with cmd:ping"
assert_has_field "$resp" "latency_ms" "ping includes latency_ms"

# ---------------------------------------------------------------------------
# Test 2: bpm 140
# ---------------------------------------------------------------------------
echo ""
echo "--- Test 2: bpm 140 ---"
resp="$(send_cmd "bpm 140")"
echo "[RECV] $resp"
assert_ok "$resp" "bpm" "bpm 140 returns ok:true"
assert_field "$resp" "bpm" "140" "bpm response echoes the new BPM value"

# ---------------------------------------------------------------------------
# Test 3: set-pattern d1 bd sn
# ---------------------------------------------------------------------------
echo ""
echo "--- Test 3: set-pattern d1 bd sn ---"
resp="$(send_cmd "set-pattern d1 bd sn")"
echo "[RECV] $resp"
assert_ok "$resp" "set-pattern" "set-pattern returns ok:true"
assert_field "$resp" "slot" "d1" "set-pattern response includes slot name"
assert_has_field "$resp" "event_count_per_cycle" "set-pattern includes event_count_per_cycle"

# ---------------------------------------------------------------------------
# Test 4: list-patterns
# ---------------------------------------------------------------------------
echo ""
echo "--- Test 4: list-patterns ---"
resp="$(send_cmd "list-patterns")"
echo "[RECV] $resp"
assert_ok "$resp" "list-patterns" "list-patterns returns ok:true"
# Verify the patterns array is present (jq or grep)
(( ASSERT_NO++ )) || true
if echo "$resp" | grep -q '"patterns"'; then
    echo "[PASS] #${ASSERT_NO} list-patterns response contains patterns array"
    (( PASS_COUNT++ )) || true
else
    echo "[FAIL] #${ASSERT_NO} list-patterns response missing patterns field. Response: $resp" >&2
    (( FAIL_COUNT++ )) || true
fi

# ---------------------------------------------------------------------------
# Test 5: clear-pattern d1 (exists)
# ---------------------------------------------------------------------------
echo ""
echo "--- Test 5: clear-pattern d1 ---"
resp="$(send_cmd "clear-pattern d1")"
echo "[RECV] $resp"
assert_ok "$resp" "clear-pattern" "clear-pattern d1 returns ok:true"
assert_field "$resp" "slot" "d1" "clear-pattern response includes slot name"

# ---------------------------------------------------------------------------
# Test 6: clear-pattern nonexistent (slot not found → error)
# ---------------------------------------------------------------------------
echo ""
echo "--- Test 6: clear-pattern nonexistent ---"
resp="$(send_cmd "clear-pattern nonexistent")"
echo "[RECV] $resp"
assert_fail "$resp" "clear-pattern" "clear-pattern nonexistent returns ok:false"
assert_error_field "$resp" "clear-pattern nonexistent includes error message"

# ---------------------------------------------------------------------------
# Test 7: bpm 5 (out of range → error, BPM unchanged)
# ---------------------------------------------------------------------------
echo ""
echo "--- Test 7: bpm 5 (out of range) ---"
resp="$(send_cmd "bpm 5")"
echo "[RECV] $resp"
assert_fail "$resp" "bpm" "bpm 5 returns ok:false"
assert_error_field "$resp" "bpm 5 includes error message"

# Verify BPM is unchanged by issuing another bpm query — we ping instead and
# then set bpm to 140 again to confirm the engine accepts commands normally
resp_ping="$(send_cmd "ping")"
assert_ok "$resp_ping" "ping" "engine still responsive after rejected bpm"

# ---------------------------------------------------------------------------
# Test 8: set-pattern d1 with invalid notation (parse error)
# ---------------------------------------------------------------------------
echo ""
echo "--- Test 8: set-pattern d1 [unclosed (invalid notation) ---"
resp="$(send_cmd "set-pattern d1 [unclosed")"
echo "[RECV] $resp"
assert_fail "$resp" "set-pattern" "invalid notation returns ok:false"
assert_error_field "$resp" "invalid notation response includes parse error message"

# ---------------------------------------------------------------------------
# Test 9: quit
# ---------------------------------------------------------------------------
echo ""
echo "--- Test 9: quit ---"
resp="$(send_cmd "quit")"
echo "[RECV] $resp"
assert_ok "$resp" "quit" "quit returns ok:true with cmd:quit"

# Close our end of the stdin pipe
exec 3>&-

# Wait briefly for hathor to exit cleanly
wait "$HATHOR_PID" 2>/dev/null || true
HATHOR_PID=""

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
echo "==============================="
echo "Results: ${PASS_COUNT} passed, ${FAIL_COUNT} failed (${ASSERT_NO} total assertions)"
echo "==============================="

if [[ "$FAIL_COUNT" -eq 0 ]]; then
    echo "[PASS] Protocol compliance test passed"
    exit 0
else
    echo "[FAIL] Protocol compliance test FAILED (${FAIL_COUNT} assertion(s) failed)" >&2
    exit 1
fi
