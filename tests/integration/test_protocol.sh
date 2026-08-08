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
#   9. set-pattern d1 bd sn ; set-pattern d2 cp hh ; slot-stop d1 → ok:true slot:"d1"
#  10. slot-play d1      → ok:true, cmd:"slot-play", slot:"d1"
#  11. slot-stop         → ok:false (missing slot name)
#  12. quit              → ok:true, cmd:"quit"
#
# Requirements: 12.3, 12.4, 14.4, 15.3, 16.5
#
# Usage:
#   ./test_protocol.sh <hathor_binary> <samples_path>
#
# Exit codes:
#   0  — all assertions passed
#   1  — at least one assertion failed

set -eu

# ---------------------------------------------------------------------------
# Arguments
# ---------------------------------------------------------------------------
HATHOR_BIN="${1:-}"
SAMPLES_PATH="${2:-}"

if [ -z "$HATHOR_BIN" ]; then
    echo "[FAIL] Usage: $0 <hathor_binary> [<samples_path>]" >&2
    exit 1
fi

if [ ! -x "$HATHOR_BIN" ]; then
    echo "[FAIL] hathor binary not found or not executable: $HATHOR_BIN" >&2
    exit 1
fi

if [ -z "$SAMPLES_PATH" ]; then
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    SAMPLES_PATH="$(cd "$SCRIPT_DIR/../.." && pwd)/samples"
fi

if [ ! -d "$SAMPLES_PATH" ]; then
    echo "[FAIL] samples directory not found: $SAMPLES_PATH" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Use Python for reliable bidirectional subprocess I/O (bash 3.2 compatible)
# ---------------------------------------------------------------------------
python3 - "$HATHOR_BIN" "$SAMPLES_PATH" <<'PYEOF'
import sys, subprocess, json, time, os

hathor_bin   = sys.argv[1]
samples_path = sys.argv[2]

proc = subprocess.Popen(
    [hathor_bin, "--samples", samples_path],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
)
os.set_blocking(proc.stdout.fileno(), False)

pass_count = 0
fail_count = 0
assert_no  = 0

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

def send_cmd(cmd_line, timeout=5.0):
    proc.stdin.write((cmd_line + "\n").encode())
    proc.stdin.flush()
    return readline_timeout(proc, timeout)

def get_field(resp, field):
    try:
        obj = json.loads(resp)
        val = obj.get(field)
        if val is None:
            return ""
        if isinstance(val, bool):
            return "true" if val else "false"
        # Represent numeric values without trailing .0 when they are integers
        if isinstance(val, float) and val == int(val):
            return str(int(val))
        return str(val)
    except Exception:
        return ""

def is_valid_json(s):
    try:
        json.loads(s)
        return True
    except Exception:
        return False

def assert_ok(resp, expected_cmd, desc):
    global pass_count, fail_count, assert_no
    assert_no += 1
    if not is_valid_json(resp):
        print(f"[FAIL] #{assert_no} {desc}: response is not valid JSON: {resp}", file=sys.stderr)
        fail_count += 1
        return
    ok  = get_field(resp, "ok")
    cmd = get_field(resp, "cmd")
    if ok != "true":
        print(f"[FAIL] #{assert_no} {desc}: expected ok:true, got ok:{ok}. Response: {resp}", file=sys.stderr)
        fail_count += 1
        return
    if cmd != expected_cmd:
        print(f"[FAIL] #{assert_no} {desc}: expected cmd:{expected_cmd}, got cmd:{cmd}. Response: {resp}", file=sys.stderr)
        fail_count += 1
        return
    print(f"[PASS] #{assert_no} {desc}")
    pass_count += 1

def assert_fail(resp, expected_cmd, desc):
    global pass_count, fail_count, assert_no
    assert_no += 1
    if not is_valid_json(resp):
        print(f"[FAIL] #{assert_no} {desc}: response is not valid JSON: {resp}", file=sys.stderr)
        fail_count += 1
        return
    ok  = get_field(resp, "ok")
    cmd = get_field(resp, "cmd")
    if ok != "false":
        print(f"[FAIL] #{assert_no} {desc}: expected ok:false, got ok:{ok}. Response: {resp}", file=sys.stderr)
        fail_count += 1
        return
    if expected_cmd and cmd != expected_cmd:
        print(f"[FAIL] #{assert_no} {desc}: expected cmd:{expected_cmd}, got cmd:{cmd}. Response: {resp}", file=sys.stderr)
        fail_count += 1
        return
    print(f"[PASS] #{assert_no} {desc}")
    pass_count += 1

def assert_field(resp, field, expected, desc):
    global pass_count, fail_count, assert_no
    assert_no += 1
    actual = get_field(resp, field)
    if actual == expected:
        print(f"[PASS] #{assert_no} {desc}: {field}={actual}")
        pass_count += 1
    else:
        print(f"[FAIL] #{assert_no} {desc}: expected {field}={expected}, got {field}={actual}. Response: {resp}", file=sys.stderr)
        fail_count += 1

def assert_has_field(resp, field, desc):
    global pass_count, fail_count, assert_no
    assert_no += 1
    val = get_field(resp, field)
    if val:
        print(f"[PASS] #{assert_no} {desc}: {field} is present ({val})")
        pass_count += 1
    else:
        print(f"[FAIL] #{assert_no} {desc}: field '{field}' is absent or empty. Response: {resp}", file=sys.stderr)
        fail_count += 1

def assert_error_field(resp, desc):
    global pass_count, fail_count, assert_no
    assert_no += 1
    err = get_field(resp, "error")
    if err:
        print(f"[PASS] #{assert_no} {desc}: error field present: '{err}'")
        pass_count += 1
    else:
        print(f"[FAIL] #{assert_no} {desc}: 'error' field is absent or empty. Response: {resp}", file=sys.stderr)
        fail_count += 1

# Wait for ready event
ready_line = readline_timeout(proc, 5.0)
try:
    ev = json.loads(ready_line)
    if ev.get("event") != "ready":
        print(f"[FAIL] hathor did not emit ready event. Got: {ready_line!r}", file=sys.stderr)
        proc.kill()
        sys.exit(1)
except json.JSONDecodeError:
    print(f"[FAIL] hathor did not emit {{\"event\":\"ready\"}} within 5s. Got: {ready_line!r}", file=sys.stderr)
    proc.kill()
    sys.exit(1)
print("[INFO] hathor is ready")

# --- Test 1: ping ---
print("\n--- Test 1: ping ---")
resp = send_cmd("ping")
print(f"[RECV] {resp}")
assert_ok(resp, "ping", "ping returns ok:true with cmd:ping")
assert_has_field(resp, "latency_ms", "ping includes latency_ms")

# --- Test 2: bpm 140 ---
print("\n--- Test 2: bpm 140 ---")
resp = send_cmd("bpm 140")
print(f"[RECV] {resp}")
assert_ok(resp, "bpm", "bpm 140 returns ok:true")
assert_field(resp, "bpm", "140", "bpm response echoes the new BPM value")

# --- Test 3: set-pattern d1 bd sn ---
print("\n--- Test 3: set-pattern d1 bd sn ---")
resp = send_cmd("set-pattern d1 bd sn", timeout=10.0)
print(f"[RECV] {resp}")
assert_ok(resp, "set-pattern", "set-pattern returns ok:true")
assert_field(resp, "slot", "d1", "set-pattern response includes slot name")
assert_has_field(resp, "event_count_per_cycle", "set-pattern includes event_count_per_cycle")

# --- Test 4: list-patterns ---
print("\n--- Test 4: list-patterns ---")
resp = send_cmd("list-patterns")
print(f"[RECV] {resp}")
assert_ok(resp, "list-patterns", "list-patterns returns ok:true")
assert_no += 1
if '"patterns"' in resp:
    print(f"[PASS] #{assert_no} list-patterns response contains patterns array")
    pass_count += 1
else:
    print(f"[FAIL] #{assert_no} list-patterns response missing patterns field. Response: {resp}", file=sys.stderr)
    fail_count += 1

# --- Test 5: clear-pattern d1 ---
print("\n--- Test 5: clear-pattern d1 ---")
resp = send_cmd("clear-pattern d1")
print(f"[RECV] {resp}")
assert_ok(resp, "clear-pattern", "clear-pattern d1 returns ok:true")
assert_field(resp, "slot", "d1", "clear-pattern response includes slot name")

# --- Test 6: clear-pattern nonexistent ---
print("\n--- Test 6: clear-pattern nonexistent ---")
resp = send_cmd("clear-pattern nonexistent")
print(f"[RECV] {resp}")
assert_fail(resp, "clear-pattern", "clear-pattern nonexistent returns ok:false")
assert_error_field(resp, "clear-pattern nonexistent includes error message")

# --- Test 7: bpm 5 (out of range) ---
print("\n--- Test 7: bpm 5 (out of range) ---")
resp = send_cmd("bpm 5")
print(f"[RECV] {resp}")
assert_fail(resp, "bpm", "bpm 5 returns ok:false")
assert_error_field(resp, "bpm 5 includes error message")
resp_ping = send_cmd("ping")
assert_ok(resp_ping, "ping", "engine still responsive after rejected bpm")

# --- Test 8: set-pattern d1 [unclosed ---
print("\n--- Test 8: set-pattern d1 [unclosed (invalid notation) ---")
resp = send_cmd("set-pattern d1 [unclosed")
print(f"[RECV] {resp}")
assert_fail(resp, "set-pattern", "invalid notation returns ok:false")
assert_error_field(resp, "invalid notation response includes parse error message")

# --- Test 9: set two patterns then slot-stop d1 ---
print("\n--- Test 9: set-pattern d1, d2 then slot-stop d1 ---")
resp = send_cmd("set-pattern d1 bd sn", timeout=10.0)
print(f"[RECV] {resp}")
assert_ok(resp, "set-pattern", "set-pattern d1 bd sn returns ok:true")

resp = send_cmd("set-pattern d2 cp hh", timeout=10.0)
print(f"[RECV] {resp}")
assert_ok(resp, "set-pattern", "set-pattern d2 cp hh returns ok:true")

resp = send_cmd("slot-stop d1")
print(f"[RECV] {resp}")
assert_ok(resp, "slot-stop", "slot-stop d1 returns ok:true")
assert_field(resp, "slot", "d1", "slot-stop response includes slot name")

# --- Test 10: slot-play d1 ---
print("\n--- Test 10: slot-play d1 ---")
resp = send_cmd("slot-play d1")
print(f"[RECV] {resp}")
assert_ok(resp, "slot-play", "slot-play d1 returns ok:true")
assert_field(resp, "slot", "d1", "slot-play response includes slot name")

# --- Test 11: slot-stop with missing slot name ---
print("\n--- Test 11: slot-stop with missing slot name ---")
resp = send_cmd("slot-stop")
print(f"[RECV] {resp}")
assert_fail(resp, "slot-stop", "slot-stop without slot returns ok:false")
assert_error_field(resp, "slot-stop without slot includes error message")

# --- Test 12: quit ---
print("\n--- Test 12: quit ---")
resp = send_cmd("quit")
print(f"[RECV] {resp}")
assert_ok(resp, "quit", "quit returns ok:true with cmd:quit")

try:
    proc.wait(timeout=3)
except subprocess.TimeoutExpired:
    proc.kill()

print(f"\n===============================")
print(f"Results: {pass_count} passed, {fail_count} failed ({assert_no} total assertions)")
print(f"===============================")

if fail_count == 0:
    print("[PASS] Protocol compliance test passed")
    sys.exit(0)
else:
    print(f"[FAIL] Protocol compliance test FAILED ({fail_count} assertion(s) failed)", file=sys.stderr)
    sys.exit(1)
PYEOF
