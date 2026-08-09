#include "audio_ipc.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <csignal>
#include <string>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using b4k06::AudioBlock;
using b4k06::SharedAudioTransport;
using b4k06::kBlockSize;
using b4k06::kRingCapacity;
using b4k06::kRingMask;

static std::atomic<bool> gRunning{true};
static SharedAudioTransport* gTransport = nullptr;
static size_t gShmSize = 0;
static std::atomic<uint64_t> gGeneration{0};

void handleSigterm(int) {
    gRunning.store(false, std::memory_order_release);
}

void produceBlock(AudioBlock& block, uint32_t seq, uint64_t gen) {
    block.sequence.store(seq | 1u, std::memory_order_release);

    for (uint32_t i = 0; i < kBlockSize; ++i) {
        float phase = (static_cast<float>((seq % 64) * kBlockSize + i) / 64.0f) + static_cast<float>(gen);
        block.samples[i] = std::sin(phase * 0.1f) * 0.1f;
    }

    block.sequence.store(seq + 2u, std::memory_order_release);
}

void controlPlaneThread() {
    const char* ctrlPath = b4k06::kControlName;
    ::unlink(ctrlPath);

    int srvFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (srvFd < 0) return;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, ctrlPath, sizeof(addr.sun_path) - 1);
    ::unlink(ctrlPath);

    if (::bind(srvFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(srvFd);
        ::unlink(ctrlPath);
        return;
    }

    ::listen(srvFd, 4);

    while (gRunning.load(std::memory_order_acquire)) {
        struct pollfd pfd{};
        pfd.fd = srvFd;
        pfd.events = POLLIN;
        int pr = ::poll(&pfd, 1, 50);
        if (pr <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;

        int connFd = ::accept(srvFd, nullptr, nullptr);
        if (connFd < 0) continue;

        char buf[256];
        ssize_t n = ::read(connFd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            std::string cmd(buf, static_cast<size_t>(n));
            while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();

            std::string resp;
            if (cmd == "status") {
                uint64_t gen = gTransport->generation.load(std::memory_order_acquire);
                uint32_t wSeq = gTransport->writeSeq.load(std::memory_order_acquire);
                uint32_t rSeq = gTransport->readSeq.load(std::memory_order_acquire);
                resp = "ok gen=" + std::to_string(gen) +
                       " wSeq=" + std::to_string(wSeq) +
                       " rSeq=" + std::to_string(rSeq) + "\n";
            } else if (cmd == "stop") {
                resp = "ok stopping\n";
                ::write(connFd, resp.c_str(), resp.size());
                ::close(connFd);
                gRunning.store(false, std::memory_order_release);
                continue;
            } else if (cmd == "ping") {
                resp = "ok pong\n";
            } else {
                resp = "err unknown command\n";
            }
            ::write(connFd, resp.c_str(), resp.size());
        }
        ::close(connFd);
    }

    ::close(srvFd);
    ::unlink(ctrlPath);
}

}

int main() {
    ::signal(SIGTERM, handleSigterm);
    ::signal(SIGINT, handleSigterm);

    int fd = ::shm_open(b4k06::kShmName, O_RDWR, 0600);
    if (fd < 0) {
        fd = ::shm_open(b4k06::kShmName, O_CREAT | O_RDWR, 0600);
        if (fd < 0) {
            std::fprintf(stderr, "[worker] shm_open failed: %s\n", std::strerror(errno));
            return 1;
        }
        if (::ftruncate(fd, b4k06::kShmSize) != 0) {
            std::fprintf(stderr, "[worker] ftruncate failed: %s\n", std::strerror(errno));
            return 1;
        }
    }

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        std::fprintf(stderr, "[worker] fstat failed\n");
        return 1;
    }

    gShmSize = static_cast<size_t>(st.st_size);
    gTransport = static_cast<SharedAudioTransport*>(
        ::mmap(nullptr, gShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (gTransport == MAP_FAILED) {
        std::fprintf(stderr, "[worker] mmap failed: %s\n", std::strerror(errno));
        return 1;
    }

    uint64_t gen = gTransport->generation.load(std::memory_order_acquire);
    gGeneration.store(gen);
    gTransport->magic.store(b4k06::kMagic, std::memory_order_release);
    gTransport->sampleRate.store(44100, std::memory_order_release);
    gTransport->channels.store(1, std::memory_order_release);
    gTransport->writeSeq.store(0, std::memory_order_release);
    gTransport->readSeq.store(0, std::memory_order_release);
    gTransport->lastHeartbeat.store(0, std::memory_order_release);
    gTransport->wrCount.store(0, std::memory_order_release);
    gTransport->wrSumNs.store(0, std::memory_order_release);
    gTransport->wrMaxNs.store(0, std::memory_order_release);
    gTransport->wrMinNs.store(0, std::memory_order_release);
    gTransport->workerAlive.store(true, std::memory_order_release);

    std::fprintf(stderr, "[worker] started, pid=%d, generation=%llu, loop begin\n", getpid(),
                 static_cast<unsigned long long>(gen));

    std::thread ctrlThread(controlPlaneThread);

    auto nextWake = std::chrono::steady_clock::now();
    uint64_t beat = 0;

    while (gRunning.load(std::memory_order_acquire)) {
        nextWake += std::chrono::milliseconds(5);
        std::this_thread::sleep_until(nextWake);

        uint32_t wSeq = gTransport->writeSeq.load(std::memory_order_relaxed);
        uint32_t rSeq = gTransport->readSeq.load(std::memory_order_acquire);
        uint32_t nextW = wSeq + 1u;

        if (nextW - rSeq > kRingCapacity) {
            rSeq = wSeq - kRingCapacity + 1u;
            gTransport->readSeq.store(rSeq, std::memory_order_release);
        }

        gen = gTransport->generation.load(std::memory_order_acquire);

        AudioBlock& block = gTransport->blocks[wSeq & kRingMask];
        auto wStart = std::chrono::steady_clock::now();
        produceBlock(block, wSeq, gen);
        auto wEnd = std::chrono::steady_clock::now();
        uint64_t wNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(wEnd - wStart).count());
        auto prevMin = gTransport->wrMinNs.load(std::memory_order_relaxed);
        while (prevMin > wNs &&
               !gTransport->wrMinNs.compare_exchange_weak(prevMin, wNs, std::memory_order_relaxed)) {}
        auto prevMax = gTransport->wrMaxNs.load(std::memory_order_relaxed);
        while (wNs > prevMax &&
               !gTransport->wrMaxNs.compare_exchange_weak(prevMax, wNs, std::memory_order_relaxed)) {}
        gTransport->wrSumNs.fetch_add(wNs, std::memory_order_relaxed);
        gTransport->wrCount.fetch_add(1, std::memory_order_relaxed);

        gTransport->lastHeartbeat.store(beat, std::memory_order_release);
        ++beat;
        gTransport->writeSeq.store(nextW, std::memory_order_release);
    }

    gTransport->workerAlive.store(false, std::memory_order_release);
    std::fprintf(stderr, "[worker] shutting down, pid=%d\n", getpid());

    if (ctrlThread.joinable()) ctrlThread.join();

    ::munmap(gTransport, gShmSize);
    ::close(fd);

    return 0;
}
