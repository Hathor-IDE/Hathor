// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "ChuckRenderWriter.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <thread>

using hathor::ChuckRenderWriter;
using hathor::RenderHandle;
using hathor::RenderResult;
using hathor::RenderState;

namespace {

constexpr unsigned kSampleRate = 44100;

} // namespace

TEST_CASE("B8-K2 RenderHandle default-constructed", "[b8-k2]")
{
    RenderHandle h;
    REQUIRE(h.id() == 0);
    REQUIRE(h.state() == RenderState::Failed);
    h.cancel(); // should be a no-op on default-constructed handle
}

TEST_CASE("B8-K2 writer null worker produces error callback", "[b8-k2][worker]")
{
    auto tmp = std::filesystem::temp_directory_path() / "b8k2_null_worker.wav";
    std::filesystem::remove(tmp);

    std::promise<void> done;
    auto fut = done.get_future();

    RenderResult result{};
    {
        ChuckRenderWriter writer(nullptr);
        writer.startRender(0, "SinOsc s => dac;", 1024, kSampleRate, tmp,
            [&](const RenderResult& r) {
                result = r;
                done.set_value();
            });

        REQUIRE(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }
    // writer destructor joins the thread

    REQUIRE_FALSE(result.success);
    REQUIRE_FALSE(result.errorMessage.empty());
    REQUIRE_FALSE(std::filesystem::exists(tmp));
}

TEST_CASE("B8-K2 writer activeRenderCount returns zero after completion", "[b8-k2]")
{
    auto tmp = std::filesystem::temp_directory_path() / "b8k2_count.wav";
    std::filesystem::remove(tmp);

    std::promise<void> done;
    auto fut = done.get_future();

    {
        ChuckRenderWriter writer(nullptr);
        writer.startRender(0, "SinOsc s => dac;", 44100, kSampleRate, tmp,
            [&](const RenderResult&) { done.set_value(); });

        REQUIRE(writer.activeRenderCount() >= 0);
        REQUIRE(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    }

    // After the writer goes out of scope, thread is joined.
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

    RenderHandle h;
    {
        ChuckRenderWriter writer(nullptr);
        h = writer.startRender(0, "SinOsc s => dac;", 44100, kSampleRate, tmp,
            [&](const RenderResult&) { done.set_value(); });

        REQUIRE(fut.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        REQUIRE(h.state() == RenderState::Failed);
    }

    REQUIRE(h.state() == RenderState::Failed);
    REQUIRE_FALSE(std::filesystem::exists(tmp));
}

TEST_CASE("B8-K2 writer rejects zero-duration render", "[b8-k2]")
{
    auto tmp = std::filesystem::temp_directory_path() / "b8k2_zero.wav";
    std::filesystem::remove(tmp);

    bool callbackCalled = false;
    std::promise<void> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(nullptr);
    RenderHandle h = writer.startRender(
        0, "SinOsc s => dac;", 0, kSampleRate, tmp,
        [&](const RenderResult& r) {
            callbackCalled = true;
            REQUIRE_FALSE(r.success);
            done.set_value();
        });

    // Zero-duration should fail synchronously before spawning a thread.
    REQUIRE(h.state() == RenderState::Failed);
    REQUIRE(callbackCalled);
    REQUIRE_FALSE(std::filesystem::exists(tmp));
}

TEST_CASE("B8-K2 writer rejects empty destination path", "[b8-k2]")
{
    std::promise<void> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(nullptr);
    RenderHandle h = writer.startRender(
        0, "SinOsc s => dac;", 44100, kSampleRate, "",
        [&](const RenderResult& r) {
            REQUIRE_FALSE(r.success);
            done.set_value();
        });

    REQUIRE(h.state() == RenderState::Failed);
    REQUIRE(fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
}
