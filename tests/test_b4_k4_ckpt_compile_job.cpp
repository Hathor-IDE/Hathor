// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b4_k4_ckpt_compile_job.cpp — B4-K4: async compile job lifecycle (Phase 2B/2C).
 *
 * Unit tests for ChuckCkJobService::queryJob() and the surrounding state machine.
 * JUCE-free: links hathor-audio-worker-lib + Catch2 only.
 *
 * Verifies:
 *   1. Unknown job returns {ok: false, error: "unknown job id"}.
 *   2. Newly submitted job is observable as queued.
 *   3. Running state is observable while the worker thread is live.
 *   4. Successful worker completion produces succeeded + structured result.
 *   5. Failed worker completion produces failed + diagnostics.
 *   6. Cancelled job reports cancelled.
 *   7. Completed state persists and is re-queryable.
 *   8. Concurrent queries do not corrupt job state.
 *   9. Diagnostic fields (line/column/message) are populated on failure.
 *  10. Shred ID / source hash are populated on success.
 *  11. Cancelling unknown job returns false.
 *  12. Cancelling already-terminal job returns false (succeeded, failed, cancelled).
 *  13. cancelJob() invokes the Canceller callback with the job's tabId.
 *  14. Cancelling before the worker thread starts still transitions to Cancelled.
 *  15. Successful completion does not fire the Canceller.
 *
 * Requirement references: AI-5 §5, AI-5 §6, AI-5 §7, B4-K4, B4-K7, AI-5 Phase 2C
 */

#include "ChuckCkJobService.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using hathor::audio_worker::ChuckCkJobService;
using hathor::audio_worker::CompileJobState;
using hathor::audio_worker::VMResult;

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

ChuckCkJobService::Publisher makePublisher(VMResult result)
{
    return [r = std::move(result)](uint8_t, const std::string&) -> VMResult {
        return r;
    };
}

ChuckCkJobService::Publisher makeDelayedPublisher(VMResult result,
                                                   std::chrono::milliseconds delay)
{
    return [r = std::move(result), delay](uint8_t, const std::string&) -> VMResult {
        std::this_thread::sleep_for(delay);
        return r;
    };
}

std::function<void(uint8_t)> makeCanceller(std::atomic<uint8_t>& outTabId,
                                            std::atomic<int>& outCallCount)
{
    return [&outTabId, &outCallCount](uint8_t tabId) {
        outTabId.store(tabId, std::memory_order_relaxed);
        outCallCount.fetch_add(1, std::memory_order_relaxed);
    };
}

bool waitForState(ChuckCkJobService& service,
                  uint64_t jobId,
                  CompileJobState expected,
                  std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        auto j = service.queryJob(jobId);
        if (!j.value("ok", false))
            return false;
        const std::string status = j.value("status", "");
        if (status == "unknown")
            return false;
        if (status == [&]() {
                switch (expected) {
                    case CompileJobState::Queued:    return "queued";
                    case CompileJobState::Running:   return "running";
                    case CompileJobState::Succeeded: return "succeeded";
                    case CompileJobState::Failed:    return "failed";
                    case CompileJobState::Cancelled: return "cancelled";
                }
                return "";
            }())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("queryJob: unknown job returns error", "[ckjob][query][unknown]")
{
    ChuckCkJobService service(makePublisher({false, 1, "not available"}));

    auto j = service.queryJob(999ULL);
    REQUIRE(j.value("ok", true) == false);
    REQUIRE(j.value("error", "") == "unknown job id");
    REQUIRE(j.value("job_id", 0ULL) == 999ULL);
}

TEST_CASE("queryJob: newly submitted job is queued", "[ckjob][query][queued]")
{
    ChuckCkJobService service(makeDelayedPublisher(
        {true, 0, "ok ck_compile tab=0 version=1 hash=abc123"}, std::chrono::milliseconds(50)));

    uint64_t jobId = service.startCompile(0, "SinOsc s => dac;", nullptr);
    REQUIRE(jobId > 0);

    auto j = service.queryJob(jobId);
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j.value("job_id", 0ULL) == jobId);
    REQUIRE(j.value("status", "") == "queued");

    service.shutdown();
}

TEST_CASE("queryJob: active compilation reports running", "[ckjob][query][running]")
{
    ChuckCkJobService service(makeDelayedPublisher(
        {true, 0, "ok ck_compile tab=0 version=1 hash=abc123"}, std::chrono::milliseconds(100)));

    uint64_t jobId = service.startCompile(0, "SinOsc s => dac;", nullptr);
    REQUIRE(jobId > 0);

    // Wait for the worker thread to transition to Running.
    REQUIRE(waitForState(service, jobId, CompileJobState::Running,
                         std::chrono::seconds(5)));

    auto j = service.queryJob(jobId);
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j.value("status", "") == "running");

    service.shutdown();
}

TEST_CASE("queryJob: successful worker completion is observable", "[ckjob][query][success]")
{
    ChuckCkJobService service(makePublisher(
        {true, 0, "ok ck_compile tab=3 version=2 hash=deadbeef shred=7"}));

    uint64_t jobId = service.startCompile(3, "SinOsc s => dac; 440 => s.freq;", nullptr);
    REQUIRE(jobId > 0);

    REQUIRE(waitForState(service, jobId, CompileJobState::Succeeded,
                         std::chrono::seconds(5)));

    auto j = service.queryJob(jobId);
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j.value("status", "") == "succeeded");
    REQUIRE(j.value("success", false) == true);

    const auto& result = j["result"];
    REQUIRE(result.value("success", false) == true);
    REQUIRE(result.value("source_hash", "") == "deadbeef");
    REQUIRE(result.value("shred_id", -1) == 7);
    REQUIRE(result.contains("diagnostics"));
    REQUIRE(result["diagnostics"].is_array());
    REQUIRE_FALSE(result["diagnostics"].empty());

    const auto& diag = result["diagnostics"][0];
    REQUIRE(diag.value("severity", "") == "info");
    REQUIRE(diag.value("code", "") == "CK_OK");

    service.shutdown();
}

TEST_CASE("queryJob: compilation failure is observable with diagnostics",
          "[ckjob][query][failure]")
{
    ChuckCkJobService service(makePublisher(
        {false, 1, "err ck_compile tab=1 version=1 error=unexpected ')' line=1 col=17"}));

    uint64_t jobId = service.startCompile(1, "SinOsc s => dac)", nullptr);
    REQUIRE(jobId > 0);

    REQUIRE(waitForState(service, jobId, CompileJobState::Failed,
                         std::chrono::seconds(5)));

    auto j = service.queryJob(jobId);
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j.value("status", "") == "failed");
    REQUIRE(j.value("success", true) == false);
    REQUIRE(j.value("error", "") == "unexpected ')'");

    const auto& result = j["result"];
    REQUIRE(result.value("success", true) == false);
    REQUIRE(result.value("error", "") == "unexpected ')'");
    REQUIRE(result.contains("diagnostics"));
    REQUIRE(result["diagnostics"].is_array());
    REQUIRE_FALSE(result["diagnostics"].empty());

    const auto& diag = result["diagnostics"][0];
    REQUIRE(diag.value("severity", "") == "error");
    REQUIRE(diag.value("code", "") == "CK_COMPILE_ERROR");
    REQUIRE(diag.value("message", "") == "unexpected ')'");
    REQUIRE(diag.value("line", 0) == 1);
    REQUIRE(diag.value("column", 0) == 17);

    service.shutdown();
}

TEST_CASE("queryJob: cancelled job reports cancelled", "[ckjob][query][cancelled]")
{
    std::atomic<uint8_t> cancelledTabId{255};
    std::atomic<int>     cancelCallCount{0};
    ChuckCkJobService service(
        makeDelayedPublisher(
            {true, 0, "ok ck_compile tab=0 version=1 hash=abc"}, std::chrono::milliseconds(500)),
        makeCanceller(cancelledTabId, cancelCallCount));

    uint64_t jobId = service.startCompile(0, "SinOsc s => dac;", nullptr);
    REQUIRE(jobId > 0);

    // Cancel before the worker completes.
    REQUIRE(service.cancelJob(jobId));

    REQUIRE(waitForState(service, jobId, CompileJobState::Cancelled,
                         std::chrono::seconds(5)));

    auto j = service.queryJob(jobId);
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j.value("status", "") == "cancelled");
    REQUIRE(j.value("success", true) == false);
    REQUIRE(j.value("error", "") == "job cancelled");

    // Canceller must have been invoked with the job's tabId (0).
    REQUIRE(cancelCallCount.load(std::memory_order_relaxed) == 1);
    REQUIRE(cancelledTabId.load(std::memory_order_relaxed) == 0);

    service.shutdown();
}

TEST_CASE("cancelJob: unknown job returns false", "[ckjob][cancel][unknown]")
{
    std::atomic<uint8_t> cancelledTabId{255};
    std::atomic<int>     cancelCallCount{0};
    ChuckCkJobService service(
        makePublisher({true, 0, "ok"}),
        makeCanceller(cancelledTabId, cancelCallCount));

    REQUIRE_FALSE(service.cancelJob(999ULL));
    REQUIRE(cancelCallCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(cancelledTabId.load(std::memory_order_relaxed) == 255);

    service.shutdown();
}

TEST_CASE("cancelJob: already-succeeded job returns false", "[ckjob][cancel][succeeded]")
{
    std::atomic<uint8_t> cancelledTabId{255};
    std::atomic<int>     cancelCallCount{0};
    ChuckCkJobService service(
        makePublisher({true, 0, "ok ck_compile tab=0 hash=abc"}),
        makeCanceller(cancelledTabId, cancelCallCount));

    uint64_t jobId = service.startCompile(0, "SinOsc s => dac;", nullptr);
    REQUIRE(waitForState(service, jobId, CompileJobState::Succeeded,
                         std::chrono::seconds(5)));

    REQUIRE_FALSE(service.cancelJob(jobId));
    REQUIRE(cancelCallCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(cancelledTabId.load(std::memory_order_relaxed) == 255);

    service.shutdown();
}

TEST_CASE("cancelJob: already-failed job returns false", "[ckjob][cancel][failed]")
{
    std::atomic<uint8_t> cancelledTabId{255};
    std::atomic<int>     cancelCallCount{0};
    ChuckCkJobService service(
        makePublisher({false, 1, "err ck_compile tab=0 error=syntax error"}),
        makeCanceller(cancelledTabId, cancelCallCount));

    uint64_t jobId = service.startCompile(0, "bad code !@#", nullptr);
    REQUIRE(waitForState(service, jobId, CompileJobState::Failed,
                         std::chrono::seconds(5)));

    REQUIRE_FALSE(service.cancelJob(jobId));
    REQUIRE(cancelCallCount.load(std::memory_order_relaxed) == 0);

    service.shutdown();
}

TEST_CASE("cancelJob: already-cancelled job returns false (idempotent)", "[ckjob][cancel][idempotent]")
{
    std::atomic<uint8_t> cancelledTabId{255};
    std::atomic<int>     cancelCallCount{0};
    ChuckCkJobService service(
        makeDelayedPublisher(
            {true, 0, "ok ck_compile tab=0 hash=abc"}, std::chrono::milliseconds(500)),
        makeCanceller(cancelledTabId, cancelCallCount));

    uint64_t jobId = service.startCompile(0, "SinOsc s => dac;", nullptr);
    REQUIRE(service.cancelJob(jobId));
    REQUIRE(waitForState(service, jobId, CompileJobState::Cancelled,
                         std::chrono::seconds(5)));

    // Second cancel must return false — already terminal.
    REQUIRE_FALSE(service.cancelJob(jobId));
    // Canceller must not fire again.
    REQUIRE(cancelCallCount.load(std::memory_order_relaxed) == 1);

    service.shutdown();
}

TEST_CASE("cancelJob: cancelling before thread start transitions to Cancelled",
          "[ckjob][cancel][pre_start]")
{
    std::atomic<uint8_t> cancelledTabId{255};
    std::atomic<int>     cancelCallCount{0};
    // Use a publisher that blocks long enough for us to cancel first.
    ChuckCkJobService service(
        makeDelayedPublisher(
            {true, 0, "ok ck_compile tab=0 hash=abc"}, std::chrono::milliseconds(1000)),
        makeCanceller(cancelledTabId, cancelCallCount));

    uint64_t jobId = service.startCompile(0, "SinOsc s => dac;", nullptr);

    // Cancel immediately — the worker thread may not have started yet.
    REQUIRE(service.cancelJob(jobId));

    REQUIRE(waitForState(service, jobId, CompileJobState::Cancelled,
                         std::chrono::seconds(5)));

    auto j = service.queryJob(jobId);
    REQUIRE(j.value("status", "") == "cancelled");
    REQUIRE(cancelCallCount.load(std::memory_order_relaxed) == 1);

    service.shutdown();
}

TEST_CASE("cancelJob: successful completion does not fire Canceller",
          "[ckjob][cancel][not_fired_on_success]")
{
    std::atomic<uint8_t> cancelledTabId{255};
    std::atomic<int>     cancelCallCount{0};
    ChuckCkJobService service(
        makePublisher({true, 0, "ok ck_compile tab=0 hash=abc shred=3"}),
        makeCanceller(cancelledTabId, cancelCallCount));

    uint64_t jobId = service.startCompile(0, "SinOsc s => dac;", nullptr);
    REQUIRE(waitForState(service, jobId, CompileJobState::Succeeded,
                         std::chrono::seconds(5)));

    REQUIRE(cancelCallCount.load(std::memory_order_relaxed) == 0);
    REQUIRE(cancelledTabId.load(std::memory_order_relaxed) == 255);

    service.shutdown();
}

TEST_CASE("queryJob: completed state remains queryable", "[ckjob][query][terminal]")
{
    ChuckCkJobService service(makePublisher(
        {true, 0, "ok ck_compile tab=2 version=1 hash=fff"}));

    uint64_t jobId = service.startCompile(2, "SinOsc s => dac;", nullptr);
    REQUIRE(jobId > 0);

    REQUIRE(waitForState(service, jobId, CompileJobState::Succeeded,
                         std::chrono::seconds(5)));

    // Query multiple times — terminal state must be stable.
    for (int i = 0; i < 5; ++i) {
        auto j = service.queryJob(jobId);
        REQUIRE(j.value("ok", false) == true);
        REQUIRE(j.value("status", "") == "succeeded");
        REQUIRE(j.value("success", false) == true);
        REQUIRE(j.contains("result"));
    }

    service.shutdown();
}

TEST_CASE("queryJob: concurrent queries do not corrupt state", "[ckjob][query][concurrent]")
{
    ChuckCkJobService service(makePublisher(
        {true, 0, "ok ck_compile tab=0 version=1 hash=ccc shred=3"}));

    uint64_t jobId = service.startCompile(0, "SinOsc s => dac;", nullptr);
    REQUIRE(jobId > 0);

    std::atomic<bool> anyFailure{false};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&service, jobId, &anyFailure]() {
            for (int q = 0; q < 50; ++q) {
                auto j = service.queryJob(jobId);
                if (!j.value("ok", false)) {
                    anyFailure.store(true, std::memory_order_release);
                    return;
                }
                const std::string status = j.value("status", "");
                if (status != "queued" && status != "running" &&
                    status != "succeeded") {
                    anyFailure.store(true, std::memory_order_release);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    for (auto& t : threads)
        t.join();

    REQUIRE_FALSE(anyFailure.load(std::memory_order_acquire));

    // After all threads finish, the job must be in a terminal state.
    REQUIRE(waitForState(service, jobId, CompileJobState::Succeeded,
                         std::chrono::seconds(5)));

    service.shutdown();
}

TEST_CASE("queryJob: bare transport error produces failed with error",
          "[ckjob][query][transport_error]")
{
    ChuckCkJobService service(makePublisher(
        {false, 1, "audio worker not available"}));

    uint64_t jobId = service.startCompile(0, "SinOsc s => dac;", nullptr);
    REQUIRE(jobId > 0);

    REQUIRE(waitForState(service, jobId, CompileJobState::Failed,
                         std::chrono::seconds(5)));

    auto j = service.queryJob(jobId);
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j.value("status", "") == "failed");
    REQUIRE(j.value("error", "") == "audio worker not available");

    const auto& result = j["result"];
    REQUIRE(result.value("success", true) == false);
    REQUIRE(result.value("error", "") == "audio worker not available");
    REQUIRE(result["diagnostics"].size() == 1);
    REQUIRE(result["diagnostics"][0].value("code", "") == "CK_COMPILE_ERROR");

    service.shutdown();
}

} // namespace
