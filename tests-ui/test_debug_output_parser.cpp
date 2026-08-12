// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_debug_output_parser.cpp — L-6 unit tests for the native debugger
 * output parser (DebugOutputParser.hpp).
 *
 * Parses realistic LLDB/GDB CLI output samples into structured frames,
 * locals, watches, and breakpoint confirmations.
 *
 * JUCE-free. Uses Catch2.
 */

#include <catch2/catch_test_macros.hpp>

#include "DebugOutputParser.hpp"

using hathor::ui::DebugStackFrame;
using hathor::ui::DebugWatchValue;
using hathor::ui::parseBreakpointConfirm;
using hathor::ui::parseLldbFrameLine;
using hathor::ui::parseLldbLocalsLine;
using hathor::ui::parseLldbWatchLine;
using hathor::ui::parseLldbStopLine;
using hathor::ui::parseGdbFrameLine;
using hathor::ui::parseGdbLocalsLine;
using hathor::ui::parseGdbWatchLine;
using hathor::ui::parseGdbStopLine;
using hathor::ui::debugTrim;

// ---------------------------------------------------------------------------
// LLDB frame lines
// ---------------------------------------------------------------------------

TEST_CASE("lldb frame line parses file/line/column/function", "[debug-parse]")
{
    DebugStackFrame f;
    REQUIRE(parseLldbFrameLine(
        "  frame #0: 0x0000000100002f0c hathor-ui`main at main.cpp:10:3", f));
    REQUIRE(f.function == "main");
    REQUIRE(f.file == "main.cpp");
    REQUIRE(f.line == 10);
    REQUIRE(f.column == 3);
}

TEST_CASE("lldb frame line with module-qualified function", "[debug-parse]")
{
    DebugStackFrame f;
    REQUIRE(parseLldbFrameLine(
        "    frame #2: 0x00007fff2045d8fd libdyld.dylib`start + 1", f));
    REQUIRE(f.function == "start + 1");
    REQUIRE(f.line == 0);   // no location → unknown
}

TEST_CASE("lldb frame line with path containing directories", "[debug-parse]")
{
    DebugStackFrame f;
    REQUIRE(parseLldbFrameLine(
        "  frame #1: 0x0000000100003f2c hathor-ui`HathorMain(int) at /Users/dev/hathor/src/Main.cpp:42:9", f));
    REQUIRE(f.function == "HathorMain(int)");
    REQUIRE(f.file == "/Users/dev/hathor/src/Main.cpp");
    REQUIRE(f.line == 42);
    REQUIRE(f.column == 9);
}

TEST_CASE("lldb non-frame lines are rejected", "[debug-parse]")
{
    DebugStackFrame f;
    REQUIRE_FALSE(parseLldbFrameLine("Process 123 stopped", f));
    REQUIRE_FALSE(parseLldbFrameLine("(lldb) ", f));
    REQUIRE_FALSE(parseLldbFrameLine("", f));
}

// ---------------------------------------------------------------------------
// LLDB locals / watches
// ---------------------------------------------------------------------------

TEST_CASE("lldb local line parses type/name/value", "[debug-parse]")
{
    DebugWatchValue v;
    REQUIRE(parseLldbLocalsLine("(int) myVar = 42", v));
    REQUIRE(v.type == "int");
    REQUIRE(v.name == "myVar");
    REQUIRE(v.value == "42");
}

TEST_CASE("lldb string local parses", "[debug-parse]")
{
    DebugWatchValue v;
    REQUIRE(parseLldbLocalsLine("(std::__1::string) s = \"hello world\"", v));
    REQUIRE(v.type == "std::__1::string");
    REQUIRE(v.name == "s");
    REQUIRE(v.value == "\"hello world\"");
}

TEST_CASE("lldb float local parses", "[debug-parse]")
{
    DebugWatchValue v;
    REQUIRE(parseLldbLocalsLine("(double) gain = 0.800000", v));
    REQUIRE(v.type == "double");
    REQUIRE(v.name == "gain");
    REQUIRE(v.value == "0.800000");
}

TEST_CASE("lldb watch expression result parses", "[debug-parse]")
{
    DebugWatchValue v;
    REQUIRE(parseLldbWatchLine("(float) 0.800000", v));
    REQUIRE(v.type == "float");
    REQUIRE(v.value == "0.800000");
    REQUIRE(v.name.empty());   // label is supplied by the caller
}

TEST_CASE("lldb watch error line is not misparsed as a value", "[debug-parse]")
{
    // Error lines are handled by DebugSession (fired via onError / the watch
    // error path) — the parser must stay strict and reject them.
    DebugWatchValue v;
    REQUIRE_FALSE(parseLldbWatchLine("error: use of undeclared identifier 'nope'", v));
}

// ---------------------------------------------------------------------------
// LLDB stop events
// ---------------------------------------------------------------------------

TEST_CASE("lldb stop reason line parses", "[debug-parse]")
{
    std::string reason;
    REQUIRE(parseLldbStopLine(
        "* thread #1, queue = 'com.apple.main-thread', stop reason = breakpoint 1.1", reason));
    REQUIRE(reason == "breakpoint 1.1");
}

TEST_CASE("lldb step stop reason parses", "[debug-parse]")
{
    std::string reason;
    REQUIRE(parseLldbStopLine(
        "* thread #1, stop reason = step over", reason));
    REQUIRE(reason == "step over");
}

TEST_CASE("lldb non-stop lines are rejected", "[debug-parse]")
{
    std::string reason;
    REQUIRE_FALSE(parseLldbStopLine("(lldb) ", reason));
    REQUIRE_FALSE(parseLldbStopLine("frame #0: ...", reason));
}

// ---------------------------------------------------------------------------
// GDB frames
// ---------------------------------------------------------------------------

TEST_CASE("gdb frame line parses", "[debug-parse]")
{
    DebugStackFrame f;
    REQUIRE(parseGdbFrameLine("#0  0x00005555555551a5 in main () at main.cpp:10", f));
    REQUIRE(f.function.find("main") != std::string::npos);
    REQUIRE(f.file == "main.cpp");
    REQUIRE(f.line == 10);
    REQUIRE(f.column == 0);
}

TEST_CASE("gdb frame line with args and full path parses", "[debug-parse]")
{
    DebugStackFrame f;
    REQUIRE(parseGdbFrameLine(
        "#1  0x00007ffff7a2b8fd in __libc_start_main (argc=1) at /usr/lib/libc/start.c:308", f));
    REQUIRE(f.function.find("__libc_start_main") != std::string::npos);
    REQUIRE(f.file == "/usr/lib/libc/start.c");
    REQUIRE(f.line == 308);
}

TEST_CASE("gdb non-frame lines are rejected", "[debug-parse]")
{
    DebugStackFrame f;
    REQUIRE_FALSE(parseGdbFrameLine("Breakpoint 1, main () at main.cpp:10", f));
    REQUIRE_FALSE(parseGdbFrameLine("(gdb) ", f));
}

// ---------------------------------------------------------------------------
// GDB locals / watches
// ---------------------------------------------------------------------------

TEST_CASE("gdb local line parses name/value", "[debug-parse]")
{
    DebugWatchValue v;
    REQUIRE(parseGdbLocalsLine("x = 42", v));
    REQUIRE(v.name == "x");
    REQUIRE(v.value == "42");
    REQUIRE(v.type.empty());
}

TEST_CASE("gdb string local parses", "[debug-parse]")
{
    DebugWatchValue v;
    REQUIRE(parseGdbLocalsLine("s = \"hello\"", v));
    REQUIRE(v.name == "s");
    REQUIRE(v.value == "\"hello\"");
}

TEST_CASE("gdb watch result parses", "[debug-parse]")
{
    DebugWatchValue v;
    REQUIRE(parseGdbWatchLine("$1 = 42", v));
    REQUIRE(v.name == "$1");
    REQUIRE(v.value == "42");
}

TEST_CASE("gdb container value parses", "[debug-parse]")
{
    DebugWatchValue v;
    REQUIRE(parseGdbWatchLine("$2 = std::vector of length 3, capacity 4 = {1, 2, 3}", v));
    REQUIRE(v.value.find("std::vector") != std::string::npos);
}

// ---------------------------------------------------------------------------
// GDB stop events
// ---------------------------------------------------------------------------

TEST_CASE("gdb breakpoint stop line parses", "[debug-parse]")
{
    std::string fn;
    REQUIRE(parseGdbStopLine("Breakpoint 1, main () at main.cpp:10", fn));
    REQUIRE(fn.find("main") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Breakpoint confirmations
// ---------------------------------------------------------------------------

TEST_CASE("lldb breakpoint confirmation parses number", "[debug-parse]")
{
    int bp = 0;
    bool pending = true;
    REQUIRE(parseBreakpointConfirm(
        "Breakpoint 1: where = main() at main.cpp:10, address = 0x0000000100003f2c", bp, pending));
    REQUIRE(bp == 1);
    REQUIRE_FALSE(pending);
}

TEST_CASE("lldb pending breakpoint is flagged", "[debug-parse]")
{
    int bp = 0;
    bool pending = false;
    REQUIRE(parseBreakpointConfirm(
        "Breakpoint 1: no locations (pending); will not get hit until a library is loaded.", bp, pending));
    REQUIRE(bp == 1);
    REQUIRE(pending);
}

TEST_CASE("gdb breakpoint confirmation parses number", "[debug-parse]")
{
    int bp = 0;
    bool pending = true;
    REQUIRE(parseBreakpointConfirm("Breakpoint 1 at 0x00005555555551a5: file main.cpp, line 10.", bp, pending));
    REQUIRE(bp == 1);
    REQUIRE_FALSE(pending);
}

TEST_CASE("non-breakpoint lines are rejected", "[debug-parse]")
{
    int bp = 0;
    bool pending = false;
    REQUIRE_FALSE(parseBreakpointConfirm("Breakpoint 1, main () at main.cpp:10", bp, pending));
    REQUIRE_FALSE(parseBreakpointConfirm("Process 123 stopped", bp, pending));
}

// ---------------------------------------------------------------------------
// Trim helper
// ---------------------------------------------------------------------------

TEST_CASE("debugTrim removes surrounding whitespace", "[debug-parse]")
{
    REQUIRE(debugTrim("  hello \t\n") == "hello");
    REQUIRE(debugTrim("") == "");
}
