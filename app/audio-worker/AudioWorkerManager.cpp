// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * AudioWorkerManager.cpp — implementation.
 *
 * Implements the out-of-process worker lifecycle, generation tracking,
 * control-plane IPC, RT-safe audio consumption, and stale-memory invalidation.
 *
 * K0.5 conformance: no concurrent compile/run API is exposed.  The control
 * plane is used for lifecycle commands only; ChucK compilation (B4-K4) will
 * later use a serialized command path through this same control plane.
 *
 * Requirements: B4-K2, B4-K0.5, B4-K0.6, Decision #24
 */

#include "AudioWorkerManager.hpp"
#include <atomic>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <vector>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

// Declare environ in the global namespace so we can reference it as ::environ
// on macOS, where <unistd.h> does not expose it in the global namespace.
extern char** environ;

namespace {

using hathor::audio_worker::kBlockSize;
using hathor::audio_worker::kControlName;
using hathor::audio_worker::kMagic;
using hathor::audio_worker::kNumTabs;
using hathor::audio_worker::kRingCapacity;
using hathor::audio_worker::kRingMask;
using hathor::audio_worker::kShmName;
using hathor::audio_worker::kShmSize;
using hathor::audio_worker::SharedAudioTransport;
using hathor::audio_worker::VMResult;

// ---------------------------------------------------------------------------
// Heartbeat staleness check.
// A worker is considered alive if lastHeartbeat advances within this window.
// This is a conservative default; the actual timeout is configurable via
// ResourceLimits::heartbeatTimeoutMs.
// ---------------------------------------------------------------------------
// (no default constant — the timeout is always driven by ResourceLimits)

} // namespace

namespace hathor {

// ---------------------------------------------------------------------------
// Impl — pimpl to keep POSIX headers out of the public header
// ---------------------------------------------------------------------------

struct AudioWorkerManager::Impl {
    // -- Shared-memory transport (mapped pointer, null if unmapped) --
    SharedAudioTransport* transport_    = nullptr;
    int                   shmFd_        = -1;
    size_t                shmSize_      = 0;

    // -- Worker process state --
    pid_t                 workerPid_    = -1;
    uint64_t              generation_   = 0;
    std::atomic<bool>     workerKnownDead_{false};

    // -- Liveness tracking --
    mutable std::mutex    livenessMtx_;
    mutable std::thread   livenessThread_;
    std::atomic<bool>     livenessRunning_{false};

    // -- Last heartbeat observed (for staleness detection) --
    std::atomic<uint64_t> lastHeartbeatSeen_{0};
    std::atomic<WorkerStatus> status_{WorkerStatus::NotStarted};

    // -- Heartbeat staleness tracking --
    std::chrono::steady_clock::time_point lastHeartbeatChange_;

    // -- Error state --
    std::string           lastError_;

    // -- Resource limits (Decision #24) --
    AudioWorkerManager::ResourceLimits resourceLimits_;

    // -- Control socket path (unique per instance to avoid cross-test collision) --
    std::string           controlSocketPath_;

    // -- Last worker path used (for restart) --
    std::string           lastWorkerPath_;

    Impl()
        : resourceLimits_{}
    {
        lastHeartbeatChange_ = std::chrono::steady_clock::now();
    }

    ~Impl()
    {
        cleanupAll();
    }

    void cleanupAll()
    {
        // Stop the liveness thread.
        livenessRunning_.store(false, std::memory_order_release);
        if (livenessThread_.joinable())
            livenessThread_.join();

        // Kill worker if still running.
        if (workerPid_ > 0) {
            // Best-effort SIGTERM, then SIGKILL.
            ::kill(workerPid_, SIGTERM);
            // Brief wait for graceful exit.
            for (int i = 0; i < 20; ++i) {
                int status = 0;
                pid_t r = ::waitpid(workerPid_, &status, WNOHANG);
                if (r == workerPid_) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            // Force kill if still alive.
            int status = 0;
            if (::waitpid(workerPid_, &status, WNOHANG) == 0) {
                ::kill(workerPid_, SIGKILL);
                ::waitpid(workerPid_, &status, 0);
            }
            workerPid_ = -1;
        }

        // Unmap and unlink shared memory.
        if (transport_ && transport_ != MAP_FAILED) {
            ::munmap(transport_, shmSize_);
            transport_ = nullptr;
        }
        if (shmFd_ >= 0) {
            ::close(shmFd_);
            shmFd_ = -1;
        }
        // Unlink the shm object so the name is available for the next worker.
        ::shm_unlink(kShmName);

        // Remove control socket.
        if (!controlSocketPath_.empty()) {
            ::unlink(controlSocketPath_.c_str());
        }

        workerKnownDead_.store(false, std::memory_order_relaxed);
        status_.store(WorkerStatus::NotStarted, std::memory_order_relaxed);
    }
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioWorkerManager::AudioWorkerManager()
    : impl_(std::make_unique<Impl>())
{
}

AudioWorkerManager::~AudioWorkerManager()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool AudioWorkerManager::start(std::string workerPath)
{
    // If a worker is already running, shut it down first.
    if (impl_->workerPid_ > 0)
        shutdown();

    impl_->lastError_.clear();

    // Store the worker path for restart() calls.
    if (!workerPath.empty())
        impl_->lastWorkerPath_ = workerPath;

    // Generate a unique control socket path for this instance.
    const char* tmpdir = ::getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0')
        tmpdir = "/tmp";

    // Use PID + atomics counter for uniqueness.
    static std::atomic<int> socketSeq{0};
    const int seq = socketSeq.fetch_add(1, std::memory_order_relaxed);
    impl_->controlSocketPath_ = std::string(tmpdir) + "/hathor-aw-"
                              + std::to_string(::getpid())
                              + "-" + std::to_string(seq) + ".sock";

    // Establish a new generation.  Generation 0 is reserved for "not started";
    // the first real generation is 1.
    const uint64_t newGen = impl_->generation_ + 1;
    if (newGen == 0)
        impl_->generation_ = 1;

    // Initialize shared memory with the new generation (outside RT callback).
    if (!initSharedMemory(newGen)) {
        impl_->lastError_ = "failed to initialize shared memory: " + impl_->lastError_;
        impl_->status_.store(WorkerStatus::StartError, std::memory_order_release);
        return false;
    }

    // Spawn the worker process.
    if (!spawnWorker(workerPath)) {
        impl_->lastError_ = "failed to spawn worker: " + impl_->lastError_;
        impl_->status_.store(WorkerStatus::StartError, std::memory_order_release);
        // Clean up the shared memory we just created.
        cleanupSharedMemory();
        return false;
    }

    // Wait for the worker to report it is alive.
    if (!waitForWorkerStart(3000)) {
        impl_->lastError_ = "worker did not become alive within timeout";
        impl_->status_.store(WorkerStatus::StartError, std::memory_order_release);
        // Kill the unresponsive worker.
        if (impl_->workerPid_ > 0) {
            ::kill(impl_->workerPid_, SIGKILL);
            int status = 0;
            ::waitpid(impl_->workerPid_, &status, 0);
            impl_->workerPid_ = -1;
        }
        cleanupSharedMemory();
        return false;
    }

    impl_->workerKnownDead_.store(false, std::memory_order_relaxed);
    impl_->lastHeartbeatSeen_.store(
        impl_->transport_->lastHeartbeat.load(std::memory_order_acquire),
        std::memory_order_relaxed);
    impl_->status_.store(WorkerStatus::Healthy, std::memory_order_release);

    // Start the liveness monitoring thread (non-RT, non-blocking for audio thread).
    impl_->livenessRunning_.store(true, std::memory_order_release);
    impl_->livenessThread_ = std::thread([this]() {
        while (impl_->livenessRunning_.load(std::memory_order_acquire)) {
            const int timeoutMs = impl_->resourceLimits_.heartbeatTimeoutMs;

            // Check process exit (non-blocking).
            if (checkProcessExit()) {
                impl_->workerKnownDead_.store(true, std::memory_order_release);
                impl_->status_.store(WorkerStatus::Dead, std::memory_order_release);
                return;
            }

            // Check heartbeat staleness (via shared memory atomics only).
            if (impl_->transport_ && impl_->transport_ != MAP_FAILED) {
                const uint64_t beat = impl_->transport_->lastHeartbeat.load(std::memory_order_acquire);
                const uint64_t lastSeen = impl_->lastHeartbeatSeen_.load(std::memory_order_relaxed);
                if (beat == lastSeen) {
                    // Heartbeat hasn't advanced — check how long.
                    const auto now = std::chrono::steady_clock::now();
                    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - impl_->lastHeartbeatChange_).count();

                    if (elapsedMs > timeoutMs) {
                        // Heartbeat stalled longer than the timeout → consider dead.
                        impl_->workerKnownDead_.store(true, std::memory_order_release);
                        impl_->status_.store(WorkerStatus::Dead, std::memory_order_release);
                        return;
                    }
                } else {
                    impl_->lastHeartbeatSeen_.store(beat, std::memory_order_relaxed);
                    impl_->lastHeartbeatChange_ = std::chrono::steady_clock::now();
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    return true;
}

void AudioWorkerManager::shutdown()
{
    // Signal the liveness thread to stop.
    impl_->livenessRunning_.store(false, std::memory_order_release);

    // Join the liveness thread first so it doesn't race with our shutdown logic.
    if (impl_->livenessThread_.joinable())
        impl_->livenessThread_.join();

    // Send "stop" command via control plane (best-effort, only if worker is
    // still alive). Skip if the worker is already known dead — connecting to
    // a dead worker's socket can take up to 500ms (connect retry timeout).
    if (impl_->workerPid_ > 0 && !impl_->workerKnownDead_.load(std::memory_order_acquire)) {
        sendControlCommand("stop", 2000);
    }

    // Wait for the worker to exit gracefully, then force-kill if needed.
    if (impl_->workerPid_ > 0) {
        for (int i = 0; i < 20; ++i) {
            int status = 0;
            pid_t r = ::waitpid(impl_->workerPid_, &status, WNOHANG);
            if (r == impl_->workerPid_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Force kill if still alive.
        int status = 0;
        if (::waitpid(impl_->workerPid_, &status, WNOHANG) == 0) {
            ::kill(impl_->workerPid_, SIGKILL);
            ::waitpid(impl_->workerPid_, &status, 0);
        }
        impl_->workerPid_ = -1;
    }

    // Invalidate transport.
    impl_->workerKnownDead_.store(true, std::memory_order_release);
    impl_->status_.store(WorkerStatus::ShuttingDown, std::memory_order_release);

    // Clean up shared memory.
    cleanupSharedMemory();

    impl_->workerKnownDead_.store(false, std::memory_order_relaxed);
    impl_->status_.store(WorkerStatus::NotStarted, std::memory_order_release);
}

bool AudioWorkerManager::restart()
{
    // Shut down the old worker.
    shutdown();

    // Start a new one — start() increments generation internally.
    // Use the stored worker path from the original start() call.
    return start(impl_->lastWorkerPath_);
}

// ---------------------------------------------------------------------------
// Status / liveness
// ---------------------------------------------------------------------------

AudioWorkerManager::WorkerStatus AudioWorkerManager::status() const noexcept
{
    return impl_->status_.load(std::memory_order_acquire);
}

uint64_t AudioWorkerManager::generation() const noexcept
{
    return impl_->generation_;
}

bool AudioWorkerManager::isWorkerAlive() const noexcept
{
    // Fast path: the liveness thread has already detected death.
    if (impl_->workerKnownDead_.load(std::memory_order_acquire))
        return false;

    // Check the workerAlive flag in shared memory.  After a clean shutdown
    // the worker sets this to false.  After a crash (SIGKILL/SIGSEGV) the
    // flag may remain true, but the liveness thread will detect the death
    // via heartbeat staleness or process-exit (waitpid) and set workerKnownDead_.
    if (impl_->transport_ && impl_->transport_ != MAP_FAILED) {
        if (!impl_->transport_->workerAlive.load(std::memory_order_acquire))
            return false;
    }

    return impl_->status_.load(std::memory_order_acquire) != WorkerStatus::Dead
        && impl_->status_.load(std::memory_order_acquire) != WorkerStatus::NotStarted;
}

std::string AudioWorkerManager::getLastError() const
{
    std::lock_guard<std::mutex> lock(impl_->livenessMtx_);
    return impl_->lastError_;
}

// ---------------------------------------------------------------------------
// Resource policy (Decision #24)
// ---------------------------------------------------------------------------

void AudioWorkerManager::setResourceLimits(const ResourceLimits& limits) noexcept
{
    impl_->resourceLimits_ = limits;
}

AudioWorkerManager::ResourceLimits AudioWorkerManager::getResourceLimits() const noexcept
{
    return impl_->resourceLimits_;
}

// ---------------------------------------------------------------------------
// RT-safe audio consumption (JUCE audio thread ONLY)
// ---------------------------------------------------------------------------

bool AudioWorkerManager::tryReadAudioBlock(float* outBuf, uint32_t blockSize,
                                           uint64_t expectedGen) noexcept
{
    // Step 1: Check generation — reject stale transport immediately.
    // This is a single atomic load — RT-safe.
    uint64_t gen = 0;
    uint32_t magic = 0;

    if (!impl_->transport_ || impl_->transport_ == MAP_FAILED)
        return false;

    magic = impl_->transport_->magic.load(std::memory_order_acquire);
    if (magic != kMagic)
        return false;

    gen = impl_->transport_->generation.load(std::memory_order_acquire);
    if (gen != expectedGen)
        return false; // Stale generation — reject.

    // Step 2: Check workerAlive flag — if the worker has cleanly shut down,
    // don't read any more data.
    if (!impl_->transport_->workerAlive.load(std::memory_order_acquire))
        return false;

     // Step 3: Check if the liveness thread has already marked the worker dead.
     // We do NOT call waitpid() here — that syscall, while non-blocking (WNOHANG),
     // can still take >2ms under kernel contention on macOS, violating the
     // audio-thread latency budget. Instead, we rely on the workerAlive flag
     // in shared memory (set atomically by the worker, cleared by the liveness
     // thread on death detection or by shutdown).
     if (impl_->workerKnownDead_.load(std::memory_order_acquire))
         return false;

    // Step 4: Read sequence numbers.
    uint32_t wSeq = impl_->transport_->writeSeq.load(std::memory_order_acquire);
    uint32_t rSeq = impl_->transport_->readSeq.load(std::memory_order_acquire);

    // Step 5: Check if there's data to read.
    if (rSeq >= wSeq)
        return false; // Ring empty.

    // Step 6: Index into the ring buffer and validate via seqlock.
    const uint32_t slot = rSeq & kRingMask;
    const auto& block = impl_->transport_->blocks[slot];

    uint32_t seq0 = block.sequence.load(std::memory_order_acquire);
    if (seq0 & 1u)
        return false; // Write in progress — fall back to silence.
    if (seq0 != rSeq + 2u)
        return false; // Seqlock mismatch — data not for this read position.

    // Step 7: Copy the audio data (bounded memcpy — RT-safe).
    const uint32_t copySize = (blockSize < kBlockSize) ? blockSize : kBlockSize;
    std::memcpy(outBuf, block.samples, copySize * sizeof(float));

    // Step 8: Validate the block wasn't torn.
    uint32_t seq1 = block.sequence.load(std::memory_order_acquire);
    if (seq1 != seq0)
        return false; // Torn read — reject and fall back to silence.

    // Step 9: Advance the read sequence.
    impl_->transport_->readSeq.store(rSeq + 1u, std::memory_order_release);

    return true;
}

// ---------------------------------------------------------------------------
// Control-plane commands
// ---------------------------------------------------------------------------

std::string AudioWorkerManager::sendControlCommand(std::string_view cmd, int timeoutMs) const
{
    if (impl_->controlSocketPath_.empty())
        return "";

    const int cfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (cfd < 0)
        return "";

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, impl_->controlSocketPath_.c_str(),
                 sizeof(addr.sun_path) - 1);

    // Retry connect for up to 500ms — the worker may not have called
    // listen() yet when waitForWorkerStart returned (workerAlive is set
    // before the control-plane socket is bound).
    auto connectDeadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::min(timeoutMs, 500));
    while (::connect(cfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (errno != ECONNREFUSED && errno != ENOENT) {
            ::close(cfd);
            return "";
        }
        if (std::chrono::steady_clock::now() >= connectDeadline) {
            ::close(cfd);
            return "";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const std::string request = std::string(cmd) + "\n";
    ssize_t written = ::write(cfd, request.c_str(), request.size());
    if (written <= 0) {
        ::close(cfd);
        return "";
    }

    // Poll for response with a bounded timeout.
    struct pollfd pfd{};
    pfd.fd = cfd;
    pfd.events = POLLIN;
    std::string response;
    char buf[256];

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        int ready = ::poll(&pfd, 1, 50);
        if (ready > 0) {
            ssize_t n = ::read(cfd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            response += std::string(buf, static_cast<size_t>(n));
            if (response.find('\n') != std::string::npos) break;
        } else if (ready < 0) {
            break;
        }
    }

    ::close(cfd);

    // Strip trailing newline/CR.
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();

    return response;
}

// -----------------------------------------------------------------------
// B4-K3: Per-tab VM control API
// -----------------------------------------------------------------------

VMResult AudioWorkerManager::activateTabVM(uint8_t tabId, unsigned sampleRate, unsigned channels)
{
    if (tabId >= kNumTabs)
        return {false, 1, "tab id out of range"};
    std::string resp = sendControlCommand(
        "vm_activate " + std::to_string(tabId) + " " + std::to_string(sampleRate) + " " + std::to_string(channels),
        5000);
    if (resp.rfind("ok", 0) == 0)
        return {true, 0, resp};
    return {false, 2, resp};
}

VMResult AudioWorkerManager::deactivateTabVM(uint8_t tabId, bool suspend)
{
    if (tabId >= kNumTabs)
        return {false, 1, "tab id out of range"};
    std::string subCmd = suspend ? "suspend" : "destroy";
    std::string resp = sendControlCommand(
        "vm_deactivate " + std::to_string(tabId) + " " + subCmd, 5000);
    if (resp.rfind("ok", 0) == 0)
        return {true, 0, resp};
    return {false, 2, resp};
}

VMResult AudioWorkerManager::resumeTabVM(uint8_t tabId)
{
    if (tabId >= kNumTabs)
        return {false, 1, "tab id out of range"};
    std::string resp = sendControlCommand("vm_resume " + std::to_string(tabId), 5000);
    if (resp.rfind("ok", 0) == 0)
        return {true, 0, resp};
    return {false, 2, resp};
}

VMResult AudioWorkerManager::destroyTabVM(uint8_t tabId)
{
    if (tabId >= kNumTabs)
        return {false, 1, "tab id out of range"};
    std::string resp = sendControlCommand("vm_destroy " + std::to_string(tabId), 5000);
    if (resp.rfind("ok", 0) == 0)
        return {true, 0, resp};
    return {false, 2, resp};
}

VMResult AudioWorkerManager::compileTabVM(uint8_t tabId, const std::string& code)
{
    if (tabId >= kNumTabs)
        return {false, 1, "tab id out of range"};

    // Query the VM generation for this tab.  If the VM was destroyed/replaced,
    // the worker will return the current generation (0 if no VM exists).
    // The ck_compile command itself handles the generation mismatch check.
    std::string genResp = sendControlCommand(
        "vm_query " + std::to_string(tabId), 5000);

    // Parse generation from the vm_query response if present.
    // The worker returns: "ok tab=<id> state=<state> ... gen=<gen>"
    // If we can't parse it, default to 0 (the worker will handle it).
    uint64_t vmGen = 0;
    {
        // Send a dedicated generation query command.
        std::string genResp2 = sendControlCommand(
            "ck_genv " + std::to_string(tabId), 5000);
        // Parse "gen=<value>" from the response.
        auto pos = genResp2.find("gen=");
        if (pos != std::string::npos) {
            vmGen = std::stoull(genResp2.substr(pos + 4));
        }
    }

    std::string resp = sendControlCommand(
        "ck_compile " + std::to_string(tabId) + " " + std::to_string(vmGen) + " 0 " + code, 5000);
    if (resp.rfind("ok", 0) == 0)
        return {true, 0, resp};
    return {false, 2, resp};
}

// ---------------------------------------------------------------------------
// B4-K7: Evaluate a .ck tab — full compile→load→execute path
// ---------------------------------------------------------------------------

VMResult AudioWorkerManager::evaluateCkTab(uint8_t tabId, const std::string& code)
{
    if (tabId >= kNumTabs)
        return {false, 1, "tab id out of range"};

    // Step 1: Ensure the VM is activated for this tab.
    // If the VM was previously stopped (ck_stop → destroy), this recreates it.
    std::string resp = sendControlCommand(
        "vm_activate " + std::to_string(tabId), 5000);
    if (resp.rfind("ok", 0) != 0) {
        // VM may already be active — that's fine, check state.
        // Fall through to compile; the worker handles already-active VMs.
    }

    // Step 2: Query the current VM generation for this tab.
    // We need the real generation so the compile dispatcher's generation
    // check passes (it rejects results targeting a stale generation).
    std::string genResp = sendControlCommand(
        "ck_genv " + std::to_string(tabId), 5000);

    uint64_t vmGen = 0;
    auto pos = genResp.find("gen=");
    if (pos != std::string::npos) {
        vmGen = std::stoull(genResp.substr(pos + 4));
    }

    // Step 3: Send the compile command with the real generation.
    resp = sendControlCommand(
        "ck_compile " + std::to_string(tabId) + " " + std::to_string(vmGen) + " 0 " + code, 5000);

    if (resp.rfind("ok", 0) == 0) {
        return {true, 0, resp};
    }
    return {false, 2, resp};
}

// ---------------------------------------------------------------------------
// B4-K7: Stop a .ck tab
// ---------------------------------------------------------------------------

VMResult AudioWorkerManager::stopCkTab(uint8_t tabId)
{
    if (tabId >= kNumTabs)
        return {false, 1, "tab id out of range"};

    std::string resp = sendControlCommand("ck_stop " + std::to_string(tabId), 5000);
    if (resp.rfind("ok", 0) == 0)
        return {true, 0, resp};
    return {false, 2, resp};
}

VMResult AudioWorkerManager::queryTabVM(uint8_t tabId) const
{
    if (tabId >= audio_worker::kNumTabs)
        return {false, 1, "tab id out of range"};
    std::string resp = sendControlCommand("vm_query " + std::to_string(tabId), 5000);
    if (resp.rfind("ok", 0) == 0)
        return {true, 0, resp};
    return {false, 2, resp};
}

std::string AudioWorkerManager::listTabVMs() const
{
    return sendControlCommand("vm_list", 5000);
}

// ---------------------------------------------------------------------------
// B4-K5: Per-VM hang detection status (UI notification support)
// ---------------------------------------------------------------------------

std::string AudioWorkerManager::queryHangStatus(int tabId) const
{
    std::string cmd;
    if (tabId < 0) {
        cmd = "vm_hang_status all";
    } else {
        cmd = "vm_hang_status " + std::to_string(tabId);
    }
    return sendControlCommand(cmd, 5000);
}

void AudioWorkerManager::setMaxConcurrentLiveVMs(int maxVms)
{
    // Send a policy update via the control plane.
    // Build a minimal JSON policy string.
    std::string json = "\"{\\\"maxConcurrentLiveVMs\\\":" + std::to_string(maxVms) + "}\"";
    sendControlCommand("policy " + json, 5000);
}

void AudioWorkerManager::invalidateTransport() noexcept
{
    impl_->workerKnownDead_.store(true, std::memory_order_release);
    if (impl_->transport_ && impl_->transport_ != MAP_FAILED) {
        impl_->transport_->workerAlive.store(false, std::memory_order_release);
    }
    impl_->status_.store(WorkerStatus::Dead, std::memory_order_release);
}

bool AudioWorkerManager::isTransportValid(uint64_t expectedGen) const noexcept
{
    if (!impl_->transport_ || impl_->transport_ == MAP_FAILED)
        return false;

    uint32_t magic = impl_->transport_->magic.load(std::memory_order_acquire);
    if (magic != kMagic)
        return false;

    uint64_t gen = impl_->transport_->generation.load(std::memory_order_acquire);
    if (gen != expectedGen)
        return false;

    if (!impl_->transport_->workerAlive.load(std::memory_order_acquire))
        return false;

    return true;
}

pid_t AudioWorkerManager::getWorkerPid() const noexcept
{
    return impl_->workerPid_;
}

void AudioWorkerManager::setHeartbeatTimeoutMs(int timeoutMs) noexcept
{
    if (timeoutMs > 0)
        impl_->resourceLimits_.heartbeatTimeoutMs = timeoutMs;
}

// ---------------------------------------------------------------------------
// Internal: process management
// ---------------------------------------------------------------------------

bool AudioWorkerManager::spawnWorker(const std::string& workerPath)
{
    std::string resolvedPath = workerPath;

    if (resolvedPath.empty()) {
        // Try to find hathor-audio-worker next to the main executable.
        char buf[4096];
        uint32_t size = sizeof(buf);
#ifdef __linux__
        ssize_t n = ::readlink("/proc/self/exe", buf, size - 1);
        if (n > 0) {
            buf[n] = '\0';
            // Replace the last component with the worker name.
            std::string exePath(buf, static_cast<size_t>(n));
            const auto pos = exePath.find_last_of('/');
            if (pos != std::string::npos) {
                resolvedPath = exePath.substr(0, pos) + "/hathor-audio-worker";
            }
        }
#elif defined(__APPLE__)
        if (_NSGetExecutablePath(buf, &size) == 0) {
            // _NSGetExecutablePath may return a path with /./ components.
            std::string exePath(buf);
            // Resolve via realpath for a clean path.
            if (const char* rp = ::realpath(exePath.c_str(), nullptr)) {
                exePath = rp;
                ::free(const_cast<char*>(rp));
            }
            const auto pos = exePath.find_last_of('/');
            if (pos != std::string::npos) {
                resolvedPath = exePath.substr(0, pos) + "/hathor-audio-worker";
            }
        }
#else
        impl_->lastError_ = "worker executable path resolution not supported on this platform";
        return false;
#endif
    }

    if (resolvedPath.empty() || !std::filesystem::exists(resolvedPath)) {
        impl_->lastError_ = "worker executable not found: " + resolvedPath;
        return false;
    }

    // Use a unique control socket path — the worker needs to know it.
    // We pass it as argv[1] so the worker binds to the same path.
    // The worker also needs to know the shared-memory name, which is fixed.

    // Build argv: ["hathor-audio-worker", <control-socket-path>, nullptr]
    std::vector<std::string> argvStrings = {
        resolvedPath,
        impl_->controlSocketPath_
    };
    std::vector<char*> argv;
    for (auto& s : argvStrings)
        argv.push_back(s.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fileActions;
    ::posix_spawn_file_actions_init(&fileActions);

    // No special file actions needed — the worker creates its own socket
    // and shared memory with known names.

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, resolvedPath.c_str(),
                                  &fileActions, nullptr,
                                  argv.data(), ::environ);

    ::posix_spawn_file_actions_destroy(&fileActions);

    if (rc != 0) {
        impl_->lastError_ = "posix_spawn failed: " + std::string(std::strerror(rc));
        return false;
    }

    impl_->workerPid_ = pid;
    impl_->workerKnownDead_.store(false, std::memory_order_release);

    return true;
}

bool AudioWorkerManager::initSharedMemory(uint64_t gen)
{
    // Clean up any stale shared memory first.
    ::shm_unlink(kShmName);

    // Create the shared-memory object.
    const int fd = ::shm_open(kShmName, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        // It may already exist from a crashed previous run — try opening it.
        impl_->shmFd_ = ::shm_open(kShmName, O_RDWR, 0600);
        if (impl_->shmFd_ < 0) {
            impl_->lastError_ = "shm_open failed: " + std::string(std::strerror(errno));
            return false;
        }
    } else {
        impl_->shmFd_ = fd;
    }

    // Size the segment.
    if (::ftruncate(impl_->shmFd_, static_cast<off_t>(kShmSize)) != 0) {
        impl_->lastError_ = "ftruncate failed: " + std::string(std::strerror(errno));
        ::close(impl_->shmFd_);
        impl_->shmFd_ = -1;
        ::shm_unlink(kShmName);
        return false;
    }

    // Map the segment.
    void* ptr = ::mmap(nullptr, kShmSize, PROT_READ | PROT_WRITE,
                       MAP_SHARED, impl_->shmFd_, 0);
    if (ptr == MAP_FAILED) {
        impl_->lastError_ = "mmap failed: " + std::string(std::strerror(errno));
        ::close(impl_->shmFd_);
        impl_->shmFd_ = -1;
        ::shm_unlink(kShmName);
        return false;
    }

    impl_->transport_ = static_cast<SharedAudioTransport*>(ptr);
    impl_->shmSize_   = kShmSize;

    // Initialise the transport header (zero everything, then set fields).
    std::memset(impl_->transport_, 0, sizeof(SharedAudioTransport));

    impl_->transport_->magic.store(kMagic, std::memory_order_release);
    impl_->transport_->generation.store(gen, std::memory_order_release);
    impl_->transport_->writeSeq.store(0, std::memory_order_release);
    impl_->transport_->readSeq.store(0, std::memory_order_release);
    impl_->transport_->sampleRate.store(44100, std::memory_order_release);
    impl_->transport_->channels.store(1, std::memory_order_release);
    impl_->transport_->workerAlive.store(false, std::memory_order_release);
    impl_->transport_->lastHeartbeat.store(0, std::memory_order_release);
    impl_->transport_->wrCount.store(0, std::memory_order_release);
    impl_->transport_->wrSumNs.store(0, std::memory_order_release);
    impl_->transport_->wrMaxNs.store(0, std::memory_order_release);
    impl_->transport_->wrMinNs.store(0, std::memory_order_release);

    impl_->generation_ = gen;

    // Close the fd — the mapping persists after close.
    ::close(impl_->shmFd_);
    impl_->shmFd_ = -1;

    return true;
}

void AudioWorkerManager::cleanupSharedMemory()
{
    if (impl_->transport_ && impl_->transport_ != MAP_FAILED) {
        ::munmap(impl_->transport_, impl_->shmSize_);
        impl_->transport_ = nullptr;
    }
    if (impl_->shmFd_ >= 0) {
        ::close(impl_->shmFd_);
        impl_->shmFd_ = -1;
    }
    ::shm_unlink(kShmName);
}

bool AudioWorkerManager::waitForWorkerStart(int timeoutMs)
{
    // Poll the workerAlive flag in shared memory.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (impl_->transport_ && impl_->transport_ != MAP_FAILED) {
            if (impl_->transport_->workerAlive.load(std::memory_order_acquire))
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Fallback: check if the process is still alive (it might have failed to
    // initialise shared memory but is still running).
    if (impl_->workerPid_ > 0) {
        int status = 0;
        pid_t r = ::waitpid(impl_->workerPid_, &status, WNOHANG);
        if (r == impl_->workerPid_) {
            impl_->lastError_ = "worker process exited immediately";
            return false;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Internal: liveness detection
// ---------------------------------------------------------------------------

bool AudioWorkerManager::checkProcessExit()
{
    if (impl_->workerPid_ <= 0)
        return false;

    int status = 0;
    pid_t r = ::waitpid(impl_->workerPid_, &status, WNOHANG);
    if (r == impl_->workerPid_) {
        // Process has exited.
        impl_->workerPid_ = -1;
        return true;
    }

    return false;
}

} // namespace hathor
