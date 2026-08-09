// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * hathor-audio-worker — companion worker process for ChucK audio execution.
 *
 * This is a standalone, JUCE-free executable that owns the ChucK VM lifecycle
 * (B4-K3) and produces audio samples into shared memory for the main Hathor
 * process to consume.
 *
 * K0.5 conformance: this worker does NOT expose concurrent compileCode/run.
 * All ChucK compile operations are serialized through ChuckCompiler's
 * dispatcher thread — the sole caller of any ChucK compile API for any VM.
 * The per-tab render thread calls run() but NEVER compileCode().
 *
 * K0.6 conformance: the shared-memory transport contract, generation identity
 * pattern, control-plane socket protocol, and seqlock discipline are identical
 * to the validated spike.
 *
 * B4-K4: .ck compilation happens on the ChuckCompiler dispatcher thread.
 * Compiled shreds are published via std::atomic_store_explicit(release)
 * into the per-tab VM's handoff slot, and consumed on the next loop
 * iteration via std::atomic_load_explicit(acquire).
 *
 * Architecture:
 *   - argv[1] = control-plane Unix socket path (passed by parent)
 *   - Fixed shared-memory name /hathor-audio-worker (from audio_ipc.h)
 *   - Parent initialises SHM and sets generation BEFORE spawning this worker
 *   - Worker reads generation at startup, becomes the sole producer
 *
 * Thread layout:
 *   - Control plane thread: receives socket commands (vm_create, vm_destroy,
 *     ck_compile), delegates to VmLifecycle and ChuckCompiler.
 *   - Compile dispatcher thread (ChuckCompiler): sole thread calling ChucK
 *     compile APIs. Serializes all compilation.
 *   - Audio render thread (main of worker): per-tab VM run() loops, produces
 *     audio into the shared-memory ring.
 *
 * Requirements: B4-K2, B4-K3, B4-K4, B4-K0.5, B4-K0.6, Decision #24
 */

#include "audio_ipc.h"
#include "ChuckCompiler.hpp"
#include "ChuckVm.hpp"
#include "VmLifecycle.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

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
using hathor::audio_worker::ChuckCompiler;
using hathor::audio_worker::ChuckVmEntry;
using hathor::audio_worker::CompileCommand;
using hathor::audio_worker::CompiledShred;
using hathor::audio_worker::TabId;
using hathor::audio_worker::VmLifecycle;
using hathor::audio_worker::VmState;

// ---------------------------------------------------------------------------
// Globals (set before threads fork; worker is single-threaded + control thread)
// ---------------------------------------------------------------------------

static std::atomic<bool> gRunning{true};
static SharedAudioTransport* gTransport = nullptr;
static size_t gShmSize = 0;
static int gShmFd = -1;
static std::string gControlSocketPath;

// Per-tab VM lifecycle manager — lives in the worker process.
// Thread-safe internally (vmTableMtx_ guards structural changes).
static VmLifecycle gVmLifecycle;

// B4-K4: serialized compiler — created on the main thread before the
// control thread starts. The compile dispatcher thread is the sole caller
// of any ChucK compile API (K0.5 NO-GO: no concurrent compile+run).
static std::unique_ptr<ChuckCompiler> gCompiler;

static void handleSigterm(int /*signum*/) {
    gRunning.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Audio production — placeholder (K3 will replace with real ChucK output)
// ---------------------------------------------------------------------------

static void produceBlock(AudioBlock& block, uint32_t wSeq, uint64_t gen) {
    // Seqlock begin: set sequence to odd (write in progress).
    block.sequence.store(wSeq | 1u, std::memory_order_release);

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
// Per-tab render loop — checks for a compiled shred on each iteration
// ---------------------------------------------------------------------------

[[maybe_unused]] static void vmRenderLoop(TabId tabId, uint64_t expectedGen) {
    // This runs on a per-tab OS thread. Each iteration:
    //   1. Check if the VM is still active (generation liveness).
    //   2. Load any pending handoff shred via std::atomic_load_explicit(acquire).
    //   3. If a new shred is available (version matches), install it.
    //   4. Produce audio (run() — when libchuck is linked) into the ring.
    //
    // K0.5 enforcement: this thread NEVER calls compileCode(). Compilation
    // is solely the responsibility of the ChuckCompiler dispatcher thread.

    while (gRunning.load(std::memory_order_acquire)) {
        // Liveness check: if the VM was destroyed or replaced, exit.
        if (gVmLifecycle.stateOf(tabId) != VmState::Active)
            return;

        // Atomic handoff consumption — matches AudioEngine::loadSlot():
        //   std::atomic_load_explicit(&slots_[idx], std::memory_order_acquire)
        auto shred = gVmLifecycle.loadHandoff(tabId);
        if (shred) {
            // New shred is ready. On the next iteration it will be consumed by
            // the VM's run() path. For now we just record the source hash.
            if (shred->ok) {
                std::fprintf(stderr, "[worker] tab=%d: received shred v=%u hash=%llu\n",
                             tabId,
                             shred->requestVersion,
                             static_cast<unsigned long long>(shred->sourceHash));
            } else {
                std::fprintf(stderr, "[worker] tab=%d: compile FAILED: %s\n",
                             tabId, shred->error.c_str());
                // Failure path: old valid shred remains active (not replaced).
            }
        }

        // Produce a block of audio (placeholder — real ChucK run() goes here).
        const uint32_t wSeq = gTransport->writeSeq.load(std::memory_order_relaxed);
        const uint32_t rSeq = gTransport->readSeq.load(std::memory_order_acquire);
        if (wSeq - rSeq >= kRingCapacity) {
            const uint32_t newRSeq = wSeq - kRingCapacity + 1u;
            gTransport->readSeq.store(newRSeq, std::memory_order_release);
        }

        const uint64_t gen = gTransport->generation.load(std::memory_order_acquire);
        // Check if the worker generation changed (worker restart invalidates).
        if (gen != expectedGen)
            return;

        AudioBlock& block = gTransport->blocks[wSeq & kRingMask];
        produceBlock(block, wSeq, gen);

        gTransport->lastHeartbeat.store(wSeq, std::memory_order_release);
        gTransport->writeSeq.store(wSeq + 1u, std::memory_order_release);

        // 5ms tick.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---------------------------------------------------------------------------
// Audio production loop — runs on the main worker thread
// ---------------------------------------------------------------------------

static void audioProductionLoop(uint64_t expectedGen) {
    // In B4-K3, each active tab gets its own render thread (vmRenderLoop).
    // For now (B4-K4 phase), we run a single placeholder loop that covers
    // all active VMs. The per-tab render thread model is introduced by K3.
    //
    // The key B4-K4 requirement: compilation does NOT happen here.
    // The compile dispatcher thread (ChuckCompiler) handles compilation
    // and publishes via atomic handoff. This loop only consumes handoff
    // results and produces audio — it never allocates for compilation.

    while (gRunning.load(std::memory_order_acquire)) {
        // Scan for active VMs and check their handoffs.
        for (int tab = 0; tab < hathor::audio_worker::kNumTabs; ++tab) {
            if (gVmLifecycle.stateOf(static_cast<TabId>(tab)) != VmState::Active)
                continue;
            // Non-blocking load of any pending handoff.
            auto shred = gVmLifecycle.loadHandoff(static_cast<TabId>(tab));
            if (shred && shred->ok) {
                std::fprintf(stderr, "[worker] tab=%d: render loop received shred v=%u\n",
                             tab, shred->requestVersion);
            }
        }

        // Produce a placeholder audio block (K3 replaces with per-VM output).
        const uint32_t wSeq = gTransport->writeSeq.load(std::memory_order_relaxed);
        const uint32_t rSeq = gTransport->readSeq.load(std::memory_order_acquire);
        if (wSeq - rSeq >= kRingCapacity) {
            const uint32_t newRSeq = wSeq - kRingCapacity + 1u;
            gTransport->readSeq.store(newRSeq, std::memory_order_release);
        }

        const uint64_t gen = gTransport->generation.load(std::memory_order_acquire);
        if (gen != expectedGen)
            return;

        AudioBlock& block = gTransport->blocks[wSeq & kRingMask];
        produceBlock(block, wSeq, gen);

        gTransport->lastHeartbeat.store(wSeq, std::memory_order_release);
        gTransport->writeSeq.store(wSeq + 1u, std::memory_order_release);

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---------------------------------------------------------------------------
// Control plane (Unix domain socket) — serialized command path
// ---------------------------------------------------------------------------

static void controlPlaneThread() {
    const std::string ctrlPath = gControlSocketPath.empty()
        ? std::string(kControlName)
        : gControlSocketPath;

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

        // Read the full command line. The control protocol is newline-terminated.
        // ck_compile carries .ck source which may exceed a single read, so we
        // loop until we see a newline or fill the buffer.
        char buf[8192];
        std::string cmd;
        bool cmdComplete = false;
        while (!cmdComplete) {
            const ssize_t n = ::read(connFd, buf, sizeof(buf));
            if (n <= 0) break;
            cmd.append(buf, static_cast<size_t>(n));
            if (cmd.size() > 0 && cmd.back() == '\n') {
                cmdComplete = true;
                cmd.pop_back();
            }
            if (cmd.size() >= sizeof(buf)) break; // safety cap
        }
        // Strip trailing CR if present (CRLF safety).
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
            ::write(connFd, resp.c_str(), resp.size());
            ::close(connFd);
            gRunning.store(false, std::memory_order_release);
            continue;
        } else if (cmd == "ping") {
            resp = "ok pong\n";
        } else if (cmd.rfind("vm_create", 0) == 0) {
            // Format: vm_create <tabId>
            // K3: create/replace the VM for this tab. Returns the new
            // vmGeneration so the caller can detect staleness.
            std::string rest = cmd.substr(9); // skip "vm_create"
            // trim leading whitespace
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' ||
                                     rest.front() == '\n' || rest.front() == '\r'))
                rest.erase(0, 1);
            try {
                TabId tabId = static_cast<TabId>(std::stoi(rest));
                const uint64_t newGen = gVmLifecycle.vmCreate(tabId);
                resp = "ok vm_create tab=" + std::to_string(tabId)
                     + " gen=" + std::to_string(newGen) + "\n";
                std::fprintf(stderr, "[worker] VM created for tab=%d gen=%llu\n",
                             static_cast<int>(tabId), static_cast<unsigned long long>(newGen));
            } catch (...) {
                resp = "err invalid tab id\n";
            }
        } else if (cmd.rfind("vm_destroy", 0) == 0) {
            // Format: vm_destroy <tabId>
            std::string rest = cmd.substr(10);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t' ||
                                     rest.front() == '\n' || rest.front() == '\r'))
                rest.erase(0, 1);
            try {
                TabId tabId = static_cast<TabId>(std::stoi(rest));
                gVmLifecycle.vmDestroy(tabId);
                resp = "ok vm_destroy tab=" + std::to_string(tabId) + "\n";
                std::fprintf(stderr, "[worker] VM destroyed for tab=%d\n", static_cast<int>(tabId));
            } catch (...) {
                resp = "err invalid tab id\n";
            }
        } else if (cmd.rfind("ck_compile", 0) == 0) {
            // Format: ck_compile <tabId> <vmGeneration> <version> <source...>
            //
            // enqueues a compile on the ChuckCompiler dispatcher thread.
            // The compile happens off the control thread and off any
            // render thread — it is NOT on the real-time audio path.
            std::string rest = cmd.substr(10);
            while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
                rest.erase(0, 1);

            // Parse: tabId vmGeneration version source
            // The source may contain spaces, so we parse the first 3
            // tokens as integers and treat the remainder as source.
            std::string_view sv = rest;
            auto parseToken = [](std::string_view& sv) -> std::string_view {
                while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                    sv.remove_prefix(1);
                size_t end = 0;
                while (end < sv.size() && sv[end] != ' ' && sv[end] != '\t')
                    ++end;
                std::string_view tok = sv.substr(0, end);
                sv.remove_prefix(end);
                return tok;
            };

            std::string_view tabStr = parseToken(sv);
            std::string_view genStr = parseToken(sv);
            std::string_view verStr = parseToken(sv);
            std::string_view srcStr = sv;
            // trim trailing whitespace from source
            while (!srcStr.empty() &&
                   (srcStr.back() == ' ' || srcStr.back() == '\t' ||
                    srcStr.back() == '\n' || srcStr.back() == '\r'))
                srcStr.remove_suffix(1);

            try {
                int tabIdInt = std::stoi(std::string(tabStr));
                TabId tabId = static_cast<TabId>(tabIdInt);
                uint64_t vmGeneration = std::stoull(std::string(genStr));
                [[maybe_unused]] uint32_t version = static_cast<uint32_t>(std::stoul(std::string(verStr)));

                if (srcStr.empty()) {
                    resp = "err ck_compile: empty source\n";
                } else {
                    // Bump the request version atomically — this ensures
                    // stale results from prior compile requests for the
                    // same tab are rejected by the render thread.
                    uint32_t bumpedVer = gVmLifecycle.bumpRequestVersion(tabId);

                    // Enqueue on the dispatcher thread. The compile happens
                    // off the control thread. The response (sent back via
                    // the socket) is delivered after the compile completes.
                    gCompiler->enqueue(CompileCommand{
                        .tabId = tabId,
                        .requestVersion = bumpedVer,
                        .vmGeneration = vmGeneration,
                        .sourceCode = std::string(srcStr),
                        .onResponse = [connFd, tabId, bumpedVer](
                            std::shared_ptr<CompiledShred> result) {
                            std::string r;
                            if (result && result->ok) {
                                r = "ok ck_compile tab=" + std::to_string(tabId)
                                  + " version=" + std::to_string(bumpedVer)
                                  + " hash=" + std::to_string(result->sourceHash)
                                  + " shred_id=assigned_on_next_vm_loop\n";
                            } else {
                                std::string errMsg = result ? result->error : "compile failed";
                                r = "err ck_compile tab=" + std::to_string(tabId)
                                  + " version=" + std::to_string(bumpedVer)
                                  + " error=" + errMsg
                                  + " line=" + std::to_string(result ? result->errorLine : 0)
                                  + " col=" + std::to_string(result ? result->errorColumn : 0)
                                  + "\n";
                            }
                            ::write(connFd, r.c_str(), r.size());
                            ::close(connFd);
                        }
                    });
                    // The socket fd is closed by the onResponse callback.
                    // Skip the normal response at the bottom.
                    continue;
                }
            } catch (...) {
                resp = "err invalid ck_compile arguments\n";
            }
        } else {
            resp = "err unknown command\n";
        }
        ::write(connFd, resp.c_str(), resp.size());
        ::close(connFd);
    }

    ::close(srvFd);
    ::unlink(ctrlPath.c_str());
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

    // Current worker generation — used by the render loop to detect
    // generation changes (worker restart invalidation).
    const uint64_t myGen = gTransport->generation.load(std::memory_order_acquire);

    // B4-K4: create the serialized compiler dispatcher thread.
    // This thread is the sole caller of any ChucK compile API (K0.5 NO-GO).
    // It publishes results via std::atomic_store_explicit into VM handoff
    // slots, and the per-tab render threads consume via
    // std::atomic_load_explicit.
    gCompiler = std::make_unique<ChuckCompiler>(
        [](TabId tabId, uint64_t vmGeneration) -> ChuckVmEntry* {
            return gVmLifecycle.lookupForCompile(tabId, vmGeneration);
        });

    // Start the control-plane listener thread.
    std::thread ctrlThread(controlPlaneThread);

    // Run the audio production loop on the main thread.
    // B4-K3 will replace this with per-tab VM render threads.
    // The render loop checks the handoff slot each iteration.
    audioProductionLoop(myGen);

    // Signal the control plane to shut down.
    gRunning.store(false, std::memory_order_release);

    // Shut down the compiler (drains queued compile commands).
    if (gCompiler) {
        gCompiler->shutdown();
        gCompiler.reset();
    }

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
