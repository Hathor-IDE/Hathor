// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_terminal_process.cpp — L-4 unit tests for TerminalProcess and TaskRunner.
 *
 * JUCE-free: tests the subprocess lifecycle manager and task runner directly
 * without any JUCE dependency. Uses Catch2.
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "TerminalProcess.hpp"
#include "TaskRunner.hpp"

#include <chrono>
#include <thread>
#include <vector>

using hathor::ui::TerminalProcess;
using hathor::ui::TaskRunner;

// ---------------------------------------------------------------------------
// Helper: drain the ring buffer until empty, collecting all output.
// ---------------------------------------------------------------------------
static std::string drainAll(TerminalProcess& proc, int maxIters = 200)
{
    std::string result;
    char buf[4096];
    for (int i = 0; i < maxIters; ++i)
    {
        std::size_t n = proc.drainOutput(buf, sizeof(buf));
        if (n > 0)
            result.append(buf, n);
        else
            break; // nothing more available right now
    }
    return result;
}

// ---------------------------------------------------------------------------
// Tests: simple command (echo)
// ---------------------------------------------------------------------------

TEST_CASE("TerminalProcess launches and drains echo output", "[terminal]")
{
    TerminalProcess proc;

    // "echo" is universally available on POSIX systems.
    std::vector<std::string> argv = {"echo", "hello", "world"};

    REQUIRE(proc.launch(argv, ""));
    REQUIRE(proc.state() == TerminalProcess::State::Running);

    // Wait for the process to exit.
    auto status = proc.waitForExit(5000);
    REQUIRE(status.has_value());
    REQUIRE(status->exitCode == 0);

    // Drain output.
    std::string output = drainAll(proc);
    REQUIRE(output.find("hello") != std::string::npos);
    REQUIRE(output.find("world") != std::string::npos);

    REQUIRE(proc.state() == TerminalProcess::State::Done);
}

// ---------------------------------------------------------------------------
// Tests: exit code propagation
// ---------------------------------------------------------------------------

TEST_CASE("TerminalProcess reports non-zero exit code", "[terminal]")
{
    TerminalProcess proc;

    // `false` exits with code 1, `true` exits with 0.
    std::vector<std::string> argv = {"sh", "-c", "exit 42"};

    REQUIRE(proc.launch(argv, ""));

    auto status = proc.waitForExit(5000);
    REQUIRE(status.has_value());
    REQUIRE(status->exitCode == 42);
    REQUIRE_FALSE(status->exitedNormally == false); // exited normally with a code

    REQUIRE(proc.state() == TerminalProcess::State::Done);
}

// ---------------------------------------------------------------------------
// Tests: stderr capture
// ---------------------------------------------------------------------------

TEST_CASE("TerminalProcess captures stderr output", "[terminal]")
{
    TerminalProcess proc;

    // sh -c 'echo to-stderr 1>&2' sends "to-stderr" to stderr.
    std::vector<std::string> argv = {"sh", "-c", "echo to-stderr 1>&2"};

    REQUIRE(proc.launch(argv, ""));

    auto status = proc.waitForExit(5000);
    REQUIRE(status.has_value());

    std::string output = drainAll(proc);
    REQUIRE(output.find("to-stderr") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Tests: long-running command
// ---------------------------------------------------------------------------

TEST_CASE("TerminalProcess streams output from long-running command", "[terminal]")
{
    TerminalProcess proc;

    std::vector<std::string> argv = {"sh", "-c", "for i in 1 2 3; do echo \"tick $i\"; sleep 0.3; done"};

    REQUIRE(proc.launch(argv, ""));
    REQUIRE(proc.state() == TerminalProcess::State::Running);

    // Poll for output while the process runs.
    std::string collected;
    bool gotPartial = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

    while (std::chrono::steady_clock::now() < deadline)
    {
        char buf[1024];
        std::size_t n = proc.drainOutput(buf, sizeof(buf));
        if (n > 0)
        {
            collected.append(buf, n);
            gotPartial = true;
        }

        if (proc.state() == TerminalProcess::State::Done)
            break;

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    REQUIRE(proc.state() == TerminalProcess::State::Done);
    REQUIRE(gotPartial);
    REQUIRE(collected.find("tick 1") != std::string::npos);
    REQUIRE(collected.find("tick 3") != std::string::npos);

    auto status = proc.exitStatus();
    REQUIRE(status.exitCode == 0);
}

// ---------------------------------------------------------------------------
// Tests: cancellation
// ---------------------------------------------------------------------------

TEST_CASE("TerminalProcess cancels a running command", "[terminal]")
{
    TerminalProcess proc;

    // A command that sleeps for a long time.
    std::vector<std::string> argv = {"sh", "-c", "sleep 30"};

    REQUIRE(proc.launch(argv, ""));
    REQUIRE(proc.state() == TerminalProcess::State::Running);

    // Give it a moment to start.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Cancel it.
    proc.cancel(1000);
    REQUIRE(proc.state() == TerminalProcess::State::Exiting);

    // Wait for it to die.
    auto status = proc.waitForExit(5000);
    REQUIRE(status.has_value());
    REQUIRE(status->wasCancelled);

    REQUIRE(proc.state() == TerminalProcess::State::Done);
}

// ---------------------------------------------------------------------------
// Tests: launch failure (nonexistent command)
// ---------------------------------------------------------------------------

TEST_CASE("TerminalProcess reports error for nonexistent command", "[terminal]")
{
    TerminalProcess proc;

    std::vector<std::string> argv = {"/nonexistent/command/that/does/not/exist"};

    bool launched = proc.launch(argv, "");
    if (launched)
    {
        // On some systems, posix_spawn succeeds immediately and the shell
        // reports the error. Wait briefly for it to fail.
        auto status = proc.waitForExit(3000);
        if (status.has_value())
        {
            // The command should have failed (non-zero exit code).
            INFO("exitCode=" << status->exitCode << " signal=" << status->signal);
            REQUIRE_FALSE(status->exitCode == 0);
        }
    }
    else
    {
        // Launch itself failed — verify error message is set.
        REQUIRE_FALSE(proc.lastError().empty());
    }
}

// ---------------------------------------------------------------------------
// Tests: cleanup (shutdown)
// ---------------------------------------------------------------------------

TEST_CASE("TerminalProcess shutdown cleans up running process", "[terminal]")
{
    TerminalProcess proc;

    std::vector<std::string> argv = {"sh", "-c", "sleep 30"};
    REQUIRE(proc.launch(argv, ""));
    REQUIRE(proc.state() == TerminalProcess::State::Running);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Shutdown should terminate the process and join the worker thread.
    proc.shutdown();

    // After shutdown, the state is reset to Idle for reuse.
    REQUIRE(proc.state() == TerminalProcess::State::Idle);
}

// ---------------------------------------------------------------------------
// Tests: repeated launch after shutdown
// ---------------------------------------------------------------------------

TEST_CASE("TerminalProcess can relaunch after shutdown", "[terminal]")
{
    TerminalProcess proc;

    // First launch + shutdown
    {
        std::vector<std::string> argv = {"sh", "-c", "echo first"};
        REQUIRE(proc.launch(argv, ""));
        auto status = proc.waitForExit(5000);
        REQUIRE(status.has_value());
    }
    proc.shutdown();

    // Second launch
    {
        std::vector<std::string> argv = {"sh", "-c", "echo second"};
        REQUIRE(proc.launch(argv, ""));
        auto status = proc.waitForExit(5000);
        REQUIRE(status.has_value());
        REQUIRE(status->exitCode == 0);

        std::string output = drainAll(proc);
        REQUIRE(output.find("second") != std::string::npos);
    }
    proc.shutdown();
}

// ---------------------------------------------------------------------------
// TaskRunner tests
// ---------------------------------------------------------------------------

TEST_CASE("TaskRunner default tasks include build, test, and check", "[tasks]")
{
    TaskRunner runner("/tmp/fake-project", "/tmp/fake-project/build");
    auto tasks = runner.tasks();

    REQUIRE_FALSE(tasks.empty());

    // Verify expected task ids exist.
    bool hasBuild = false, hasTest = false, hasCheck = false;
    for (const auto& t : tasks)
    {
        if (t.id == "build") hasBuild = true;
        if (t.id == "test") hasTest = true;
        if (t.id == "check") hasCheck = true;
    }
    REQUIRE(hasBuild);
    REQUIRE(hasTest);
    REQUIRE(hasCheck);
}

TEST_CASE("TaskRunner expandPlaceholders replaces buildDir and projectDir", "[tasks]")
{
    TaskRunner runner("/Users/dev/myproject", "/Users/dev/myproject/build");

    std::string expanded = runner.expandPlaceholders("cmake --build {buildDir}");
    REQUIRE(expanded == "cmake --build /Users/dev/myproject/build");

    expanded = runner.expandPlaceholders("cd {projectDir} && cmake -B {buildDir}");
    REQUIRE(expanded.find("/Users/dev/myproject") != std::string::npos);
}

TEST_CASE("TaskRunner findTask returns nullptr for unknown id", "[tasks]")
{
    TaskRunner runner("/tmp", "/tmp/build");
    REQUIRE(runner.findTask("nonexistent") == nullptr);
}

TEST_CASE("TaskRunner findTask returns match for known id", "[tasks]")
{
    TaskRunner runner("/tmp", "/tmp/build");
    const auto* task = runner.findTask("build");
    REQUIRE(task != nullptr);
    REQUIRE_FALSE(task->command.empty());
}

TEST_CASE("TaskRunner defaults buildDir to <projectDir>/build", "[tasks]")
{
    TaskRunner runner("/tmp/my-proj"); // no explicit buildDir
    REQUIRE(runner.buildDir() == "/tmp/my-proj/build");
}

TEST_CASE("TaskRunner taskList returns id/label pairs", "[tasks]")
{
    TaskRunner runner("/tmp", "/tmp/build");
    auto list = runner.taskList();

    REQUIRE_FALSE(list.empty());
    for (const auto& [id, label] : list)
    {
        REQUIRE_FALSE(id.empty());
        REQUIRE_FALSE(label.empty());
    }
}
