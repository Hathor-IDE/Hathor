// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * hathor-audio-worker — companion worker process for ChucK audio execution.
 *
 * This is a standalone, JUCE-free executable that owns the ChucK VM lifecycle
 * (filled in by B4-K3) and produces audio samples into shared memory for the
 * main Hathor process to consume.
 *
 * K0.5 conformance: this worker does NOT expose concurrent compileCode/run.
 * The control plane handles lifecycle only; ChucK compilation will be added
 * by B4-K4 and must use a serialized command path.
 *
 * K0.6 conformance: the shared-memory transport contract, generation identity
 * pattern, control-plane socket protocol, and seqlock discipline are identical
 * to the validated spike.
 *
 * Architecture:
 *   - argv[1] = control-plane Unix socket path (passed by parent)
 *   - Fixed shared-memory name /hathor-audio-worker (from audio_ipc.h)
 *   - Parent initialises SHM and sets generation BEFORE spawning this worker
 *   - Worker reads generation at startup, becomes the sole producer
 *
 * Requirements: B4-K2, B4-K0.5, B4-K0.6, Decision #24
 */

#include "audio_ipc.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

using hathor::audio_worker::AudioBlock;
using hathor::audio_worker::SharedAudioTransport;
using hathor::audio_worker::kBlockSize;
using hathor::audio_worker::kControlName;
using hathor::audio_worker::kMagic;
using hathor::audio_worker::kRingCapacity;
using hathor::audio_worker::kRingMask;
using hathor::audio_worker::kShmName;
using hathor::audio_worker::kShmSize;

// ---------------------------------------------------------------------------
// Globals (set before threads fork; worker is single-threaded + control thread)
// ---------------------------------------------------------------------------

static std::atomic<bool> gRunning{true};
static SharedAudioTransport* gTransport = nullptr;
static size_t gShmSize = 0;
static int gShmFd = -1;
static std::string gControlSocketPath;

static void handleSigterm(int /*signum*/) {
    gRunning.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Audio production (placeholder — K3 will replace with real ChucK output)
// ---------------------------------------------------------------------------

static void produceBlock(AudioBlock& block, uint32_t wSeq, uint64_t gen) {
    // Seqlock begin: set sequence to odd (write in progress).
    block.sequence.store(wSeq | 1u, std::memory_order_release);

    // Generate a deterministic test tone.  K3 will replace this with actual
    // ChucK VM output via a per-VM audio callback.
    const float basePhase = static_cast<float>(gen) * 0.01f;
    const float freq      = 220.0f + static_cast<float>(gen) * 11.0f; // A3 + gen offset
    const float phaseInc  = freq * 2.0f * 3.14159265f / 44100.0f;

    for (uint32_t i = 0; i < kBlockSize; ++i) {
        const float phase = basePhase + static_cast<float>(wSeq * kBlockSize + i) * phaseInc;
        block.samples[i] = std::sin(phase) * 0.1f;
    }

    // Seqlock end: set sequence to even (write complete).
    block.sequence.store(wSeq + 2u, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Control plane (Unix domain socket)
// ---------------------------------------------------------------------------

static void controlPlaneThread() {
    // Use the socket path passed via argv[1] by the parent, falling back to
    // the compile-time default if empty (for standalone/manual runs).
    const std::string ctrlPath = gControlSocketPath.empty()
        ? std::string(kControlName)
        : gControlSocketPath;

    // Remove any stale socket file.
    ::unlink(ctrlPath.c_str());

    const int srvFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srvFd < 0) {
        std::fprintf(stderr, "[worker] socket() failed: %s\n", std::strerror(errno));
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, ctrlPath.c_str(), sizeof(addr.sun_path) - 1);
    ::unlink(ctrlPath.c_str());

    if (::bind(srvFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::fprintf(stderr, "[worker] bind() failed: %s\n", std::strerror(errno));
        ::close(srvFd);
        ::unlink(ctrlPath.c_str());
        return;
    }

    if (::listen(srvFd, 4) != 0) {
        std::fprintf(stderr, "[worker] listen() failed: %s\n", std::strerror(errno));
        ::close(srvFd);
        ::unlink(ctrlPath.c_str());
        return;
    }

    while (gRunning.load(std::memory_order_acquire)) {
        struct pollfd pfd{};
        pfd.fd = srvFd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, 50);
        if (pr <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        const int connFd = ::accept(srvFd, nullptr, nullptr);
        if (connFd < 0) continue;

        char buf[256];
        const ssize_t n = ::read(connFd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string cmd(buf, static_cast<size_t>(n));
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
                cmd.pop_back();

            std::string resp;
            if (cmd == "status") {
                const uint64_t gen = gTransport->generation.load(std::memory_order_acquire);
                const uint32_t wSeq = gTransport->writeSeq.load(std::memory_order_acquire);
                const uint32_t rSeq = gTransport->readSeq.load(std::memory_order_acquire);
                const uint64_t beat = gTransport->lastHeartbeat.load(std::memory_order_acquire);
                resp = "ok gen=" + std::to_string(gen)
                     + " wSeq=" + std::to_string(wSeq)
                     + " rSeq=" + std::to_string(rSeq)
                     + " beat=" + std::to_string(beat) + "\n";
            } else if (cmd == "stop") {
                resp = "ok stopping\n";
                // Send the response BEFORE setting gRunning=false so the
                // parent receives the acknowledgment.
                ::write(connFd, resp.c_str(), resp.size());
                ::close(connFd);
                gRunning.store(false, std::memory_order_release);
                continue;
            } else if (cmd == "ping") {
                resp = "ok pong\n";
            } else if (cmd.rfind("vm_create", 0) == 0) {
                // K3 will implement actual VM creation here.
                // For now: acknowledge that the command is understood but
                // the feature is not yet implemented (B4-K0.5 says no VM
                // auto-creation per open file; K3 handles VM policy).
                resp = "ok vm_create_not_implemented\n";
            } else if (cmd.rfind("vm_destroy", 0) == 0) {
                resp = "ok vm_destroy_not_implemented\n";
            } else {
                resp = "err unknown command\n";
            }
            ::write(connFd, resp.c_str(), resp.size());
        }
        ::close(connFd);
    }

    ::close(srvFd);
    ::unlink(ctrlPath.c_str());
}

// ---------------------------------------------------------------------------
// Audio production loop
// ---------------------------------------------------------------------------

static void audioProductionLoop() {
    // 5ms tick — produces 64 samples per tick (12800 Hz at kBlockSize=64).
    // K3 will replace this with a real audio callback driven by the ChucK VM.
    auto nextWake = std::chrono::steady_clock::now();
    uint64_t beat = 0;

    while (gRunning.load(std::memory_order_acquire)) {
        nextWake += std::chrono::milliseconds(5);
        std::this_thread::sleep_until(nextWake);

        const uint32_t wSeq = gTransport->writeSeq.load(std::memory_order_relaxed);
        const uint32_t rSeq = gTransport->readSeq.load(std::memory_order_acquire);

        // If the consumer (main process) is falling behind, drop old blocks
        // to prevent the ring from stalling.
        if (wSeq - rSeq >= kRingCapacity) {
            const uint32_t newRSeq = wSeq - kRingCapacity + 1u;
            gTransport->readSeq.store(newRSeq, std::memory_order_release);
        }

        // Re-read generation in case the parent changed it (shouldn't happen
        // in normal operation, but the contract allows it).
        const uint64_t gen = gTransport->generation.load(std::memory_order_acquire);

        // Produce the block at slot wSeq (matching the seqlock contract:
        // block[wSeq].sequence transitions 0→wSeq|1→wSeq+2, and the reader
        // checks block[rSeq].sequence == rSeq + 2).
        const auto wStart = std::chrono::steady_clock::now();
        AudioBlock& block = gTransport->blocks[wSeq & kRingMask];
        produceBlock(block, wSeq, gen);
        const auto wEnd = std::chrono::steady_clock::now();

        // Update writer-side instrumentation (best-effort).
        const uint64_t wNs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wEnd - wStart).count());

        uint64_t prevMin = gTransport->wrMinNs.load(std::memory_order_relaxed);
        while (prevMin > wNs &&
               !gTransport->wrMinNs.compare_exchange_weak(prevMin, wNs, std::memory_order_relaxed)) {}

        uint64_t prevMax = gTransport->wrMaxNs.load(std::memory_order_relaxed);
        while (wNs > prevMax &&
               !gTransport->wrMaxNs.compare_exchange_weak(prevMax, wNs, std::memory_order_relaxed)) {}

        gTransport->wrSumNs.fetch_add(wNs, std::memory_order_relaxed);
        gTransport->wrCount.fetch_add(1, std::memory_order_relaxed);

        // Advance the heartbeat and write sequence.
        gTransport->lastHeartbeat.store(beat, std::memory_order_release);
        ++beat;
        gTransport->writeSeq.store(wSeq + 1u, std::memory_order_release);
    }
}

// ---------------------------------------------------------------------------
// Shared-memory initialisation
// ---------------------------------------------------------------------------

static bool initSharedMemory() {
    // The parent process creates and sizes the SHM segment BEFORE spawning us.
    // We open it read-write.  If it doesn't exist (standalone/manual run),
    // we create it ourselves.
    gShmFd = ::shm_open(kShmName, O_RDWR, 0600);
    if (gShmFd < 0) {
        // Fallback: create it ourselves (standalone mode).
        gShmFd = ::shm_open(kShmName, O_CREAT | O_RDWR, 0600);
        if (gShmFd < 0) {
            std::fprintf(stderr, "[worker] shm_open failed: %s\n", std::strerror(errno));
            return false;
        }
        if (::ftruncate(gShmFd, static_cast<off_t>(kShmSize)) != 0) {
            std::fprintf(stderr, "[worker] ftruncate failed: %s\n", std::strerror(errno));
            ::close(gShmFd);
            gShmFd = -1;
            return false;
        }
    }

    struct stat st{};
    if (::fstat(gShmFd, &st) != 0) {
        std::fprintf(stderr, "[worker] fstat failed\n");
        ::close(gShmFd);
        gShmFd = -1;
        return false;
    }

    gShmSize = static_cast<size_t>(st.st_size);
    gTransport = static_cast<SharedAudioTransport*>(
        ::mmap(nullptr, gShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, gShmFd, 0));
    if (gTransport == MAP_FAILED) {
        std::fprintf(stderr, "[worker] mmap failed: %s\n", std::strerror(errno));
        ::close(gShmFd);
        gShmFd = -1;
        gTransport = nullptr;
        return false;
    }

    // Read generation from the transport — set by the parent before spawn.
    const uint64_t gen = gTransport->generation.load(std::memory_order_acquire);

    // Ensure magic is set (parent should have already done this, but verify).
    gTransport->magic.store(kMagic, std::memory_order_release);
    gTransport->sampleRate.store(44100, std::memory_order_release);
    gTransport->channels.store(1, std::memory_order_release);
    gTransport->workerAlive.store(true, std::memory_order_release);
    gTransport->lastHeartbeat.store(0, std::memory_order_release);

    std::fprintf(stderr, "[worker] started, pid=%d, generation=%llu\n",
                 static_cast<int>(::getpid()),
                 static_cast<unsigned long long>(gen));

    return true;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Install signal handlers for graceful shutdown.
    ::signal(SIGTERM, handleSigterm);
    ::signal(SIGINT, handleSigterm);

    // Read the control socket path from argv[1] (passed by the parent).
    if (argc >= 2) {
        gControlSocketPath = argv[1];
    }

    if (!initSharedMemory()) {
        std::fprintf(stderr, "[worker] failed to initialise shared memory\n");
        return 1;
    }

    // Start the control-plane listener thread.
    std::thread ctrlThread(controlPlaneThread);

    // Run the audio production loop on the main thread.
    // K3 will replace this with a ChucK VM audio callback.
    audioProductionLoop();

    // Signal the control plane to shut down.
    gRunning.store(false, std::memory_order_release);

    // Clean up: mark worker as no longer alive.
    if (gTransport && gTransport != MAP_FAILED) {
        gTransport->workerAlive.store(false, std::memory_order_release);
    }

    std::fprintf(stderr, "[worker] shutting down, pid=%d\n", static_cast<int>(::getpid()));

    // Wait for the control thread to finish.
    if (ctrlThread.joinable())
        ctrlThread.join();

    // Unmap and close shared memory.
    if (gTransport && gTransport != MAP_FAILED) {
        ::munmap(gTransport, gShmSize);
    }
    if (gShmFd >= 0) {
        ::close(gShmFd);
    }

    // NOTE: We do NOT unlink the SHM here — the parent process owns SHM
    // lifetime and is responsible for shm_unlink after waitpid.
    // This avoids race conditions where the parent cleans up while we
    // are still reading or writing.

    return 0;
}
