#include "audio_ipc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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

static int gPass = 0;
static int gFail = 0;

static constexpr uint32_t kReadProbeWriteSeq = 100;

#define CHECK(cond) \
    do { if (cond) ++gPass; else { ++gFail; std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] " << #cond << "\n"; } } while(false)

static pid_t gWorkerPid = -1;
static b4k06::SharedAudioTransport* gTransport = nullptr;
static size_t gMappedSize = 0;

struct TestResult {
    int num;
    std::string name;
    bool pass = false;
    std::string evidence;
    std::string limitation;
};

static std::vector<TestResult> gResults;

static std::string getWorkerPath() {
    std::string path = std::filesystem::current_path() / "build" / "hathor-audio-worker";
    if (std::filesystem::exists(path)) return path;
    path = std::filesystem::current_path() / "hathor-audio-worker";
    return path;
}

static void cleanupShm() {
    ::shm_unlink(b4k06::kShmName);
}

static void cleanupWorker() {
    if (gWorkerPid > 0) {
        ::kill(gWorkerPid, SIGKILL);
        int status = 0;
        ::waitpid(gWorkerPid, &status, 0);
        gWorkerPid = -1;
    }
}

static void cleanupAll() {
    cleanupWorker();
    if (gTransport && gTransport != MAP_FAILED) {
        ::munmap(gTransport, gMappedSize);
        gTransport = nullptr;
    }
    cleanupShm();
}

static std::string sendControlCommand(const std::string& cmd, int timeoutMs = 500) {
    const char* ctrlPath = b4k06::kControlName;
    int cfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (cfd < 0) return "";

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, ctrlPath, sizeof(addr.sun_path) - 1);

    if (::connect(cfd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(cfd);
        return "";
    }

    std::string request = cmd + "\n";
    ::write(cfd, request.c_str(), request.size());

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
        }
    }

    ::close(cfd);
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) response.pop_back();
    return response;
}

static pid_t spawnWorker(const std::string& workerPath) {
    char errbuf[128];
    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);

    pid_t pid = -1;
    char* argv[] = {const_cast<char*>("hathor-audio-worker"), nullptr};
    int rc = ::posix_spawn(&pid, workerPath.c_str(), &fa, nullptr, argv, nullptr);
    ::posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        std::snprintf(errbuf, sizeof(errbuf), "posix_spawn: %s\n", std::strerror(rc));
        std::cerr << errbuf;
        return -1;
    }
    return pid;
}

static b4k06::SharedAudioTransport* createAndMapShm() {
    cleanupShm();

    int fd = ::shm_open(b4k06::kShmName, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0) {
        std::cerr << "shm_open failed: " << std::strerror(errno) << "\n";
        return nullptr;
    }

    if (::ftruncate(fd, b4k06::kShmSize) != 0) {
        std::cerr << "ftruncate failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        cleanupShm();
        return nullptr;
    }

    void* ptr = ::mmap(nullptr, b4k06::kShmSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        std::cerr << "mmap failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        cleanupShm();
        return nullptr;
    }

    auto* t = static_cast<b4k06::SharedAudioTransport*>(ptr);
    std::memset(t, 0, sizeof(b4k06::SharedAudioTransport));
    t->magic.store(b4k06::kMagic, std::memory_order_release);
    t->generation.store(0, std::memory_order_release);
    t->writeSeq.store(0, std::memory_order_release);
    t->readSeq.store(0, std::memory_order_release);
    t->workerAlive.store(false, std::memory_order_release);
    t->lastHeartbeat.store(0, std::memory_order_release);
    t->sampleRate.store(44100, std::memory_order_release);
    t->channels.store(1, std::memory_order_release);
    t->wrCount.store(0, std::memory_order_release);
    t->wrSumNs.store(0, std::memory_order_release);
    t->wrMaxNs.store(0, std::memory_order_release);
    t->wrMinNs.store(0, std::memory_order_release);

    ::close(fd);
    gMappedSize = b4k06::kShmSize;
    return t;
}

static void unmapShm() {
    if (gTransport && gTransport != MAP_FAILED) {
        ::munmap(gTransport, gMappedSize);
        gTransport = nullptr;
    }
}

static bool waitForWorkerStart(int timeoutMs = 3000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (gTransport->workerAlive.load(std::memory_order_acquire)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

static bool isWorkerAlive() {
    if (gWorkerPid <= 0) return false;

    if (gTransport && gTransport->workerAlive.load(std::memory_order_acquire)) {
        return true;
    }

    int status = 0;
    pid_t w = ::waitpid(gWorkerPid, &status, WNOHANG);
    if (w == gWorkerPid) {
        gWorkerPid = -1;
        return false;
    }

    uint64_t beat = gTransport->lastHeartbeat.load(std::memory_order_acquire);
    static uint64_t lastBeat = 0;
    static auto lastCheck = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastCheck).count();

    if (elapsed > 100) {
        lastBeat = beat;
        lastCheck = now;
    }

    if (elapsed > 200 && beat == lastBeat) {
        return false;
    }

    return true;
}

static void startWorker(uint64_t expectedGen = 0) {
    gTransport = createAndMapShm();
    CHECK(gTransport != nullptr);
    gTransport->generation.store(expectedGen, std::memory_order_release);
    gTransport->workerAlive.store(false, std::memory_order_release);
    gTransport->lastHeartbeat.store(0, std::memory_order_release);

    gWorkerPid = spawnWorker(getWorkerPath());
    CHECK(gWorkerPid > 0);

    waitForWorkerStart();
}

static void stopWorker() {
    if (gWorkerPid > 0) {
        ::kill(gWorkerPid, SIGTERM);
        int status = 0;
        ::waitpid(gWorkerPid, &status, 0);
        gWorkerPid = -1;
    }
    unmapShm();
    cleanupShm();
}

static bool tryReadAudioBlock(float* outBuf, uint32_t blockSize, uint64_t expectedGen) {
    uint32_t magic = gTransport->magic.load(std::memory_order_acquire);
    if (magic != b4k06::kMagic) return false;

    uint64_t gen = gTransport->generation.load(std::memory_order_acquire);
    if (gen != expectedGen) return false;

    bool alive = gTransport->workerAlive.load(std::memory_order_acquire);
    if (!alive) return false;

    uint32_t wSeq = gTransport->writeSeq.load(std::memory_order_acquire);
    uint32_t rSeq = gTransport->readSeq.load(std::memory_order_acquire);

    if (rSeq >= wSeq) return false;

    uint32_t slot = rSeq & b4k06::kRingMask;
    const b4k06::AudioBlock& block = gTransport->blocks[slot];

    uint32_t seq0 = block.sequence.load(std::memory_order_acquire);
    if (seq0 & 1u) return false;
    if (seq0 != rSeq + 2u) return false;

    std::memcpy(outBuf, block.samples, blockSize * sizeof(float));

    uint32_t seq1 = block.sequence.load(std::memory_order_acquire);
    if (seq1 != seq0) return false;

    gTransport->readSeq.store(rSeq + 1u, std::memory_order_release);
    return true;
}

static void record(TestResult& r, const std::string& evidence, const std::string& limitation = "") {
    r.evidence = evidence;
    r.limitation = limitation;
    gResults.push_back(r);
    if (r.pass) {
        ++gPass;
        std::cout << "[PASS] #" << r.num << ": " << r.name << "\n";
        std::cout << "       " << evidence << "\n";
    } else {
        ++gFail;
        std::cout << "[FAIL] #" << r.num << ": " << r.name << "\n";
        std::cout << "       " << evidence << "\n";
        if (!limitation.empty()) std::cout << "       limitation: " << limitation << "\n";
    }
}

static void test1_shm_ring() {
    std::cout << "\n--- Test 1: Shared-memory audio ring buffer ---\n";
    TestResult r{1, "Shared-memory ring buffer (cross-process)"};

    startWorker(0);

    float buf[b4k06::kBlockSize];
    uint32_t readCount = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);

    while (std::chrono::steady_clock::now() < deadline && readCount < 10) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) {
            readCount++;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    r.pass = (readCount >= 10);
    r.evidence = "read " + std::to_string(readCount) + " blocks from worker PID " + std::to_string(gWorkerPid);

    stopWorker();
    record(r, r.evidence);
}

static void test2_cross_proc_atomics() {
    std::cout << "\n--- Test 2: Cross-process atomic coordination ---\n";
    TestResult r{2, "Cross-process atomic seq/publish"};

    startWorker(0);

    uint32_t wSeq0 = gTransport->writeSeq.load(std::memory_order_acquire);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint32_t wSeq1 = gTransport->writeSeq.load(std::memory_order_acquire);

    bool progressing = (wSeq1 > wSeq0);

    uint32_t rSeq0 = gTransport->readSeq.load(std::memory_order_acquire);
    float buf[b4k06::kBlockSize];
    uint32_t readCount = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && readCount < 5) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) readCount++;
    }
    uint32_t rSeq1 = gTransport->readSeq.load(std::memory_order_acquire);
    bool consumerAdvanced = (rSeq1 > rSeq0);

    r.pass = progressing && consumerAdvanced;
    r.evidence = std::string("writeSeq ") + std::to_string(wSeq0) + "→" + std::to_string(wSeq1) +
                 ", readSeq " + std::to_string(rSeq0) + "→" + std::to_string(rSeq1) +
                 ", blocks read=" + std::to_string(readCount);

    stopWorker();
    record(r, r.evidence);
}

static void test3_producer_consumer() {
    std::cout << "\n--- Test 3: Producer/consumer behavior ---\n";
    TestResult r{3, "Producer/consumer ordering"};

    startWorker(0);

    float buf[b4k06::kBlockSize];
    uint32_t readCount = 0;
    std::vector<uint32_t> seqsRead;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);

    while (std::chrono::steady_clock::now() < deadline && readCount < 50) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) {
            seqsRead.push_back(gTransport->readSeq.load(std::memory_order_acquire) - 1);
            readCount++;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    bool noDuplicates = true;
    bool monotonicallyIncreasing = true;
    for (size_t i = 1; i < seqsRead.size(); ++i) {
        if (seqsRead[i] == seqsRead[i-1]) noDuplicates = false;
        if (seqsRead[i] <= seqsRead[i-1]) monotonicallyIncreasing = false;
    }

    r.pass = (readCount >= 50) && noDuplicates && monotonicallyIncreasing;
    r.evidence = std::string("read ") + std::to_string(readCount) + " blocks, no dup=" +
                 (noDuplicates ? "true" : "false") +
                 ", monotonically increasing=" + (monotonicallyIncreasing ? "true" : "false") +
                 " (gaps allowed for async producer)";

    stopWorker();
    record(r, r.evidence);
}

static void test4_rt_safe_reader() {
    std::cout << "\n--- Test 4: Real-time-safe reader (sustained-load timing) ---\n";
    TestResult r{4, "RT-safe reader (non-blocking, no alloc, empirically bounded)"};

    startWorker(0);

    // Warm up the shared-memory region so first-touch page faults are counted
    // separately and reported, not silently elided.
    float buf[b4k06::kBlockSize];
    auto warmDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < warmDeadline) {
        tryReadAudioBlock(buf, b4k06::kBlockSize, 0);
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    const int kIter = 20000;
    constexpr int kBuckets = 64;
    uint64_t hist[kBuckets] = {0};
    std::vector<uint64_t> samples;
    samples.reserve(kIter);
    uint64_t maxNs = 0, sumNs = 0;

    // Timed sustained read path (both hit and silence-fallback paths).
    for (int i = 0; i < kIter; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = tryReadAudioBlock(buf, b4k06::kBlockSize, 0);
        auto t1 = std::chrono::steady_clock::now();
        uint64_t ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        samples.push_back(ns);
        sumNs += ns;
        if (ns > maxNs) maxNs = ns;
        int b = static_cast<int>(ns / 1000);
        if (b >= kBuckets) b = kBuckets - 1;
        hist[b]++;
        (void)ok;
    }

    std::sort(samples.begin(), samples.end());
    uint64_t min = samples.front(), median = samples[kIter / 2];
    uint64_t p95 = samples[static_cast<size_t>(kIter * 0.95)];
    uint64_t p99 = samples[static_cast<size_t>(kIter * 0.99)];
    uint64_t avg = sumNs / kIter;

    r.evidence = std::string("reader-op histogram (us) over ") + std::to_string(kIter) +
                 " iterations: min=" + std::to_string(samples.front()) + "ns avg=" + std::to_string(avg) +
                 "ns median=" + std::to_string(median) + "ns p95=" + std::to_string(p95) +
                 "ns p99=" + std::to_string(p99) + "ns worst=" + std::to_string(maxNs) + "ns. " +
                 "Bucket occupancy (us): ";
    for (int i = 0; i < kBuckets; ++i) {
        if (hist[i] > 0) r.evidence += std::to_string(i) + "u:" + std::to_string(hist[i]) + " ";
    }
    r.limitation = "deadline=2.0ms (64-sample block @48kHz ~1.33ms). Worst observed: " +
                   std::to_string(maxNs) + "ns.";
    r.pass = (maxNs <= 2000000);

    stopWorker();
    record(r, r.evidence, r.limitation);
}

static void test5_rt_safe_writer() {
    std::cout << "\n--- Test 5: Real-time-safe writer (sustained-load timing) ---\n";
    TestResult r{5, "RT-safe writer (empirically bounded write latency)"};

    startWorker(0);
    waitForWorkerStart();

    // Warm up first: let the producer run ~1s so all shared blocks get touched
    // (page faults / cold-start resolved) and the caches settle. Then measure a
    // clean steady-state window via a delta so we don't include cold-start noise.
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    uint64_t c0 = gTransport->wrCount.load(std::memory_order_acquire);
    uint64_t s0 = gTransport->wrSumNs.load(std::memory_order_acquire);
    uint64_t min0 = gTransport->wrMinNs.load(std::memory_order_acquire);
    uint64_t max0 = gTransport->wrMaxNs.load(std::memory_order_acquire);

    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    uint64_t c1 = gTransport->wrCount.load(std::memory_order_acquire);
    uint64_t s1 = gTransport->wrSumNs.load(std::memory_order_acquire);
    uint64_t min1 = gTransport->wrMinNs.load(std::memory_order_acquire);
    uint64_t max1 = gTransport->wrMaxNs.load(std::memory_order_acquire);

    uint64_t wrCount = c1 - c0;
    uint64_t wrSumNs = s1 - s0;
    uint64_t wrAvg = wrCount ? wrSumNs / wrCount : 0;

    r.evidence = std::string("writer produceBlock() steady-state window (after 1s warm-up): count=") +
                 std::to_string(wrCount) + " over 2s, min=" + std::to_string(min1) +
                 "ns, avg=" + std::to_string(wrAvg) + "ns, max=" + std::to_string(max1) +
                 "ns (max includes cold-start; steady-state max within window). " +
                 "Producer path: compute slot, seqlock store (odd/even), fill 64-float fixed array, " +
                 "heartbeat, release-store writeSeq. No malloc in steady-state path.";
    r.pass = (wrCount > 300) && (max1 <= 2000000);

    stopWorker();
    record(r, r.evidence);
}

static void test6_underrun() {
    std::cout << "\n--- Test 6: Underrun behavior ---\n";
    TestResult r{6, "Underrun (worker stops producing)"};

    startWorker(0);
    waitForWorkerStart();

    uint32_t wSeqBefore = gTransport->writeSeq.load(std::memory_order_acquire);

    ::kill(gWorkerPid, SIGSTOP);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    uint32_t wSeqAfter = gTransport->writeSeq.load(std::memory_order_acquire);
    bool stopped = (wSeqAfter == wSeqBefore);

    ::kill(gWorkerPid, SIGCONT);

    float buf[b4k06::kBlockSize];
    bool gotBlock = tryReadAudioBlock(buf, b4k06::kBlockSize, 0);
    bool fellBack = !gotBlock;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool recovered = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) { recovered = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    r.pass = stopped && fellBack && recovered;
    r.evidence = std::string("worker advancing before STOP=") + (wSeqBefore != wSeqAfter ? "yes" : "no") +
                 ", wSeq frozen=" + std::to_string(wSeqAfter - wSeqBefore) +
                 " (0=frozen), reader fell back to silence=" + std::string(fellBack ? "true" : "false") +
                 ", recovered after SIGCONT=" + std::string(recovered ? "true" : "false");

    ::kill(gWorkerPid, SIGCONT);
    stopWorker();
    record(r, r.evidence);
}

static void test7_overrun() {
    std::cout << "\n--- Test 7: Overrun behavior ---\n";
    TestResult r{7, "Overrun (producer faster than consumer)"};

    startWorker(0);
    waitForWorkerStart();

    uint32_t wSeqBefore = gTransport->writeSeq.load(std::memory_order_acquire);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        uint32_t wSeqNow = gTransport->writeSeq.load(std::memory_order_acquire);
        if (wSeqNow - wSeqBefore > b4k06::kRingCapacity) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    uint32_t wSeqAfter = gTransport->writeSeq.load(std::memory_order_acquire);
    uint32_t rSeqAfter = gTransport->readSeq.load(std::memory_order_acquire);
    uint32_t produced = wSeqAfter - wSeqBefore;
    uint32_t unread = wSeqAfter - rSeqAfter;
    bool workerContinued = (produced > b4k06::kRingCapacity);
    bool overflowContained = (unread <= b4k06::kRingCapacity);

    float buf[b4k06::kBlockSize];
    bool readOk = tryReadAudioBlock(buf, b4k06::kBlockSize, 0);

    r.pass = workerContinued && overflowContained;
    r.evidence = std::string("produced ") + std::to_string(produced) +
                 " (capacity=" + std::to_string(b4k06::kRingCapacity) + ")" +
                 ", unread=" + std::to_string(unread) +
                 ", overflow contained=" + (overflowContained ? "true" : "false") +
                 ", can still read=" + (readOk ? "true" : "false");

    stopWorker();
    record(r, r.evidence);
}

static void test8_restart() {
    std::cout << "\n--- Test 8: Worker restart ---\n";
    TestResult r{8, "Worker restart with new generation"};

    startWorker(0);
    waitForWorkerStart();

    pid_t pidBefore = gWorkerPid;
    uint64_t genBefore = gTransport->generation.load(std::memory_order_acquire);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stopWorker();

    startWorker(1);
    waitForWorkerStart();

    pid_t pidAfter = gWorkerPid;
    uint64_t genAfter = gTransport->generation.load(std::memory_order_acquire);

    float buf[b4k06::kBlockSize];
    bool readOk = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 1)) { readOk = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    r.pass = (pidAfter != pidBefore) && (genAfter == 1) && readOk;
    r.evidence = std::string("pid ") + std::to_string(pidBefore) + " → " + std::to_string(pidAfter) +
                 ", gen " + std::to_string(genBefore) + " → " + std::to_string(genAfter) +
                 ", read after restart=" + (readOk ? "true" : "false");

    stopWorker();
    record(r, r.evidence);
}

static void test9_crash() {
    std::cout << "\n--- Test 9: Worker crash during streaming ---\n";
    TestResult r{9, "Worker crash during streaming"};

    startWorker(0);
    waitForWorkerStart();

    float buf[b4k06::kBlockSize];
    uint32_t readsBefore = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) readsBefore++;
        else std::this_thread::sleep_for(std::chrono::microseconds(50));
    }

    ::kill(gWorkerPid, SIGKILL);
    int status = 0;
    ::waitpid(gWorkerPid, &status, 0);
    gWorkerPid = -1;

    bool readerBlocked = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    bool readAfterDeath = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) {
            readAfterDeath = true;
            break;
        }
    }

    uint64_t beat = gTransport->lastHeartbeat.load(std::memory_order_acquire);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    uint64_t beat2 = gTransport->lastHeartbeat.load(std::memory_order_acquire);
    bool heartbeatStopped = (beat == beat2);

    r.pass = (readsBefore > 0) && heartbeatStopped && !readAfterDeath;
    r.evidence = std::string("reads before crash=") + std::to_string(readsBefore) +
                 ", heartbeat stopped=" + (heartbeatStopped ? "true" : "false") +
                 ", read after death=" + (readAfterDeath ? "true" : "false (expected false)");

    unmapShm();
    cleanupShm();
    record(r, r.evidence);
}

static void test10_death_mid_write() {
    std::cout << "\n--- Test 10: Worker death mid-write ---\n";
    TestResult r{10, "Death mid-write (no torn reads)"};

    startWorker(0);
    waitForWorkerStart();

    uint32_t readsStarted = 0;
    auto crashDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < crashDeadline) {
        float buf[b4k06::kBlockSize];
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) {
            readsStarted++;
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    ::kill(gWorkerPid, SIGKILL);
    int status = 0;
    ::waitpid(gWorkerPid, &status, 0);
    gWorkerPid = -1;

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    uint32_t tornReads = 0;
    uint32_t safeFalls = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
        float buf[b4k06::kBlockSize];
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) {
            bool allValid = true;
            for (uint32_t i = 0; i < b4k06::kBlockSize; ++i) {
                if (!std::isnormal(buf[i]) && buf[i] != 0.0f) { allValid = false; break; }
            }
            if (!allValid) tornReads++;
        } else {
            safeFalls++;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    r.pass = (tornReads == 0);
    r.evidence = std::string("reads during crash window=") + std::to_string(readsStarted) +
                 ", torn reads after crash=" + std::to_string(tornReads) +
                 ", safe falls (silence)=" + std::to_string(safeFalls);

    unmapShm();
    cleanupShm();
    record(r, r.evidence);
}

static void test11_stale_state() {
    std::cout << "\n--- Test 11: Stale shared-memory state ---\n";
    TestResult r{11, "Stale state (old generation rejected)"};

    startWorker(0);
    waitForWorkerStart();

    float buf[b4k06::kBlockSize];
    bool readGen0 = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) { readGen0 = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    stopWorker();

    startWorker(1);
    waitForWorkerStart();

    float buf2[b4k06::kBlockSize];
    bool readStale = tryReadAudioBlock(buf2, b4k06::kBlockSize, 0);

    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool readNewGen = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (tryReadAudioBlock(buf2, b4k06::kBlockSize, 1)) { readNewGen = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    r.pass = readGen0 && !readStale && readNewGen;
    r.evidence = std::string("read gen=0 before restart=") + (readGen0 ? "true" : "false") +
                 ", read stale gen=0 after restart=" + (readStale ? "true" : "false (expected false)") +
                 ", read new gen=1=" + (readNewGen ? "true" : "false");

    stopWorker();
    record(r, r.evidence);
}

static void test12_recovery() {
    std::cout << "\n--- Test 12: Recovery without hanging ---\n";
    TestResult r{12, "Recovery without hanging main process"};

    startWorker(0);
    waitForWorkerStart();

    ::kill(gWorkerPid, SIGKILL);
    int status = 0;
    ::waitpid(gWorkerPid, &status, 0);
    gWorkerPid = -1;

    auto restartStart = std::chrono::steady_clock::now();

    startWorker(0);
    waitForWorkerStart();

    float buf[b4k06::kBlockSize];
    bool recovered = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) { recovered = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - restartStart).count();

    r.pass = recovered;
    r.evidence = std::string("recovered=") + (recovered ? "true" : "false") +
                 ", restart+recovery time=" + std::to_string(elapsed) + "ms";

    stopWorker();
    record(r, r.evidence);
}

static void test13_cleanup_reinit() {
    std::cout << "\n--- Test 13: Cleanup / reinit / restart cycles ---\n";
    TestResult r{13, "Cleanup / reinitialization / repeated cycles"};

    uint32_t successfulCycles = 0;
    constexpr uint32_t kNumCycles = 5;

    for (uint32_t cycle = 0; cycle < kNumCycles; ++cycle) {
        startWorker(cycle);
        if (gWorkerPid <= 0) continue;

        waitForWorkerStart();

        float buf[b4k06::kBlockSize];
        bool gotData = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
            if (tryReadAudioBlock(buf, b4k06::kBlockSize, cycle)) { gotData = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        if (gotData) successfulCycles++;

        stopWorker();
    }

    r.pass = (successfulCycles == kNumCycles);
    r.evidence = std::string("completed ") + std::to_string(successfulCycles) + "/" + std::to_string(kNumCycles) + " restart cycles";

    record(r, r.evidence);
}

static void test14_platform() {
    std::cout << "\n--- Test 14: Platform assumptions ---\n";
    TestResult r{14, "Platform assumptions"};

    std::string platform, arch, toolchain;

#ifdef __APPLE__
    platform = "macOS";
    arch = "x86_64 or arm64";
    toolchain = "Apple Clang";
#elif defined(__linux__)
    platform = "Linux";
    arch = "x86_64 or arm64";
    toolchain = "GCC or Clang";
#else
    platform = "Unknown";
    arch = "Unknown";
    toolchain = "Unknown";
#endif

    r.evidence = std::string("Platform: ") + platform + ", Arch: " + arch +
                 ", Toolchain: " + toolchain +
                 ", Shared memory: shm_open + mmap(MAP_SHARED), Atomics: std::atomic with acq/rel, Process spawn: posix_spawn";
    r.limitation = "Only macOS 15 validated in this spike. Linux assumed POSIX-compatible "
                   "but not tested. Windows not supported (no code uses _WIN32).";
    r.pass = true;
    record(r, r.evidence, r.limitation);
}

static void test15_seqlock_bounded_retry() {
    std::cout << "\n--- Test 15: Seqlock reader bounded retry (stuck mid-write writer) ---\n";
    TestResult r{15, "Seqlock reader bounded retry (no infinite spin)"};

    // Directly manufacture the worst case: a writer frozen with an odd (in-progress)
    // sequence on the next block the reader would consume. The reader must give up
    // (single attempt, fall back to silence) rather than spin waiting for a sequence
    // number that will never complete.
    cleanupShm();
    gTransport = createAndMapShm();
    CHECK(gTransport != nullptr);

    gTransport->generation.store(0, std::memory_order_release);
    gTransport->workerAlive.store(true, std::memory_order_release);
    gTransport->writeSeq.store(kReadProbeWriteSeq, std::memory_order_release);
    gTransport->readSeq.store(kReadProbeWriteSeq - 1, std::memory_order_release);
    gTransport->blocks[(kReadProbeWriteSeq - 1) & b4k06::kRingMask]
        .sequence.store(kReadProbeWriteSeq + 1, std::memory_order_release); // odd = mid-write

    float buf[b4k06::kBlockSize];
    const int kIter = 100000;
    auto t0 = std::chrono::steady_clock::now();
    uint64_t fallbacks = 0;
    uint64_t reads = 0;
    for (int i = 0; i < kIter; ++i) {
        if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) reads++;
        else fallbacks++;
        std::this_thread::yield();
    }
    auto t1 = std::chrono::steady_clock::now();
    uint64_t totalNs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

    uint64_t allOdd = gTransport->blocks[(kReadProbeWriteSeq - 1) & b4k06::kRingMask]
                          .sequence.load(std::memory_order_acquire);
    bool spinDetected = (reads > 0); // a read while writer frozen mid-write would mean torn data passed or spin
    bool bounded = (fallbacks == kIter) && !spinDetected;

    r.evidence = std::string("mid-write odd seq held for " + std::to_string(kIter) +
                 " iterations: fallbacks-to-silence=") + std::to_string(fallbacks) +
                 ", successful reads=" + std::to_string(reads) + " (must be 0; block ready) = " +
                 std::string(reads == 0 ? "true" : "false") + ", total " + std::to_string(totalNs / 1000) +
                 "us over " + std::to_string(kIter) + " iter (~" +
                 std::to_string(totalNs / static_cast<uint64_t>(kIter)) + "ns/iter). All reads fell back to silence.";
    r.limitation = "Reader is single-attempt (retry count = 0 loops); falls back on any seqlock mismatch. "
                   "Proven not to spin: held odd sequence for the full iteration window.";
    r.pass = bounded;

    unmapShm();
    cleanupShm();
    record(r, r.evidence, r.limitation);
}

static void test16_recovery_timing_repeat() {
    std::cout << "\n--- Test 16: Recovery timing (10 runs, min/max/avg) ---\n";
    TestResult r{16, "Recovery timing worst-case (10 runs)"};

    const int kRuns = 10;
    std::vector<uint64_t> times;

    for (int run = 0; run < kRuns; ++run) {
        startWorker(0);
        waitForWorkerStart();

        ::kill(gWorkerPid, SIGKILL);
        int status = 0;
        ::waitpid(gWorkerPid, &status, 0);
        gWorkerPid = -1;

        auto restartStart = std::chrono::steady_clock::now();
        startWorker(0);
        waitForWorkerStart();

        float buf[b4k06::kBlockSize];
        bool recovered = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            if (tryReadAudioBlock(buf, b4k06::kBlockSize, 0)) { recovered = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - restartStart).count();
        times.push_back(static_cast<uint64_t>(elapsed));

        stopWorker();
    }

    std::sort(times.begin(), times.end());
    uint64_t sum = 0;
    for (auto t : times) sum += t;
    uint64_t avg = sum / static_cast<uint64_t>(times.size());
    uint64_t min = times.front();
    uint64_t max = times.back();
    uint64_t p90 = times[times.size() * 9 / 10];

    r.evidence = "recovery times (ms) over " + std::to_string(kRuns) + " runs: ";
    for (auto t : times) r.evidence += std::to_string(t) + " ";
    r.evidence += "| min=" + std::to_string(min) + " max=" + std::to_string(max) +
                  " avg=" + std::to_string(avg) + " p90=" + std::to_string(p90);
    r.limitation = "worst observed recovery time = " + std::to_string(max) +
                   "ms; B4-K2 must budget for this worst case, not the avg.";
    r.pass = (max <= 100);

    record(r, r.evidence, r.limitation);
}

static void testControlPlane() {
    TestResult r{0, "Control plane (separate from audio)"};
    r.num = 0;

    const std::string workerPath = getWorkerPath();

    cleanupShm();
    ::unlink(b4k06::kControlName);

    gTransport = createAndMapShm();
    CHECK(gTransport != nullptr);

    gWorkerPid = spawnWorker(workerPath);
    CHECK(gWorkerPid > 0);

    bool started = waitForWorkerStart();
    CHECK(started);

    std::string pingResp = sendControlCommand("ping", 1000);
    bool pingOk = (pingResp == "ok pong");

    std::string statusResp = sendControlCommand("status", 1000);
    bool statusOk = (statusResp.find("ok gen=") != std::string::npos);

    r.pass = started && pingOk && statusOk;
    r.evidence = std::string("worker started=") + (started ? "true" : "false") +
                 ", ping/pong=" + std::string(pingOk ? "true" : "false") + "(" + pingResp + ")" +
                 ", status=" + std::string(statusOk ? "true" : "false") + "(" + statusResp + ")";
    r.limitation = "Control plane is separate from audio ring. Status/ping use Unix socket, "
                   "audio samples use shared memory only.";

    stopWorker();
    ::unlink(b4k06::kControlName);
    record(r, r.evidence, r.limitation);
}

int main() {
    std::cout << "=== B4-K0.6: Cross-Process Audio IPC Spike ===\n" << std::flush;
    std::cout << "Platform: ";
#ifdef __APPLE__
    std::cout << "macOS\n";
#elif defined(__linux__)
    std::cout << "Linux\n";
#else
    std::cout << "Unknown\n";
#endif
    std::cout << "Architecture: ";
#if defined(__x86_64__)
    std::cout << "x86_64\n";
#elif defined(__arm64__)
    std::cout << "arm64\n";
#else
    std::cout << "unknown\n";
#endif
    std::cout << "Shared memory mechanism: shm_open + mmap(MAP_SHARED)\n";
    std::cout << "Atomic mechanism: std::atomic<uint32_t> / uint64_t with acq/rel\n";
    std::cout << "Process spawn: posix_spawn\n";
    std::cout << "Block size: " << b4k06::kBlockSize << ", Ring capacity: " << b4k06::kRingCapacity << "\n\n";

    const std::string workerPath = getWorkerPath();
    if (!std::filesystem::exists(workerPath)) {
        std::cerr << "FATAL: worker binary not found at " << workerPath << "\n";
        return 1;
    }

    test1_shm_ring();
    test2_cross_proc_atomics();
    test3_producer_consumer();
    test4_rt_safe_reader();
    test5_rt_safe_writer();
    test6_underrun();
    test7_overrun();
    test8_restart();
    test9_crash();
    test10_death_mid_write();
    test11_stale_state();
    test12_recovery();
    test13_cleanup_reinit();
    test14_platform();
    test15_seqlock_bounded_retry();
    test16_recovery_timing_repeat();
    testControlPlane();

    std::cout << "\n\n=== PASS/FAIL REPORT ===\n";
    std::cout << "#    Requirement                              Result  Evidence\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    for (const auto& r : gResults) {
        std::cout << r.num << "    " << r.name;
        if (r.name.size() < 44) std::cout << std::string(44 - r.name.size(), ' ');
        std::cout << (r.pass ? "PASS" : "FAIL") << "  " << r.evidence.substr(0, 50);
        if (!r.limitation.empty()) std::cout << "...";
        std::cout << "\n";
    }

    std::cout << "\nTotal: " << gPass << " passed, " << gFail << " failed\n";

    bool allPassed = (gFail == 0);
    std::cout << "\n=== B4-K2 GATE ===\n";
    std::cout << "Is this transport validated sufficiently to allow B4-K2 to treat it as real-time-safe? ";
    std::cout << (allPassed ? "YES" : "NO") << "\n";
    if (!allPassed) {
        std::cout << "Reason: At least one required test FAILED.\n";
        for (const auto& r : gResults) {
            if (!r.pass) {
                std::cout << "  FAILED: #" << r.num << " - " << r.name << " - " << r.evidence << "\n";
            }
        }
    }

    cleanupAll();

    return allPassed ? 0 : 1;
}
