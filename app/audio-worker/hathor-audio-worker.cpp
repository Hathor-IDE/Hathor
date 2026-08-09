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
 * B4-K3: per-tab Chuck_VM isolation — each active .ck tab owns one ChuckVM,
 * one dedicated OS thread, one watchdog attachment point.  A failure in one
 * VM never silences another.  Resource policy is data-driven (Decision #24).
 *
 * B4-K4: .ck compilation happens on the ChuckCompiler dispatcher thread.
 * Compiled shreds are published via std::atomic_store_explicit(release)
 * into the per-tab VM's handoffShred slot, and consumed on the next loop
 * iteration via std::atomic_load_explicit(acquire).
 *
 * Architecture:
 *   - argv[1] = control-plane Unix socket path (passed by parent)
 *   - Fixed shared-memory name /hathor-audio-worker (from audio_ipc.h)
 *   - Parent initialises SHM and sets generation BEFORE spawning this worker
 *   - Worker reads generation at startup, becomes the sole producer
 *
 * Thread layout:
 *   - Control plane thread: receives socket commands (vm_activate, vm_create,
 *     vm_deactivate, vm_resume, vm_destroy, ck_compile), delegates to VMManager
 *     and ChuckCompiler.
 *   - Compile dispatcher thread (ChuckCompiler): sole thread calling ChucK
 *     compile APIs. Serializes all compilation.
 *   - Per-tab render threads (ChuckVM): one OS thread per active VM.
 *     Calls run() via render callback, consumes handoff shreds, publishes
 *     audio to the shared-memory ring.
 *
 * Control-plane protocol (see audio_ipc.h for full spec):
 *   vm_activate <tabId> <sampleRate> <channels>     — create/resume VM
 *   vm_deactivate <tabId> [suspend|destroy]          — suspend/destroy VM
 *   vm_resume <tabId>                                — resume suspended VM
 *   vm_destroy <tabId>                               — full destroy + remove
 *   vm_query <tabId>                                 — query VM state
 *   vm_list                                            — list all VMs
 *   vm_create <tabId>                                — legacy alias for vm_activate
 *   ck_compile <tabId> <vmGeneration> <version> <src> — compile (K4, serialized)
 *   ck_stop <tabId>                                      — stop .ck tab (K7)
 *   status, ping, stop                                 — lifecycle/standard
 *
 * Requirements: B4-K2, B4-K3, B4-K4, B4-K0.5, B4-K0.6, Decision #24
 */

#include "audio_ipc.h"
#include "ChuckCompiler.hpp"
#include "ChuckVm.hpp"
#include "VMManager.hpp"
#include "VmLifecycle.hpp"
#include "VmWatchdog.hpp"
#include "ResourcePolicy.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cctype>
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
using hathor::audio_worker::kNumTabs;
using hathor::audio_worker::kRingCapacity;
using hathor::audio_worker::kRingMask;
using hathor::audio_worker::kShmName;
using hathor::audio_worker::kShmSize;
using hathor::audio_worker::ChuckCompiler;
using hathor::audio_worker::ChuckVmEntry;
using hathor::audio_worker::ChuckVM;
using hathor::audio_worker::CompileCommand;
using hathor::audio_worker::CompiledShred;
using hathor::audio_worker::ResourcePolicy;
using hathor::audio_worker::TabId;
using hathor::audio_worker::VMManager;
using hathor::audio_worker::VMResult;
using hathor::audio_worker::VmLifecycle;
using hathor::audio_worker::VmState;
using hathor::audio_worker::VmWatchdog;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static std::atomic<bool> gRunning{true};
static SharedAudioTransport* gTransport = nullptr;
static size_t gShmSize = 0;
static int gShmFd = -1;
static std::string gControlSocketPath;

// Per-tab VM lifecycle manager (table + handoff slots for K4).
static VmLifecycle gVmLifecycle;

// K3: VM manager — manages ChuckVM instances with per-tab OS threads,
// resource policy enforcement, and lifecycle state machine.
static VMManager gVmManager;

// B4-K4: serialized compiler — the sole caller of any ChucK compile API.
static std::unique_ptr<ChuckCompiler> gCompiler;

// B4-K5: per-VM watchdog for hang detection and recovery.
static std::unique_ptr<VmWatchdog> gWatchdog;

// B4-K5: hang event log — records recent hang detection events for the
// control-plane "vm_hang_status" command.  Accessed from the watchdog thread
// (writer) and the control-plane thread (reader).  Protected by a mutex.
struct HangEvent {
    TabId tabId;
    uint64_t oldGeneration;
    uint64_t heartbeatValue;
    std::chrono::steady_clock::time_point detectionTime;
    uint64_t newGeneration;
    bool recovered;
};
static std::vector<HangEvent> gHangEvents;
static std::mutex gHangEventsMtx;

// B4-K5: last hang notification per tab (for quick status queries).
static std::unordered_map<TabId, HangEvent> gLastHangPerTab;
static std::mutex gLastHangMtx;

static void handleSigterm(int /*signum*/) {
    gRunning.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Per-tab render callback — used by ChuckVM to produce audio into the ring
// ---------------------------------------------------------------------------

static void perTabRenderCallback(TabId tabId, float* /*outBuf*/,
                                  unsigned numFrames, unsigned /*numChannels*/,
                                  uint64_t expectedGen)
{
    // Check generation liveness — if the worker was restarted, return.
    const uint64_t gen = gTransport->generation.load(std::memory_order_acquire);
    if (gen != expectedGen)
        return;

    // Produce a block into the ring buffer (placeholder tone per tab).
    const uint32_t wSeq = gTransport->writeSeq.load(std::memory_order_relaxed);
    const uint32_t rSeq = gTransport->readSeq.load(std::memory_order_acquire);

    if (wSeq - rSeq >= kRingCapacity) {
        const uint32_t newRSeq = wSeq - kRingCapacity + 1u;
        gTransport->readSeq.store(newRSeq, std::memory_order_release);
    }

    AudioBlock& block = gTransport->blocks[wSeq & kRingMask];
    block.sequence.store((wSeq << 1) | 1u, std::memory_order_release);

    // Placeholder: deterministic tone per tab (silence for non-active tabs).
    const float basePhase = static_cast<float>(tabId) * 0.1f;
    const float freq = 220.0f + static_cast<float>(tabId) * 22.0f;
    const float phaseInc = freq * 2.0f * 3.14159265f / 44100.0f;

    for (uint32_t i = 0; i < numFrames; ++i) {
        const float phase = basePhase + static_cast<float>(wSeq * kBlockSize + i) * phaseInc;
        block.samples[i] = std::sin(phase) * 0.05f;
    }

    block.sequence.store((wSeq << 1) + 2u, std::memory_order_release);
    gTransport->writeSeq.store(wSeq + 1u, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Audio production — legacy placeholder (used when no VMs are active)
// ---------------------------------------------------------------------------

static void produceBlock(AudioBlock& block, uint32_t wSeq, uint64_t gen) {
    block.sequence.store((wSeq << 1) | 1u, std::memory_order_release);

    const float basePhase = static_cast<float>(gen) * 0.01f;
    const float freq      = 220.0f + static_cast<float>(gen) * 11.0f;
    const float phaseInc  = freq * 2.0f * 3.14159265f / 44100.0f;

    for (uint32_t i = 0; i < kBlockSize; ++i) {
        const float phase = basePhase + static_cast<float>(wSeq * kBlockSize + i) * phaseInc;
        block.samples[i] = std::sin(phase) * 0.1f;
    }

    block.sequence.store((wSeq << 1) + 2u, std::memory_order_release);
}

static void placeholderProductionLoop(uint64_t expectedGen) {
    while (gRunning.load(std::memory_order_acquire)) {
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

    if (::listen(srvFd, 8) != 0) {
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
            if (cmd.size() >= sizeof(buf)) break;
        }
        while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
            cmd.pop_back();

        std::string resp;

        auto parseToken = [](std::string_view& sv) -> std::string_view {
            while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
                sv.remove_prefix(1);
            size_t end = 0;
            while (end < sv.size() && !std::isspace(static_cast<unsigned char>(sv[end])))
                ++end;
            std::string_view tok = sv.substr(0, end);
            sv.remove_prefix(end);
            return tok;
        };

        auto trimSpaces = [](std::string& s) {
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
                s.erase(0, 1);
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
                s.pop_back();
        };

        // -----------------------------------------------------------------
        // Standard lifecycle commands
        // -----------------------------------------------------------------
        if (cmd == "stop") {
            resp = "ok stopping\n";
            ::write(connFd, resp.c_str(), resp.size());
            ::close(connFd);
            gRunning.store(false, std::memory_order_release);
            continue;
        } else if (cmd == "ping") {
            resp = "ok pong\n";
        } else if (cmd == "status") {
            const uint64_t gen = gTransport->generation.load(std::memory_order_acquire);
            const uint32_t wSeq = gTransport->writeSeq.load(std::memory_order_acquire);
            const uint32_t rSeq = gTransport->readSeq.load(std::memory_order_acquire);
            const uint64_t beat = gTransport->lastHeartbeat.load(std::memory_order_acquire);
            int activeVms = gVmManager.countActive();
            resp = "ok gen=" + std::to_string(gen)
                 + " wSeq=" + std::to_string(wSeq)
                 + " rSeq=" + std::to_string(rSeq)
                 + " beat=" + std::to_string(beat)
                 + " active_vms=" + std::to_string(activeVms) + "\n";
        }
        // -----------------------------------------------------------------
        // B4-K3: VM lifecycle commands
        // -----------------------------------------------------------------
        else if (cmd.rfind("vm_activate", 0) == 0) {
            std::string rest = cmd.substr(11);
            trimSpaces(rest);
            std::string_view sv = rest;

            std::string_view tabStr = parseToken(sv);
            std::string_view srStr  = parseToken(sv);
            std::string_view chStr  = parseToken(sv);

            try {
                int tabId = std::stoi(std::string(tabStr));
                unsigned sampleRate = srStr.empty() ? 44100 : std::stoul(std::string(srStr));
                unsigned channels = chStr.empty() ? 1 : std::stoul(std::string(chStr));

                if (tabId < 0 || tabId >= kNumTabs) {
                    resp = "err vm_activate: tab id out of range [0," + std::to_string(kNumTabs) + ")\n";
                } else {
                    uint64_t vmGen = gVmLifecycle.vmCreate(static_cast<TabId>(tabId));

                    // Set up the render callback for this VM.
                    const uint64_t genForCb = gTransport->generation.load(std::memory_order_acquire);
                    gVmManager.setRenderCallback(
                        [tabId, genForCb]
                        (float* outBuf, unsigned numFrames, unsigned numChannels) {
                            perTabRenderCallback(static_cast<TabId>(tabId), outBuf,
                                                  numFrames, numChannels, genForCb);
                        });

                    // B4-K7: Wire the handoff loader so ChuckVM's render thread
                    // can consume compiled shreds from VmLifecycle::loadHandoff().
                    gVmManager.setHandoffLoader(
                        [tabId]() {
                            return gVmLifecycle.loadHandoff(static_cast<TabId>(tabId));
                        });

                    auto result = gVmManager.activateVM(static_cast<TabId>(tabId),
                                                          sampleRate, channels);
                     if (result.ok) {
                         // B4-K5: register the VM with the watchdog.
                         if (gWatchdog) {
                             gWatchdog->registerVM(static_cast<TabId>(tabId), vmGen);
                         }
                         resp = "ok vm_activated tab=" + std::to_string(tabId)
                              + " gen=" + std::to_string(vmGen)
                              + " state=active\n";
                     } else {
                        resp = "err vm_activate_failed tab=" + std::to_string(tabId)
                             + " code=" + std::to_string(result.errorCode)
                             + " " + result.message + "\n";
                    }
                }
            } catch (...) {
                resp = "err invalid vm_activate arguments\n";
            }
        }
        else if (cmd.rfind("vm_create", 0) == 0) {
            // Legacy alias for vm_activate (kept for backward compatibility).
            std::string rest = cmd.substr(9);
            trimSpaces(rest);
            try {
                int tabId = std::stoi(rest);
                if (tabId < 0 || tabId >= kNumTabs) {
                    resp = "err vm_create: tab id out of range\n";
                } else {
                    uint64_t vmGen = gVmLifecycle.vmCreate(static_cast<TabId>(tabId));

                    const uint64_t genForCb = gTransport->generation.load(std::memory_order_acquire);
                    gVmManager.setRenderCallback(
                        [tabId, genForCb]
                        (float* outBuf, unsigned numFrames, unsigned numChannels) {
                            perTabRenderCallback(static_cast<TabId>(tabId), outBuf,
                                                  numFrames, numChannels, genForCb);
                        });

                    // B4-K7: Wire the handoff loader for compiled shred consumption.
                    gVmManager.setHandoffLoader(
                        [tabId]() {
                            return gVmLifecycle.loadHandoff(static_cast<TabId>(tabId));
                        });

                    auto result = gVmManager.activateVM(static_cast<TabId>(tabId));
                    if (result.ok) {
                        // B4-K5: register the VM with the watchdog.
                        if (gWatchdog) {
                            gWatchdog->registerVM(static_cast<TabId>(tabId), vmGen);
                        }
                        resp = "ok vm_create tab=" + std::to_string(tabId)
                             + " gen=" + std::to_string(vmGen) + "\n";
                    } else {
                        resp = "err vm_create_failed tab=" + std::to_string(tabId)
                             + " " + result.message + "\n";
                    }
                }
            } catch (...) {
                resp = "err invalid tab id\n";
            }
        }
        else if (cmd.rfind("vm_deactivate", 0) == 0) {
            std::string rest = cmd.substr(13);
            trimSpaces(rest);
            std::string_view sv = rest;

            std::string_view tabStr = parseToken(sv);
            std::string_view modeStr = parseToken(sv);

            try {
                int tabId = std::stoi(std::string(tabStr));
                bool suspend = true;
                if (!modeStr.empty()) {
                    std::string mode(static_cast<std::string>(modeStr));
                    if (mode == "destroy") suspend = false;
                }

                 auto result = gVmManager.deactivateVM(static_cast<TabId>(tabId), suspend);
                 if (result.ok) {
                     // B4-K5: unregister from the watchdog when deactivating.
                     if (gWatchdog) {
                         gWatchdog->unregisterVM(static_cast<TabId>(tabId));
                     }
                     resp = "ok vm_deactivated tab=" + std::to_string(tabId)
                          + " " + result.message + "\n";
                } else {
                    resp = "err vm_deactivate_failed tab=" + std::to_string(tabId)
                         + " " + result.message + "\n";
                }
            } catch (...) {
                resp = "err invalid vm_deactivate arguments\n";
            }
        }
        else if (cmd.rfind("vm_resume", 0) == 0) {
            std::string rest = cmd.substr(9);
            trimSpaces(rest);
            try {
                int tabId = std::stoi(rest);
                if (tabId < 0 || tabId >= kNumTabs) {
                    resp = "err vm_resume: tab id out of range\n";
                } else {
                    auto result = gVmManager.resumeVM(static_cast<TabId>(tabId));
                    if (result.ok) {
                        // B4-K5: re-register with the watchdog and reset the
                        // heartbeat baseline so the resumed VM doesn't get
                        // immediately classified as hung (its heartbeat was
                        // unchanged while suspended).
                        if (gWatchdog) {
                            uint64_t gen = gVmLifecycle.generationOf(static_cast<TabId>(tabId));
                            gWatchdog->registerVM(static_cast<TabId>(tabId), gen);
                            gWatchdog->resetHeartbeat(static_cast<TabId>(tabId));
                        }
                        resp = "ok vm_resumed tab=" + std::to_string(tabId)
                             + " state=active\n";
                    } else {
                        resp = "err vm_resume_failed tab=" + std::to_string(tabId)
                             + " " + result.message + "\n";
                    }
                }
            } catch (...) {
                resp = "err invalid vm_resume arguments\n";
            }
        }
        // -----------------------------------------------------------------
        // B4-K5: Per-VM hang detection status and watchdog control
        // -----------------------------------------------------------------
        else if (cmd.rfind("vm_hang_status", 0) == 0) {
            // Query the watchdog's hang event log for a specific tab or all tabs.
            std::string rest = cmd.substr(14);
            trimSpaces(rest);

            if (rest.empty() || rest == "all") {
                // Return all recent hang events.
                std::lock_guard<std::mutex> lock(gHangEventsMtx);
                std::string events;
                for (const auto& e : gHangEvents) {
                    char buf[256];
                    std::snprintf(buf, sizeof(buf),
                        "tab=%u old_gen=%llu hb=%llu new_gen=%llu recovered=%d ",
                        static_cast<unsigned>(e.tabId),
                        static_cast<unsigned long long>(e.oldGeneration),
                        static_cast<unsigned long long>(e.heartbeatValue),
                        static_cast<unsigned long long>(e.newGeneration),
                        e.recovered ? 1 : 0);
                    events += buf;
                }
                resp = "ok vm_hang_status events=" + std::to_string(gHangEvents.size())
                     + " " + events + "\n";
            } else {
                // Query a specific tab.
                int tabId = -1;
                try {
                    tabId = std::stoi(rest);
                } catch (...) {
                    resp = "err invalid vm_hang_status arguments\n";
                    ::write(connFd, resp.c_str(), resp.size());
                    ::close(connFd);
                    continue;
                }
                std::lock_guard<std::mutex> lock(gLastHangMtx);
                auto it = gLastHangPerTab.find(static_cast<TabId>(tabId));
                if (it != gLastHangPerTab.end()) {
                    const auto& e = it->second;
                    resp = "ok vm_hang_status tab=" + std::to_string(tabId)
                         + " old_gen=" + std::to_string(e.oldGeneration)
                         + " hb=" + std::to_string(e.heartbeatValue)
                         + " new_gen=" + std::to_string(e.newGeneration)
                         + " recovered=" + (e.recovered ? "1" : "0")
                         + " restarts=" + std::to_string(
                             gWatchdog ? gWatchdog->restartCountFor(static_cast<TabId>(tabId)) : 0)
                         + "\n";
                } else {
                    resp = "ok vm_hang_status tab=" + std::to_string(tabId)
                         + " no_hang_events\n";
                }
            }
        }
        else if (cmd.rfind("watchdog_status", 0) == 0) {
            resp = "ok watchdog monitored=" + std::to_string(
                gWatchdog ? gWatchdog->monitoredCount() : 0)
                 + " total_detections=" + std::to_string(
                     gWatchdog ? gWatchdog->totalHangDetections() : 0)
                 + "\n";
        }
        else if (cmd.rfind("watchdog_timeout", 0) == 0) {
            // Configure the watchdog timeout (milliseconds).
            std::string rest = cmd.substr(16);
            trimSpaces(rest);
            try {
                int ms = std::stoi(rest);
                if (gWatchdog) gWatchdog->setTimeout(std::chrono::milliseconds(ms));
                resp = "ok watchdog_timeout ms=" + std::to_string(ms) + "\n";
            } catch (...) {
                resp = "err watchdog_timeout: invalid argument\n";
            }
        }
        else if (cmd.rfind("watchdog_interval", 0) == 0) {
            // Configure the watchdog check interval (milliseconds).
            std::string rest = cmd.substr(18);
            trimSpaces(rest);
            try {
                int ms = std::stoi(rest);
                if (gWatchdog) gWatchdog->setInterval(std::chrono::milliseconds(ms));
                resp = "ok watchdog_interval ms=" + std::to_string(ms) + "\n";
            } catch (...) {
                resp = "err watchdog_interval: invalid argument\n";
            }
        }
        else if (cmd.rfind("vm_destroy", 0) == 0) {
            std::string rest = cmd.substr(10);
            trimSpaces(rest);
            try {
                int tabId = std::stoi(rest);
                if (tabId < 0 || tabId >= kNumTabs) {
                    resp = "err vm_destroy: tab id out of range\n";
                 } else {
                     // B4-K5: unregister from the watchdog when destroying.
                     if (gWatchdog) {
                         gWatchdog->unregisterVM(static_cast<TabId>(tabId));
                     }
                     gVmLifecycle.vmDestroy(tabId);
                     gVmManager.destroyVM(static_cast<TabId>(tabId));
                     resp = "ok vm_destroyed tab=" + std::to_string(tabId) + "\n";
                }
            } catch (...) {
                resp = "err invalid vm_destroy arguments\n";
            }
        }
        else if (cmd.rfind("vm_query", 0) == 0) {
            std::string rest = cmd.substr(8);
            trimSpaces(rest);
            try {
                int tabId = std::stoi(rest);
                if (tabId < 0 || tabId >= kNumTabs) {
                    resp = "err vm_query: tab id out of range\n";
                } else {
                    VMResult qr = gVmManager.queryVM(static_cast<TabId>(tabId));
                    resp = (qr.ok ? "ok " : "err ") + qr.message + "\n";
                }
            } catch (...) {
                resp = "err invalid vm_query arguments\n";
            }
        }
        else if (cmd == "vm_list") {
            resp = gVmManager.listVMs() + "\n";
        }
        else if (cmd.rfind("ck_genv", 0) == 0) {
            // B4-K7: Query the current VM generation for a tab.
            // The main process needs the real generation to send to ck_compile
            // so the dispatcher's generation-mismatch check passes.
            std::string rest = cmd.substr(7);
            trimSpaces(rest);
            try {
                int tabId = std::stoi(rest);
                if (tabId < 0 || tabId >= kNumTabs) {
                    resp = "err ck_genv: tab id out of range [0," + std::to_string(kNumTabs) + ")\n";
                } else {
                    uint64_t gen = gVmLifecycle.generationOf(tabId);
                    uint64_t ver = gVmLifecycle.currentVersionOf(tabId);
                    bool active = gVmLifecycle.hasActiveVm(tabId);
                    resp = "ok ck_genv tab=" + std::to_string(tabId)
                         + " gen=" + std::to_string(gen)
                         + " version=" + std::to_string(ver)
                         + " active=" + (active ? "true" : "false") + "\n";
                }
            } catch (...) {
                resp = "err invalid ck_genv arguments\n";
            }
        }
        else if (cmd.rfind("ck_compile", 0) == 0) {
            std::string rest = cmd.substr(10);
            trimSpaces(rest);
            std::string_view sv = rest;

            std::string_view tabStr = parseToken(sv);
            std::string_view genStr = parseToken(sv);
            std::string_view verStr = parseToken(sv);
            std::string_view srcStr = sv;
            while (!srcStr.empty() &&
                   std::isspace(static_cast<unsigned char>(srcStr.back())))
                srcStr.remove_suffix(1);

            try {
                int tabId = std::stoi(std::string(tabStr));
                uint64_t vmGeneration = std::stoull(std::string(genStr));
                uint32_t version = static_cast<uint32_t>(std::stoul(std::string(verStr)));
                (void)version;

                if (srcStr.empty()) {
                    resp = "err ck_compile: empty source\n";
                } else {
                    uint32_t bumpedVer = gVmLifecycle.bumpRequestVersion(static_cast<TabId>(tabId));

                    gCompiler->enqueue(CompileCommand{
                        .tabId = static_cast<TabId>(tabId),
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
                    continue;
                }
            } catch (...) {
                resp = "err invalid ck_compile arguments\n";
            }
        }
        else if (cmd.rfind("ck_stop", 0) == 0) {
            // B4-K7: Stop a .ck tab — deactivate (suspend) the per-tab VM
            // and remove any pending handoff shred.  This is the .ck stop
            // path invoked by the Play/Stop button on a .ck tab.
            std::string rest = cmd.substr(7);
            trimSpaces(rest);
            try {
                int tabId = std::stoi(rest);
                if (tabId < 0 || tabId >= kNumTabs) {
                    resp = "err ck_stop: tab id out of range [0," + std::to_string(kNumTabs) + ")\n";
                } else {
                    // Unregister from watchdog if present.
                    if (gWatchdog) {
                        gWatchdog->unregisterVM(static_cast<TabId>(tabId));
                    }
                    // Destroy the VM (full teardown — shred is discarded).
                    gVmLifecycle.vmDestroy(static_cast<TabId>(tabId));
                    auto result = gVmManager.destroyVM(static_cast<TabId>(tabId));
                    if (result.ok) {
                        resp = "ok ck_stopped tab=" + std::to_string(tabId) + "\n";
                    } else {
                        resp = "err ck_stop_failed tab=" + std::to_string(tabId)
                             + " " + result.message + "\n";
                    }
                }
            } catch (...) {
                resp = "err invalid ck_stop arguments\n";
            }
        }
        else if (cmd.rfind("policy", 0) == 0) {
            std::string rest = cmd.substr(6);
            trimSpaces(rest);
            if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"')
                rest = rest.substr(1, rest.size() - 2);

            ResourcePolicy policy;
            if (policy.deserialize(rest)) {
                gVmManager.setPolicy(policy);
                resp = "ok policy_set max_vms=" + std::to_string(policy.maxConcurrentLiveVMs) + "\n";
            } else {
                resp = "err policy_parse_failed\n";
            }
        }
        // -----------------------------------------------------------------
        // B4-K8: Test-mode commands (for hard gate tests only — NOT for
        // production use; these intentionally exercise safety boundaries).
        // -----------------------------------------------------------------
        else if (cmd == "test_crash_worker") {
            // Deliberately crash the worker process (SIGSEGV) to test that
            // the main process detects the native crash, does not crash itself,
            // and can recover via restart.
            ::raise(SIGSEGV);
            resp = "err test_crash_worker: should not reach here\n";
        }
        else if (cmd == "test_hang_worker") {
            // Make the entire worker spin indefinitely without advancing
            // the heartbeat. Simulates a worker-wide hang (not just one VM).
            // The main process should detect heartbeat staleness and restart.
            // We spin on a non-SIGTERM-catchable loop.
            while (true) {
                ::pause();
                // Even if woken by a signal, keep spinning.
            }
            resp = "err test_hang_worker: should not reach here\n";
        }
        else if (cmd.rfind("test_hang_vm", 0) == 0) {
            // Inject a hung shred into a specific VM's render callback.
            // This simulates a ChucK shred running while(true){} with no
            // now =>. The VM's render thread spins, stalling its heartbeat.
            // The per-VM watchdog (B4-K5) should detect and recover.
            std::string rest = cmd.substr(13);
            trimSpaces(rest);
            try {
                int tabId = std::stoi(rest);
                if (tabId < 0 || tabId >= kNumTabs) {
                    resp = "err test_hang_vm: tab id out of range\n";
                } else {
                    gVmManager.forceSetHangingCallback(static_cast<TabId>(tabId));
                    resp = "ok test_hang_vm tab=" + std::to_string(tabId) + "\n";
                }
            } catch (...) {
                resp = "err test_hang_vm: invalid arguments\n";
            }
        }
        else if (cmd.rfind("test_clear_hang_vm", 0) == 0) {
            // Clear the hang on a specific VM, allowing cooperative join.
            std::string rest = cmd.substr(19);
            trimSpaces(rest);
            try {
                int tabId = std::stoi(rest);
                gVmManager.clearHangingCallback(static_cast<TabId>(tabId));
                resp = "ok test_clear_hang_vm tab=" + std::to_string(tabId) + "\n";
            } catch (...) {
                resp = "err test_clear_hang_vm: invalid arguments\n";
            }
        }
        else {
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
    gShmFd = ::shm_open(kShmName, O_RDWR, 0600);
    if (gShmFd < 0) {
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

    const uint64_t gen = gTransport->generation.load(std::memory_order_acquire);

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
    ::signal(SIGTERM, handleSigterm);
    ::signal(SIGINT, handleSigterm);

    if (argc >= 2) {
        gControlSocketPath = argv[1];
    }

    if (!initSharedMemory()) {
        std::fprintf(stderr, "[worker] failed to initialise shared memory\n");
        return 1;
    }

    const uint64_t myGen = gTransport->generation.load(std::memory_order_acquire);

    // B4-K4: create the serialized compiler dispatcher thread.
    gCompiler = std::make_unique<ChuckCompiler>(
        [](TabId tabId, uint64_t vmGeneration) -> ChuckVmEntry* {
            return gVmLifecycle.lookupForCompile(tabId, vmGeneration);
        });

    // B4-K5: create and start the per-VM watchdog.
    // The watchdog callbacks record hang events for UI notification via the
    // control plane (vm_hang_status command).
    gWatchdog = std::make_unique<VmWatchdog>(
        &gVmManager,
        &gVmLifecycle,
        // onHangDetected callback — records the event and fires a one-shot
        // notification to any control-plane client waiting for it.
        [](TabId tabId, uint64_t oldGen, uint64_t beat, std::chrono::steady_clock::time_point when) {
            HangEvent evt{};
            evt.tabId = tabId;
            evt.oldGeneration = oldGen;
            evt.heartbeatValue = beat;
            evt.detectionTime = when;
            evt.newGeneration = 0;
            evt.recovered = false;

            {
                std::lock_guard<std::mutex> lock(gHangEventsMtx);
                gHangEvents.push_back(evt);
            }
            {
                std::lock_guard<std::mutex> lock(gLastHangMtx);
                gLastHangPerTab[tabId] = evt;
            }

            std::fprintf(stderr,
                         "[watchdog] hang notification: tab=%u old_gen=%llu hb=%llu\n",
                         static_cast<unsigned>(tabId),
                         static_cast<unsigned long long>(oldGen),
                         static_cast<unsigned long long>(beat));
        },
        // onRecoveryComplete callback — records the new generation.
        [](TabId tabId, uint64_t newGen) {
            std::lock_guard<std::mutex> lock(gLastHangMtx);
            auto it = gLastHangPerTab.find(tabId);
            if (it != gLastHangPerTab.end()) {
                it->second.newGeneration = newGen;
                it->second.recovered = true;
            }

            std::lock_guard<std::mutex> lock2(gHangEventsMtx);
            if (!gHangEvents.empty()) {
                gHangEvents.back().newGeneration = newGen;
                gHangEvents.back().recovered = true;
            }

            std::fprintf(stderr,
                         "[watchdog] recovery complete: tab=%u new_gen=%llu\n",
                         static_cast<unsigned>(tabId),
                         static_cast<unsigned long long>(newGen));
        }
    );
    gWatchdog->start();

    // Start the control-plane listener thread.
    std::thread ctrlThread(controlPlaneThread);

    // Run production loop (per-tab threads are spawned on vm_activate commands).
    placeholderProductionLoop(myGen);

    // Signal shutdown.
    gRunning.store(false, std::memory_order_release);

    // B4-K5: stop the watchdog before tearing down VMs.
    if (gWatchdog) {
        gWatchdog->stop();
        gWatchdog.reset();
    }

    if (gCompiler) {
        gCompiler->shutdown();
        gCompiler.reset();
    }

    if (gTransport && gTransport != MAP_FAILED) {
        gTransport->workerAlive.store(false, std::memory_order_release);
    }

    std::fprintf(stderr, "[worker] shutting down, pid=%d\n", static_cast<int>(::getpid()));

    if (ctrlThread.joinable())
        ctrlThread.join();

    if (gTransport && gTransport != MAP_FAILED) {
        ::munmap(gTransport, gShmSize);
    }
    if (gShmFd >= 0) {
        ::close(gShmFd);
    }

    // NOTE: We do NOT unlink the SHM here — the parent process owns SHM
    // lifetime and is responsible for shm_unlink after waitpid.
    return 0;
}
