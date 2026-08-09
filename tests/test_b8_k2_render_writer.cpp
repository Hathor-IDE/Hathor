// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "ChuckRenderWriter.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

using hathor::ChuckRenderWriter;
using hathor::RenderHandle;
using hathor::RenderResult;
using hathor::RenderState;

namespace {

constexpr unsigned kSampleRate = 44100;

uint16_t readU16LE(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readU32LE(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

struct WavInfo {
    bool     valid = false;
    uint32_t audioLengthBytes = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint16_t channels = 0;
};

WavInfo parseWav(const std::filesystem::path& path)
{
    WavInfo info{};
    std::ifstream f(path, std::ios::binary);
    if (!f) return info;

    f.seekg(0, std::ios::end);
    auto sz = f.tellg();
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    if (static_cast<size_t>(f.gcount()) < 44) return info;

    if (buf[0] != 'R' || buf[1] != 'I' || buf[2] != 'F' || buf[3] != 'R')
        return info;
    if (buf[8] != 'W' || buf[9] != 'A' || buf[10] != 'V' || buf[11] != 'E')
        return info;

    size_t offset = 12;
    bool   foundFmt   = false;
    bool   foundData  = false;
    while (offset + 8 <= buf.size()) {
        char id[4];
        std::memcpy(id, &buf[offset], 4);
        uint32_t chunkSize = readU32LE(&buf[offset + 4]);

        if (id[0] == 'f' && id[1] == 'm' && id[2] == 't' && id[3] == ' ') {
            info.sampleRate    = readU32LE(&buf[offset + 12]);
            info.channels      = readU16LE(&buf[offset + 10]);
            info.bitsPerSample = readU16LE(&buf[offset + 22]);
            foundFmt = true;
        } else if (id[0] == 'd' && id[1] == 'a' && id[2] == 't' && id[3] == 'a') {
            info.audioLengthBytes = chunkSize;
            foundData = true;
            break;
        }
        offset += 8 + chunkSize;
        if (offset % 2) ++offset;
    }
    info.valid = foundFmt && foundData;
    return info;
}

} // namespace

TEST_CASE("B8-K2 RenderHandle default-constructed", "[b8-k2]")
{
    RenderHandle h;
    REQUIRE(h.id() == 0);
    REQUIRE(h.state() == RenderState::Failed);
}

TEST_CASE("B8-K2 writer null worker produces error callback", "[b8-k2][worker]")
{
    auto tmp = std::filesystem::temp_directory_path() / "b8k2_null_worker.wav";
    std::filesystem::remove(tmp);

    bool callbackCalled = false;
    RenderResult result{};

    std::promise<void> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(nullptr);
    writer.startRender(0, "SinOsc s => dac;", 1024, kSampleRate, tmp,
        [&](const RenderResult& r) {
            callbackCalled = true;
            result = r;
            done.set_value();
        });

    REQUIRE(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(callbackCalled);
    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(result.errorMessage.empty());
    REQUIRE_FALSE(std::filesystem::exists(tmp));
}

TEST_CASE("B8-K2 writer activeRenderCount", "[b8-k2]")
{
    ChuckRenderWriter writer(nullptr);
    REQUIRE(writer.activeRenderCount() == 0);

    std::promise<void> done;
    auto fut = done.get_future();

    writer.startRender(0, "SinOsc s => dac;", 44100, kSampleRate,
        std::filesystem::temp_directory_path() / "b8k2_count.wav",
        [&](const RenderResult&) { done.set_value(); });

    REQUIRE(writer.activeRenderCount() == 1);

    REQUIRE(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    REQUIRE(writer.activeRenderCount() == 0);
}

TEST_CASE("B8-K2 writer shutdown is safe and idempotent", "[b8-k2]")
{
    ChuckRenderWriter writer(nullptr);
    writer.shutdown();
    writer.shutdown();
    REQUIRE(writer.activeRenderCount() == 0);
}

TEST_CASE("B8-K2 writer fails gracefully on null worker", "[b8-k2]")
{
    auto tmp = std::filesystem::temp_directory_path() / "b8k2_fail.wav";
    std::filesystem::remove(tmp);

    std::promise<void> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(nullptr);
    RenderHandle h = writer.startRender(
        0, "SinOsc s => dac;", 44100, kSampleRate, tmp,
        [&](const RenderResult&) { (void)0;
}
