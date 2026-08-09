#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace b4k06 {

constexpr uint32_t kBlockSize = 64;
constexpr uint32_t kRingCapacity = 256;
constexpr uint32_t kRingMask = kRingCapacity - 1;
constexpr uint32_t kMagic = 0xB4A7D006;

static_assert((kRingCapacity & kRingMask) == 0, "kRingCapacity must be power of two");

struct AudioBlock {
    alignas(64) float samples[kBlockSize];
    std::atomic<uint32_t> sequence{0};
};

struct SharedAudioTransport {
    std::atomic<uint32_t> magic{kMagic};
    std::atomic<uint64_t> generation{0};

    std::atomic<uint32_t> writeSeq{0};
    std::atomic<uint32_t> readSeq{0};

    std::atomic<uint32_t> sampleRate{44100};
    std::atomic<uint32_t> channels{1};

    std::atomic<bool> workerAlive{false};
    std::atomic<uint64_t> lastHeartbeat{0};

    AudioBlock blocks[kRingCapacity];

    static constexpr size_t blockSize() { return kBlockSize; }
    static constexpr size_t ringCapacity() { return kRingCapacity; }
};

inline constexpr const char* kShmName = "/hathor-b4-k0-6";
inline constexpr size_t kShmSize = sizeof(SharedAudioTransport);

inline constexpr const char* kControlName = "hathor-b4-k0-6-control";

} // namespace b4k06
