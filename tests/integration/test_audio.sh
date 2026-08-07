#!/usr/bin/env bash
# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# test_audio.sh — Automated audio quality tests (Req 20.1 / Req 20.2)
#
# Runs two checks without requiring a human listener:
#
#   Req 20.1 — Onset check:
#     Send "set-pattern d1 bd sn", capture 2 s of audio, confirm non-silence
#     begins within 500 ms of the set-pattern command being sent.
#
#   Req 20.2 — Hot-swap check:
#     Play "bd sn" for 1 s, hot-swap to "bd sn [hh hh] cp", record 2 more
#     seconds, confirm no silence gap > one beat period and no amplitude
#     discontinuity > 0.5 FS around the swap point.
#
# Both checks use --capture-to-file to record the audio engine output to a
# WAV file, then analyze it with analyze_audio.py (pure Python stdlib, no
# numpy/scipy required).
#
# A synthetic sample bank (440 Hz sine tone, ~100 ms) is generated at test
# time so the test is self-contained and CI-safe without real audio files.
#
# Requirements: 20.1, 20.2
#
# Usage:
#   ./test_audio.sh <hathor_binary> [<samples_path>]
#
# Exit codes:
#   0  — all assertions passed
#   1  — at least one assertion failed

set -eu

HATHOR_BIN="${1:-}"
SAMPLES_PATH="${2:-}"   # optional — if omitted, a synthetic bank is generated

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ANALYZER="$SCRIPT_DIR/analyze_audio.py"
BPM=120
MAX_ONSET_MS=500

if [ -z "$HATHOR_BIN" ]; then
    echo "[FAIL] Usage: $0 <hathor_binary> [<samples_path>]" >&2
    exit 1
fi

if [ ! -x "$HATHOR_BIN" ]; then
    echo "[FAIL] hathor binary not found or not executable: $HATHOR_BIN" >&2
    exit 1
fi

if [ ! -f "$ANALYZER" ]; then
    echo "[FAIL] analyze_audio.py not found: $ANALYZER" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Create a temporary working directory; clean up on exit
# ---------------------------------------------------------------------------
WORK_DIR="$(mktemp -d)"
cleanup() { rm -rf "$WORK_DIR"; }
trap cleanup EXIT

# ---------------------------------------------------------------------------
# Generate a synthetic sample bank (440 Hz sine, 100 ms, 44100 Hz mono 16-bit)
# unless a real samples path was provided.
# ---------------------------------------------------------------------------
if [ -z "$SAMPLES_PATH" ]; then
    SAMPLES_PATH="$WORK_DIR/samples"
    python3 - "$SAMPLES_PATH" <<'PYEOF'
import struct, math, os, sys

out_root = sys.argv[1]

def write_sine_wav(path, freq=440.0, duration_s=0.10, sr=44100, amplitude=0.8):
    """Write a minimal sine-tone WAV at path."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    n_samples = int(sr * duration_s)
    data = struct.pack(f'<{n_samples}h',
        *[int(amplitude * 32767 * math.sin(2 * math.pi * freq * i / sr))
          for i in range(n_samples)])
    data_size = len(data)
    header = struct.pack('<4sI4s4sIHHIIHH4sI',
        b'RIFF', 36 + data_size, b'WAVE',
        b'fmt ', 16, 1, 1, sr, sr*2, 2, 16,
        b'data', data_size)
    with open(path, 'wb') as f:
        f.write(header + data)

# bd and sn: slightly different pitches so they are distinguishable
write_sine_wav(os.path.join(out_root, 'bd', '0.wav'), freq=80.0,  duration_s=0.12)
write_sine_wav(os.path.join(out_root, 'sn', '0.wav'), freq=200.0, duration_s=0.08)
write_sine_wav(os.path.join(out_root, 'hh', '0.wav'), freq=800.0, duration_s=0.04)
write_sine_wav(os.path.join(out_root, 'cp', '0.wav'), freq=400.0, duration_s=0.06)
print(f"[INFO] Synthetic sample bank written to: {out_root}")
PYEOF
fi

if [ ! -d "$SAMPLES_PATH" ]; then
    echo "[FAIL] samples directory not found: $SAMPLES_PATH" >&2
    exit 1
fi

PASS_COUNT=0
FAIL_COUNT=0

# ---------------------------------------------------------------------------
# Helper: run hathor, drive it via Python subprocess, capture to WAV
# ---------------------------------------------------------------------------
# Arguments: <wav_out> <total_record_s> <swap_after_s> <pat1> <pat2>
# If swap_after_s <= 0, no hot-swap is performed.
run_capture() {
    local wav_out="$1"
    local total_s="$2"
    local swap_after_s="$3"
    local pat1="$4"
    local pat2="${5:-}"

    python3 - "$HATHOR_BIN" "$SAMPLES_PATH" "$wav_out" \
              "$total_s" "$swap_after_s" "$pat1" "$pat2" <<'PYEOF'
import sys, subprocess, time, os

hathor_bin   = sys.argv[1]
samples_path = sys.argv[2]
wav_out      = sys.argv[3]
total_s      = float(sys.argv[4])
swap_after_s = float(sys.argv[5])
pat1         = sys.argv[6]
pat2         = sys.argv[7] if len(sys.argv) > 7 else ""

proc = subprocess.Popen(
    [hathor_bin, "--samples", samples_path,
     "--capture-to-file", wav_out],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.DEVNULL,
)
os.set_blocking(proc.stdout.fileno(), False)

def read_line(f, timeout=8.0):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        try:
            ch = f.read(1)
            if ch is None: time.sleep(0.001); continue
            if not ch: break
            buf += ch
            if ch == b"\n": break
        except BlockingIOError:
            time.sleep(0.001)
    return buf.decode(errors="replace").strip()

# Wait for ready
ready = read_line(proc.stdout, 8.0)
if '"event":"ready"' not in ready:
    print(f"[FAIL] hathor did not emit ready event: {ready!r}", file=sys.stderr)
    proc.kill(); sys.exit(1)

# Record start time for offset calculation
t_start = time.time()

# Send first pattern
proc.stdin.write(f"set-pattern d1 {pat1}\n".encode())
proc.stdin.flush()
resp1 = read_line(proc.stdout, 8.0)
t_set1 = time.time() - t_start
print(f"[INFO] set-pattern d1 {pat1!r} at +{t_set1*1000:.0f}ms: {resp1}")

if '"ok":true' not in resp1:
    print(f"[FAIL] set-pattern failed: {resp1}", file=sys.stderr)
    proc.kill(); sys.exit(1)

# Wait until swap point, then hot-swap if pat2 is given
if swap_after_s > 0 and pat2:
    elapsed = time.time() - t_start
    wait = swap_after_s - elapsed
    if wait > 0:
        time.sleep(wait)
    proc.stdin.write(f"set-pattern d1 {pat2}\n".encode())
    proc.stdin.flush()
    resp2 = read_line(proc.stdout, 8.0)
    t_set2 = time.time() - t_start
    print(f"[INFO] set-pattern d1 {pat2!r} at +{t_set2*1000:.0f}ms: {resp2}")
    if '"ok":true' not in resp2:
        print(f"[FAIL] hot-swap set-pattern failed: {resp2}", file=sys.stderr)
        proc.kill(); sys.exit(1)

# Record until total_s
elapsed = time.time() - t_start
remaining = total_s - elapsed
if remaining > 0:
    time.sleep(remaining)

proc.stdin.write(b"quit\n")
proc.stdin.flush()
read_line(proc.stdout, 3.0)
proc.wait(timeout=5)
print(f"[INFO] Recording complete: {wav_out}")
PYEOF
}

# ---------------------------------------------------------------------------
# Test A: Req 20.1 — onset within 500 ms
# ---------------------------------------------------------------------------
echo ""
echo "=== Test A: Req 20.1 — Audio onset within ${MAX_ONSET_MS} ms ==="
WAV_A="$WORK_DIR/onset_test.wav"

run_capture "$WAV_A" 2.0 0 "bd sn"

if [ ! -f "$WAV_A" ]; then
    echo "[FAIL] WAV file not created: $WAV_A" >&2
    FAIL_COUNT=$((FAIL_COUNT + 1))
else
    WAV_SIZE=$(python3 -c "import os; print(os.path.getsize('$WAV_A'))")
    echo "[INFO] WAV size: ${WAV_SIZE} bytes"

    if python3 "$ANALYZER" onset "$WAV_A" "$MAX_ONSET_MS" "$BPM"; then
        PASS_COUNT=$((PASS_COUNT + 1))
        echo "[PASS] Req 20.1: Audio onset test passed"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] Req 20.1: Audio onset test failed" >&2
    fi
fi

# ---------------------------------------------------------------------------
# Test B: Req 20.2 — no glitch during hot-swap
# ---------------------------------------------------------------------------
echo ""
echo "=== Test B: Req 20.2 — No glitch during hot-swap ==="
WAV_B="$WORK_DIR/hotswap_test.wav"
SWAP_AFTER_S=1.0
TOTAL_S=3.0

run_capture "$WAV_B" "$TOTAL_S" "$SWAP_AFTER_S" "bd sn" "bd sn [hh hh] cp"

if [ ! -f "$WAV_B" ]; then
    echo "[FAIL] WAV file not created: $WAV_B" >&2
    FAIL_COUNT=$((FAIL_COUNT + 1))
else
    WAV_SIZE=$(python3 -c "import os; print(os.path.getsize('$WAV_B'))")
    echo "[INFO] WAV size: ${WAV_SIZE} bytes"

    if python3 "$ANALYZER" hotswap "$WAV_B" "$SWAP_AFTER_S" "$BPM"; then
        PASS_COUNT=$((PASS_COUNT + 1))
        echo "[PASS] Req 20.2: Hot-swap glitch test passed"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "[FAIL] Req 20.2: Hot-swap glitch test failed" >&2
    fi
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
echo ""
TOTAL=$((PASS_COUNT + FAIL_COUNT))
echo "==============================="
echo "Results: ${PASS_COUNT}/${TOTAL} audio tests passed"
echo "==============================="

if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "[PASS] All audio quality tests passed"
    exit 0
else
    echo "[FAIL] ${FAIL_COUNT} audio test(s) failed" >&2
    exit 1
fi
