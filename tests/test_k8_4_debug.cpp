#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <chrono>
#include <cstring>
#include <thread>
#include <iostream>

#include "AudioWorkerManager.hpp"
#include "audio_ipc.h"

using hathor::AudioWorkerManager;
using hathor::audio_worker::kBlockSize;
using hathor::audio_worker::kShmName;
using hathor::audio_worker::SharedAudioTransport;

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static std::string getWorkerPath() {
    return "build/app/audio-worker/hathor-audio-worker";
}

struct ShmHandle {
    int fd = -1;
    void* ptr = nullptr;
    size_t size = 0;
    SharedAudioTransport* transport() const { return static_cast<SharedAudioTransport*>(ptr); }
    ~ShmHandle() {
        if (ptr && ptr != MAP_FAILED) ::munmap(ptr, size);
        if (fd >= 0) ::close(fd);
    }
};

static ShmHandle mapShm() {
    ShmHandle h;
    h.fd = ::shm_open(kShmName, O_RDWR, 0600);
    if (h.fd < 0) return h;
    struct stat st{};
    ::fstat(h.fd, &st);
    h.size = static_cast<size_t>(st.st_size);
    h.ptr = ::mmap(nullptr, h.size, PROT_READ | PROT_WRITE, MAP_SHARED, h.fd, 0);
    if (h.ptr == MAP_FAILED) { h.ptr = nullptr; }
    return h;
}

static void waitForWorkerDeath(AudioWorkerManager& mgr, std::chrono::milliseconds maxWait) {
    auto deadline = std::chrono::steady_clock::now() + maxWait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!mgr.isWorkerAlive()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

TEST_CASE("K8.4 restart debug", "[k8][mid-write][recovery][debug]") {
    const std::string workerPath = getWorkerPath();
    AudioWorkerManager mgr;
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();
    const pid_t workerPid = mgr.getWorkerPid();
    std::cerr << "gen=" << gen << " pid=" << workerPid << "\n";

    // Baseline audio
    float buf[kBlockSize];
    uint32_t reads = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && reads < 10) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen)) {
            for (uint32_t i = 0; i < kBlockSize; ++i)
                REQUIRE(std::isfinite(buf[i]));
            ++reads;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }
    std::cerr << "baseline reads: " << reads << "\n";
    REQUIRE(reads >= 5);

    // Map SHM directly
    ShmHandle shm = mapShm();
    REQUIRE(shm.transport() != nullptr);
    std::cerr << "SHM1: magic=" << shm.transport()->magic.load()
              << " gen=" << shm.transport()->generation.load()
              << " workerAlive=" << shm.transport()->workerAlive.load() << "\n";

    // Kill worker mid-write
    REQUIRE(::kill(workerPid, SIGKILL) == 0);
    std::cerr << "killed worker\n";

    // Wait for death
    waitForWorkerDeath(mgr, std::chrono::milliseconds(2000));
    std::cerr << "isWorkerAlive after death: " << mgr.isWorkerAlive() << "\n";
    std::cerr << "status after death: " << static_cast<int>(mgr.status()) << "\n";
    REQUIRE_FALSE(mgr.isWorkerAlive());

    // Restart
    auto t0 = std::chrono::steady_clock::now();
    bool restartOk = mgr.restart();
    auto t1 = std::chrono::steady_clock::now();
    auto restartMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cerr << "restart: ok=" << restartOk << " elapsed=" << restartMs << "ms\n";
    REQUIRE(restartOk);

    const uint64_t gen2 = mgr.generation();
    std::cerr << "gen2=" << gen2 << " isWorkerAlive=" << mgr.isWorkerAlive() << "\n";
    REQUIRE(gen2 == gen + 1);
    REQUIRE(mgr.isWorkerAlive());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Check SHM
    ShmHandle shm2 = mapShm();
    if (shm2.transport() != nullptr) {
        std::cerr << "SHM2: magic=" << shm2.transport()->magic.load()
                  << " gen=" << shm2.transport()->generation.load()
                  << " workerAlive=" << shm2.transport()->workerAlive.load()
                  << " heartbeat=" << shm2.transport()->lastHeartbeat.load()
                  << " writeSeq=" << shm2.transport()->writeSeq.load()
                  << " readSeq=" << shm2.transport()->readSeq.load() << "\n";
        REQUIRE(shm2.transport()->generation.load(std::memory_order_acquire) == gen2);
    }

    // Read audio
    bool gotData = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int attempts = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen2)) {
            for (uint32_t i = 0; i < kBlockSize; ++i)
                REQUIRE(std::isfinite(buf[i]));
            gotData = true;
            std::cerr << "got audio after " << attempts << " attempts\n";
            break;
        }
        ++attempts;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cerr << "gotData=" << gotData << " attempts=" << attempts << "\n";
    REQUIRE(gotData);

    mgr.shutdown();
}
