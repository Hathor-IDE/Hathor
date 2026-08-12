// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_debug_session.cpp — L-6 unit tests for the DebugSession native
 * debugger integration surface.
 *
 * The full debugger workflow needs a real LLDB/GDB process, which is not
 * available in headless CI.  These tests verify the deterministic,
 * debugger-independent parts of the contract:
 *
 *   - platform detection never throws and returns a valid enum;
 *   - all command methods are safe no-ops when no session is running;
 *   - launch() validates its configuration and reports errors explicitly;
 *   - shutdown() is idempotent and safe.
 *
 * (The stdin command plumbing — writeStdin round-trip — is covered in
 * test_terminal_process.cpp, which drives a real child process.)
 *
 * JUCE-free. Uses Catch2.
 */

#include <catch2/catch_test_macros.hpp>

#include "DebugSession.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

using hathor::ui::DebugSession;

TEST_CASE("DebugSession platform detection returns a valid enum", "[debug-session]")
{
    const auto type = DebugSession::detectDebugger();
    const bool valid = (type == DebugSession::DebuggerType::Lldb ||
                        type == DebugSession::DebuggerType::Gdb ||
                        type == DebugSession::DebuggerType::None);
    REQUIRE(valid);
}

TEST_CASE("DebugSession starts not running with no error", "[debug-session]")
{
    DebugSession session;
    REQUIRE_FALSE(session.isRunning());
    REQUIRE(session.lastError().empty());
}

TEST_CASE("DebugSession commands before launch are safe no-ops", "[debug-session]")
{
    DebugSession session;

    session.continue_();
    session.interrupt();
    session.stepOver();
    session.stepInto();
    session.stepOut();
    session.requestCallStack();
    session.requestLocals();
    session.evaluateWatch("foo", "foo");
    session.pollResults();

    REQUIRE_FALSE(session.isRunning());
}

TEST_CASE("DebugSession setBreakpoint before launch fails with a clear error", "[debug-session]")
{
    DebugSession session;
    const int id = session.setBreakpoint("main.cpp", 10);
    REQUIRE(id == -1);
    REQUIRE_FALSE(session.lastError().empty());
    REQUIRE(session.listBreakpoints().empty());
}

TEST_CASE("DebugSession launch with empty executable fails explicitly", "[debug-session]")
{
    DebugSession session;

    DebugSession::Config cfg;
    cfg.executable.clear();

    const std::string err = session.launch(cfg);
    REQUIRE_FALSE(err.empty());
    REQUIRE_FALSE(session.isRunning());
}

TEST_CASE("DebugSession deleteBreakpoint before launch returns false", "[debug-session]")
{
    DebugSession session;
    REQUIRE_FALSE(session.deleteBreakpoint(1));
}

TEST_CASE("DebugSession shutdown is safe when not running", "[debug-session]")
{
    DebugSession session;
    session.shutdown();
    session.shutdown();   // idempotent
    REQUIRE_FALSE(session.isRunning());
}

// ---------------------------------------------------------------------------
// End-to-end: drive a REAL LLDB against a tiny compiled C program.
//
// This is the L-6 evidence that the deterministic debugging workflow actually
// provides useful information: set breakpoint → continue → stop → call stack
// → locals → watch.  Skipped automatically when LLDB or a C compiler is not
// available (headless CI, Windows).
// ---------------------------------------------------------------------------

TEST_CASE("DebugSession drives real LLDB end-to-end", "[debug-session-lldb]")
{
    if (DebugSession::detectDebugger() != DebugSession::DebuggerType::Lldb)
        SKIP("LLDB not available on this machine.");

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "hathor_dbg_selftest";
    fs::create_directories(tmp);
    const fs::path src = tmp / "prog.c";
    const fs::path exe = tmp / "prog";

    {
        std::ofstream f(src);
        f << "int compute(int x) {\n"      // line 1
          << "  int y = x * 2;\n"           // line 2 (y assigned here)
          << "  return y + 1;\n"            // line 3 (breakpoint — y is 42)
          << "}\n"                          // line 4
          << "int main(void) {\n"           // line 5
          << "  int a = compute(21);\n"     // line 6
          << "  return a;\n"                // line 7
          << "}\n";                         // line 8
    }

    // Compile with debug info (best effort; skip if no compiler is present).
    const int rc = std::system(("cc -g -O0 \"" + src.string() +
                                "\" -o \"" + exe.string() + "\" 2>/dev/null").c_str());
    if (rc != 0 || !fs::exists(exe))
    {
        fs::remove_all(tmp);
        SKIP("No C compiler available to build the debug target.");
    }

    DebugSession session;
    bool gotStop = false;
    bool gotLocals = false;
    bool gotWatch = false;
    std::vector<DebugSession::StackFrame> stopFrames;
    std::string sessionOutput;   // for failure diagnostics

    session.onStopped = [&](std::vector<DebugSession::StackFrame> frames) {
        gotStop = true;
        stopFrames = std::move(frames);
    };
    std::string localsDiag;   // for failure diagnostics
    session.onLocals = [&](std::vector<DebugSession::WatchValue> values) {
        localsDiag += "[onLocals n=" + std::to_string(values.size()) + "]";
        for (const auto& v : values)
        {
            localsDiag += " '" + v.name + "'=" + v.value;
            if (v.name == "y")   // compute()'s local, should be 42
                gotLocals = true;
        }
        localsDiag += "\n";
    };
    session.onWatchValue = [&](DebugSession::WatchValue v) {
        if (v.name == "y" && v.value.find("42") != std::string::npos)
            gotWatch = true;
    };
    session.onOutput = [&](std::string line) {
        if (sessionOutput.size() < 4000)
            sessionOutput += line + "\n";
    };
    session.onError = [&](std::string err) {
        if (sessionOutput.size() < 4000)
            sessionOutput += "[err] " + err + "\n";
    };

    const std::string err = session.launch(DebugSession::Config{exe.string(), tmp.string(), {}});
    REQUIRE(err.empty());

    // Breakpoint after y is assigned (line 3), then continue.
    REQUIRE(session.setBreakpoint("prog.c", 3) > 0);
    session.continue_();

    // Poll until the breakpoint stop is reported.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!gotStop && std::chrono::steady_clock::now() < deadline)
    {
        session.pollResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!gotStop)
    {
        session.shutdown();
        const std::string diag = "LLDB never reported a breakpoint stop. Output:\n" + sessionOutput;
        fs::remove_all(tmp);
        FAIL(diag);
    }

    // The stop must carry a useful call stack (compute → main).
    REQUIRE_FALSE(stopFrames.empty());
    REQUIRE(stopFrames.front().line >= 1);

    // Locals at the stop point.
    session.requestLocals();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!gotLocals && std::chrono::steady_clock::now() < deadline)
    {
        session.pollResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!gotLocals)
    {
        session.shutdown();
        const std::string diag = "Locals never arrived. Output:\n" + sessionOutput +
                                 "\nLocals diag:\n" + localsDiag;
        fs::remove_all(tmp);
        FAIL(diag);
    }

    // Watch evaluation (y should be 42 at line 3).
    session.evaluateWatch("y", "y");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!gotWatch && std::chrono::steady_clock::now() < deadline)
    {
        session.pollResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(gotWatch);

    // Continue to completion.  The debugger shell itself stays alive, so
    // just verify the command round-trips without error, then shut down.
    session.continue_();
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline)
    {
        session.pollResults();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    session.shutdown();
    fs::remove_all(tmp);
}
