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

TEST_CASE("K8 restart debug", "[k8][restart][debug]") {
    const std::string workerPath = "build/app/audio-worker/hathor-audio-worker";
    
    AudioWorkerManager mgr;
    AudioWorkerManager::ResourceLimits limits;
    limits.heartbeatTimeoutMs = 300;
    mgr.setResourceLimits(limits);

    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();
    const pid_t workerPid = mgr.getWorkerPid();
    std::cerr << "gen=" << gen << " pid=" << workerPid << "\n";

    // Read audio baseline
    float buf[kBlockSize];
    bool gotBaseline = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(buf, kBlockSize, gen)) {
            gotBaseline = true;
            std::cerr << "got baseline audio, first sample=" << buf[0] << "\n";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(gotBaseline);

    // Kill the worker
    REQUIRE(::kill(workerPid, SIGKILL) == 0);
    std::cerr << "killed pid=" << workerPid << "\n";

    // Wait for death detection
    auto waitStart = std::chrono::steady_clock::now();
    bool dead = false;
    while (std::chrono::steady_clock::now() < waitStart + std::chrono::milliseconds(2000)) {
        if (!mgr.isWorkerAlive()) {
            dead = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto waitElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - waitStart).count();
    std::cerr << "waitForWorkerDeath: dead=" << dead << " elapsed=" << waitElapsed << "ms\n";
    std::cerr << "isWorkerAlive=" << mgr.isWorkerAlive() << " status=" << static_cast<int>(mgr.status()) << "\n";
    REQUIRE(dead);

    // Check if the worker is actually a zombie
    int st = 0;
    pid_t r = ::waitpid(workerPid, &st, WNOHANG);
    std::cerr << "waitpid check: r=" << r << " (0=zombie, pid=reaped, -1=error)\n";

    // Now restart
    auto restartStart = std::chrono::steady_clock::now();
    bool restartOk = mgr.restart();
    auto restartElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - restartStart).count();
    std::cerr << "restart: ok=" << restartOk << " elapsed=" << restartElapsed << "ms\n";
    
    if (restartOk) {
        const uint64_t gen2 = mgr.generation();
        std::cerr << "gen2=" << gen2 << " isWorkerAlive=" << mgr.isWorkerAlive() << "\n";
        
        // Try reading audio
        bool gotData = false;
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        int attempts = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            if (mgr.tryReadAudioBlock(buf, kBlockSize, gen2)) {
                gotData = true;
                std::cerr << "got audio on gen2 after " << attempts << " attempts\n";
                break;
            }
            ++attempts;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::cerr << "gotData=" << gotData << " attempts=" << attempts << "\n";
        REQUIRE(gotData);
    } else {
        std::cerr << "RESTART FAILED\n";
    }

    mgr.shutdown();
}
