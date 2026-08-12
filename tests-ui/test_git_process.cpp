// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "GitProcess.hpp"

using hathor::ui::GitProcess;

TEST_CASE("GitProcess runs sync echo", "[GitProcess]")
{
    GitProcess process;

    // 'git --version' should always succeed on a dev machine.
    auto result = process.runSync({ "--version" }, ".", 10000);

    REQUIRE(result.exitCode == 0);
    REQUIRE_FALSE(result.output.empty());
    REQUIRE(result.output.find("git version") != std::string::npos);
    REQUIRE_FALSE(result.timedOut);
}

TEST_CASE("GitProcess handles unknown command gracefully", "[GitProcess]")
{
    GitProcess process;

    auto result = process.runSync({ "--not-a-real-flag" }, ".", 10000);

    // git exits non-zero for unknown flags.
    REQUIRE(result.exitCode != 0);
    REQUIRE_FALSE(result.timedOut);
}

TEST_CASE("GitProcess async callback completes", "[GitProcess]")
{
    GitProcess process;

    std::atomic<bool> called{false};
    std::string capturedOutput;

    process.runAsync({ "--version" }, ".",
        [&called, &capturedOutput](const GitProcess::CompletionResult& result)
        {
            called.store(true);
            capturedOutput = result.output;
        }, 10000);

    // Wait for the async callback (up to 5 seconds).
    for (int i = 0; i < 500 && !called.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(called.load());
    REQUIRE_FALSE(capturedOutput.empty());
}
