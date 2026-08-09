#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <thread>

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

TEST_CASE("K8 debug", "[k8][debug]") {
    const std::string workerPath = "build/app/audio-worker/hathor-audio-worker";
    
    AudioWorkerManager mgr;
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();

    // Test ping
    std::string ping = mgr.sendControlCommand("ping", 2000);
    WARN("ping response: '" << ping << "'");
    REQUIRE_FALSE(ping.empty());

    // Test status
    std::string status = mgr.sendControlCommand("status", 2000);
    WARN("status response: '" << status << "'");
    REQUIRE_FALSE(status.empty());

    // Test watchdog_timeout
    std::string wdTimeout = mgr.sendControlCommand("watchdog_timeout 200", 2000);
    WARN("watchdog_timeout response: '" << wdTimeout << "'");
    REQUIRE_FALSE(wdTimeout.empty());

    // Test watchdog_interval
    std::string wdInterval = mgr.sendControlCommand("watchdog_interval 50", 2000);
    WARN("watchdog_interval response: '" << wdInterval << "'");
    REQUIRE_FALSE(wdInterval.empty());

    // Test vm_activate
    std::string vmResp = mgr.sendControlCommand("vm_activate 0 44100 1", 2000);
    WARN("vm_activate response: '" << vmResp << "'");
    REQUIRE(vmResp.find("ok vm_activated") != std::string::npos);

    // Test watchdog_status
    std::string wdStatus = mgr.sendControlCommand("watchdog_status", 2000);
    WARN("watchdog_status response: '" << wdStatus << "'");

    // Try reading audio
    float buf[kBlockSize];
    uint32_t reads = 0;
    for (int i = 0; i < 200 && reads < 3; ++i) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen)) {
            reads++;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    WARN("reads before hang: " << reads);
    REQUIRE(reads >= 1);

    // Check SHM directly
    int fd = ::shm_open(kShmName, O_RDWR, 0600);
    if (fd >= 0) {
        struct stat st{};
        ::fstat(fd, &st);
        void* ptr = ::mmap(nullptr, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr != MAP_FAILED) {
            SharedAudioTransport* t = static_cast<SharedAudioTransport*>(ptr);
            WARN("SHM: magic=" << t->magic.load()
                      << " gen=" << t->generation.load()
                      << " workerAlive=" << t->workerAlive.load()
                      << " heartbeat=" << t->lastHeartbeat.load()
                      << " writeSeq=" << t->writeSeq.load()
                      << " readSeq=" << t->readSeq.load());
            ::munmap(ptr, st.st_size);
        }
        ::close(fd);
    }

    mgr.shutdown();
}
