#!/usr/bin/env python3
# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Deterministic generator for the committed Hathor sample-bank fixtures.

Produces two small, deterministic, musically-appropriate drum samples that are
committed as real playback fixtures for the audio integration tests:

    samples/bd/0.wav  ->  kick drum  (low-frequency sine with a pitch glide + decay)
    samples/sn/0.wav  ->  snare drum (mid-range decaying buzz, harmonics of ~220 Hz)

Format contract (matches the repository's own test-audio tooling in
``tests/integration/test_audio.sh`` and ``tests/test_b8_k4_sample_registration.cpp``):

    * Sample rate: 44100 Hz  (matches Main.cpp's SampleBank::load device rate)
    * Channels:    1 (mono)
    * Bit depth:   16-bit signed little-endian PCM
    * Filename:    <name>/<index>.wav  (SuperDirt-style: bd/0.wav, sn/0.wav)

Design notes
------------
Both samples begin at a zero crossing (sin(0) == 0) so that triggering them
never produces a large single-sample amplitude jump.  This matters because
``analyze_audio.py check_hotswap`` fails the integration test on any inter-sample
jump exceeding 0.5 full-scale (it checks the +/-500 ms window around the
hot-swap point where a new pattern may begin firing voices).

The content is fully deterministic (no RNG) so the fixtures are byte-reproducible
and the integration assertions are stable across regenerations.
"""

import math
import os
import struct
import sys

SR = 44100
PEAK = 0.85  # master amplitude (well within [-1, 1], leaves headroom)


def write_wav(path: str, samples) -> int:
    """Write a mono 16-bit PCM WAV file. Returns the number of samples written."""
    n = len(samples)
    os.makedirs(os.path.dirname(path), exist_ok=True)

    # Quantise float [-1.0, 1.0] to int16, clamped.
    ints = []
    for s in samples:
        s = -1.0 if s < -1.0 else (1.0 if s > 1.0 else s)
        ints.append(int(round(s * 32767.0)))

    data = struct.pack(f"<{n}h", *ints)
    data_size = len(data)
    chunk_size = 36 + data_size

    header = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", chunk_size, b"WAVE",
        b"fmt ", 16,           # subchunk1 size = 16 (PCM)
        1,                     # audio format = 1 (PCM)
        1,                     # num channels = 1 (mono)
        SR,                    # sample rate
        SR * 2,                # byte rate = sampleRate * blockAlign
        2,                     # block align = 2 (16-bit * 1 channel)
        16,                    # bits per sample
        b"data", data_size,    # subchunk2
    )

    with open(path, "wb") as f:
        f.write(header + data)

    return n


def make_kick(duration_s: float = 0.12) -> list:
    """Kick drum: 140 Hz -> 45 Hz pitch glide with a fast attack + exponential decay."""
    n = int(round(SR * duration_s))
    out = [0.0] * n
    phase = 0.0
    attack_s = 0.004
    decay_tau = 0.06
    for i in range(n):
        t = i / SR
        # Pitch envelope: fast exponential decay -> low frequency tail.
        glide = math.exp(-t / 0.015)
        freq = 45.0 + (140.0 - 45.0) * glide
        # Amplitude envelope: linear attack then exponential decay.
        if t < attack_s:
            amp = t / attack_s
        else:
            amp = math.exp(-(t - attack_s) / decay_tau)
        phase += 2.0 * math.pi * freq / SR
        out[i] = PEAK * amp * math.sin(phase)
    return out


def make_snare(duration_s: float = 0.08) -> list:
    """Snare drum: decaying buzz from 220/440/660 Hz harmonics with a short attack.

    Pure-sinusoidal content keeps every inter-sample delta small (highest
    component is 660 Hz -> max ~0.08 FS/sample at PEAK), so the hotswap glitch
    check's 0.5 FS threshold is never approached.  All components start at a
    zero crossing, so triggering never produces a discontinuous jump.
    """
    n = int(round(SR * duration_s))
    out = [0.0] * n
    attack_s = 0.002
    decay_tau = 0.025
    # (frequency, relative amplitude)
    components = [(220.0, 1.0), (440.0, 0.55), (660.0, 0.35)]
    amp_scale = sum(c[1] for c in components)
    for i in range(n):
        t = i / SR
        if t < attack_s:
            env = t / attack_s
        else:
            env = math.exp(-(t - attack_s) / decay_tau)
        s = 0.0
        for freq, rel in components:
            s += rel * math.sin(2.0 * math.pi * freq * t)
        out[i] = PEAK * env * (s / amp_scale)
    return out


def main() -> int:
    root = os.path.dirname(os.path.abspath(__file__))
    bd_path = os.path.join(root, "bd", "0.wav")
    sn_path = os.path.join(root, "sn", "0.wav")

    bd = make_kick()
    sn = make_snare()

    n_bd = write_wav(bd_path, bd)
    n_sn = write_wav(sn_path, sn)

    print(f"[fixtures] wrote {bd_path}  ({n_bd} samples, {n_bd/SR*1000:.1f} ms, mono 16-bit {SR} Hz)")
    print(f"[fixtures] wrote {sn_path}  ({n_sn} samples, {n_sn/SR*1000:.1f} ms, mono 16-bit {SR} Hz)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
