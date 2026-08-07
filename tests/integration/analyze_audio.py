#!/usr/bin/env python3
# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
analyze_audio.py — Audio quality analysis for Hathor integration tests.

Provides two analysis functions used by test_audio.sh:

  1. check_onset(wav_path, max_silence_ms, bpm)
     Req 20.1: Confirms that non-silence audio begins within max_silence_ms
     milliseconds of the start of the recording.

  2. check_hotswap(wav_path, swap_time_s, bpm)
     Req 20.2: Confirms there is no glitch/dropout around the hot-swap point.
     A "glitch" is defined as either:
       - A silence gap longer than one beat period in the middle of the recording
       - A sample-to-sample amplitude discontinuity exceeding 0.5 of full scale
         (a click/pop audible as > 6 dB above the signal floor)

Usage:
    python3 analyze_audio.py onset  <wav_file> <max_silence_ms> <bpm>
    python3 analyze_audio.py hotswap <wav_file> <swap_time_s> <bpm>
    python3 analyze_audio.py full   <wav_file> <max_silence_ms> <swap_time_s> <bpm>

Exit codes:
    0  — all checks passed
    1  — at least one check failed
"""

import sys
import struct
import math


# ---------------------------------------------------------------------------
# WAV reader (no numpy/scipy dependency — pure stdlib)
# ---------------------------------------------------------------------------

def read_wav(path):
    """
    Read a WAV file and return (samples_list, sample_rate, num_channels).
    samples_list contains interleaved float values in [-1.0, 1.0].
    Supports 8, 16, 24, and 32-bit PCM.
    """
    with open(path, 'rb') as f:
        riff = f.read(4)
        if riff != b'RIFF':
            raise ValueError(f"Not a RIFF file: {path}")
        _file_size = struct.unpack('<I', f.read(4))[0]
        wave = f.read(4)
        if wave != b'WAVE':
            raise ValueError(f"Not a WAVE file: {path}")

        # Read chunks until we find fmt and data
        sample_rate   = None
        num_channels  = None
        bits_per_sample = None
        audio_data    = None

        while True:
            chunk_id = f.read(4)
            if len(chunk_id) < 4:
                break
            chunk_size = struct.unpack('<I', f.read(4))[0]
            chunk_data = f.read(chunk_size)

            if chunk_id == b'fmt ':
                audio_fmt, num_channels, sample_rate = struct.unpack_from('<HHI', chunk_data)
                bits_per_sample = struct.unpack_from('<H', chunk_data, 14)[0]
            elif chunk_id == b'data':
                audio_data = chunk_data

        if audio_data is None or sample_rate is None:
            raise ValueError(f"Incomplete WAV file: {path}")

        # Decode samples to float [-1, 1]
        if bits_per_sample == 16:
            n = len(audio_data) // 2
            raw = struct.unpack(f'<{n}h', audio_data[:n*2])
            scale = 1.0 / 32768.0
            samples = [s * scale for s in raw]
        elif bits_per_sample == 8:
            raw = struct.unpack(f'{len(audio_data)}B', audio_data)
            samples = [(s - 128) / 128.0 for s in raw]
        elif bits_per_sample == 24:
            n = len(audio_data) // 3
            samples = []
            for i in range(n):
                b0, b1, b2 = audio_data[3*i], audio_data[3*i+1], audio_data[3*i+2]
                v = b0 | (b1 << 8) | (b2 << 16)
                if v >= 0x800000:
                    v -= 0x1000000
                samples.append(v / 8388608.0)
        elif bits_per_sample == 32:
            n = len(audio_data) // 4
            raw = struct.unpack(f'<{n}i', audio_data[:n*4])
            scale = 1.0 / 2147483648.0
            samples = [s * scale for s in raw]
        else:
            raise ValueError(f"Unsupported bit depth: {bits_per_sample}")

        return samples, sample_rate, num_channels


def mono_mix(samples, num_channels):
    """Mix interleaved multi-channel samples down to mono."""
    if num_channels == 1:
        return samples
    mono = []
    for i in range(0, len(samples), num_channels):
        frame = samples[i:i+num_channels]
        mono.append(sum(frame) / len(frame))
    return mono


def rms_window(mono, start_frame, end_frame):
    """Compute RMS over a frame range."""
    chunk = mono[max(0, start_frame):min(len(mono), end_frame)]
    if not chunk:
        return 0.0
    return math.sqrt(sum(s*s for s in chunk) / len(chunk))


# ---------------------------------------------------------------------------
# Check 1: onset within max_silence_ms (Req 20.1)
# ---------------------------------------------------------------------------

def check_onset(wav_path, max_silence_ms, bpm=120.0):
    """
    Verify that audio onset (RMS above silence threshold) begins within
    max_silence_ms milliseconds of the recording start.

    Returns (passed: bool, message: str).
    """
    samples, sr, channels = read_wav(wav_path)
    mono = mono_mix(samples, channels)

    if not mono:
        return False, "WAV file contains no samples"

    # Silence threshold: -50 dBFS RMS
    silence_threshold = 10 ** (-50.0 / 20.0)

    # Window size: 10 ms
    window_frames = max(1, int(sr * 0.010))
    max_silence_frames = int(sr * max_silence_ms / 1000.0)

    onset_frame = None
    for frame in range(0, len(mono) - window_frames, window_frames):
        rms = rms_window(mono, frame, frame + window_frames)
        if rms > silence_threshold:
            onset_frame = frame
            break

    if onset_frame is None:
        total_ms = 1000.0 * len(mono) / sr
        return False, (
            f"No audio onset detected in {total_ms:.0f} ms of recording "
            f"(entire file is below silence threshold {silence_threshold:.6f} RMS). "
            f"Peak level: {max(abs(s) for s in mono):.6f}"
        )

    onset_ms = 1000.0 * onset_frame / sr
    if onset_ms <= max_silence_ms:
        return True, f"Audio onset at {onset_ms:.1f} ms (limit: {max_silence_ms} ms)"
    else:
        return False, (
            f"Audio onset too late: {onset_ms:.1f} ms > {max_silence_ms} ms limit"
        )


# ---------------------------------------------------------------------------
# Check 2: no glitch/dropout during hot-swap (Req 20.2)
# ---------------------------------------------------------------------------

def check_hotswap(wav_path, swap_time_s, bpm=120.0):
    """
    Verify no audio glitch or dropout occurs around the hot-swap point.

    Two checks:
      a) No silence gap longer than one beat period (60/bpm seconds) anywhere
         in the post-onset portion of the recording.
      b) No single-sample amplitude discontinuity > 0.5 full scale (click/pop).

    Returns (passed: bool, message: str).
    """
    samples, sr, channels = read_wav(wav_path)
    mono = mono_mix(samples, channels)

    if not mono:
        return False, "WAV file contains no samples"

    silence_threshold = 10 ** (-50.0 / 20.0)
    window_frames     = max(1, int(sr * 0.010))   # 10 ms windows

    # Find onset frame (same logic as check_onset)
    onset_frame = None
    for frame in range(0, len(mono) - window_frames, window_frames):
        rms = rms_window(mono, frame, frame + window_frames)
        if rms > silence_threshold:
            onset_frame = frame
            break

    if onset_frame is None:
        return False, "No audio onset detected — cannot check for hotswap glitch"

    # -----------------------------------------------------------------------
    # Check a: no silence gap > one beat period after onset
    # -----------------------------------------------------------------------
    beat_frames       = int(sr * 60.0 / bpm)
    max_gap_frames    = beat_frames  # one full beat of silence = dropout

    swap_frame = int(swap_time_s * sr)
    # Only check from onset to end (ignore leading silence before first hit)
    check_start = onset_frame
    check_end   = len(mono) - window_frames

    current_gap = 0
    max_observed_gap = 0
    for frame in range(check_start, check_end, window_frames):
        rms = rms_window(mono, frame, frame + window_frames)
        if rms < silence_threshold:
            current_gap += window_frames
            max_observed_gap = max(max_observed_gap, current_gap)
        else:
            current_gap = 0

    max_gap_ms       = 1000.0 * max_observed_gap / sr
    max_allowed_ms   = 1000.0 * max_gap_frames / sr

    if max_observed_gap > max_gap_frames:
        return False, (
            f"Silence gap of {max_gap_ms:.0f} ms detected "
            f"(limit: {max_allowed_ms:.0f} ms = one beat at {bpm} BPM). "
            f"Possible dropout at hot-swap."
        )

    # -----------------------------------------------------------------------
    # Check b: no large amplitude discontinuity (click/pop > 0.5 FS)
    # -----------------------------------------------------------------------
    click_threshold = 0.5  # 0.5 full scale ≈ -6 dBFS jump
    max_jump = 0.0
    max_jump_frame = -1

    # Only check around the swap window ±500 ms for speed
    check_lo = max(onset_frame, swap_frame - int(sr * 0.5))
    check_hi = min(len(mono) - 1, swap_frame + int(sr * 0.5))

    for i in range(check_lo, check_hi):
        jump = abs(mono[i+1] - mono[i])
        if jump > max_jump:
            max_jump = jump
            max_jump_frame = i

    if max_jump > click_threshold:
        jump_ms = 1000.0 * max_jump_frame / sr
        return False, (
            f"Amplitude discontinuity of {max_jump:.3f} FS at {jump_ms:.1f} ms "
            f"(threshold: {click_threshold} FS). Possible click/pop at hot-swap."
        )

    return True, (
        f"No glitch detected around swap at {swap_time_s*1000:.0f} ms. "
        f"Max silence gap: {max_gap_ms:.0f} ms (limit: {max_allowed_ms:.0f} ms), "
        f"max jump: {max_jump:.3f} FS (limit: {click_threshold} FS)"
    )


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)

    mode = sys.argv[1]
    passed_all = True

    if mode == "onset":
        if len(sys.argv) < 5:
            print("Usage: analyze_audio.py onset <wav> <max_silence_ms> <bpm>", file=sys.stderr)
            sys.exit(1)
        wav     = sys.argv[2]
        max_ms  = float(sys.argv[3])
        bpm     = float(sys.argv[4])
        ok, msg = check_onset(wav, max_ms, bpm)
        print(f"[{'PASS' if ok else 'FAIL'}] onset: {msg}")
        if not ok:
            passed_all = False

    elif mode == "hotswap":
        if len(sys.argv) < 5:
            print("Usage: analyze_audio.py hotswap <wav> <swap_time_s> <bpm>", file=sys.stderr)
            sys.exit(1)
        wav      = sys.argv[2]
        swap_s   = float(sys.argv[3])
        bpm      = float(sys.argv[4])
        ok, msg  = check_hotswap(wav, swap_s, bpm)
        print(f"[{'PASS' if ok else 'FAIL'}] hotswap: {msg}")
        if not ok:
            passed_all = False

    elif mode == "full":
        if len(sys.argv) < 6:
            print("Usage: analyze_audio.py full <wav> <max_silence_ms> <swap_time_s> <bpm>",
                  file=sys.stderr)
            sys.exit(1)
        wav     = sys.argv[2]
        max_ms  = float(sys.argv[3])
        swap_s  = float(sys.argv[4])
        bpm     = float(sys.argv[5])

        ok1, msg1 = check_onset(wav, max_ms, bpm)
        ok2, msg2 = check_hotswap(wav, swap_s, bpm)
        print(f"[{'PASS' if ok1 else 'FAIL'}] onset:   {msg1}")
        print(f"[{'PASS' if ok2 else 'FAIL'}] hotswap: {msg2}")
        if not ok1 or not ok2:
            passed_all = False

    else:
        print(f"Unknown mode: {mode}", file=sys.stderr)
        sys.exit(1)

    sys.exit(0 if passed_all else 1)


if __name__ == "__main__":
    main()
