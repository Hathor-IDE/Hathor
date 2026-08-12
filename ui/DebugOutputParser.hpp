// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * DebugOutputParser.hpp — L-6: JUCE-free parsing of native debugger CLI output.
 *
 * DebugSession wraps LLDB (macOS/Linux) and GDB (Linux) over stdio pipes.
 * This header provides the small, deterministic set of parsers needed to
 * turn the debuggers' text output into structured data:
 *
 *   - breakpoint-set confirmations (including "pending" locations)
 *   - stop events (breakpoint hit / signal / step complete)
 *   - call-stack frame lines (lldb `frame #N:` and gdb `#N` formats)
 *   - local-variable lines (lldb `(type) name = value`, gdb `name = value`)
 *   - watch/expression evaluation output (lldb `(type) value`, gdb `$N = value`)
 *
 * Deliberately conservative: any line that does not match a known format is
 * ignored (returned false) rather than mis-parsed.  The raw debugger stream
 * remains available to the UI via DebugSession::onOutput.
 *
 * No JUCE dependency — fully unit-testable without the GUI stack.
 *
 * Requirement references: L-6 §Native/C++ Debugging
 */

#include <cstdint>
#include <string>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Structured debugger data
// ---------------------------------------------------------------------------

/// A single source-level call-stack frame.
struct DebugStackFrame
{
    int         line   = 0;      ///< 1-based line number (0 if unknown)
    int         column = 0;      ///< 1-based column (0 if unknown)
    std::string function;        ///< function/method name (may include args for gdb)
    std::string file;            ///< source file path (empty if unknown)
};

/// A single local variable or watch evaluation result.
struct DebugWatchValue
{
    std::string name;            ///< variable name or watch label (may be empty)
    std::string type;            ///< declared type (empty when the debugger omits it)
    std::string value;           ///< textual value
};

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

/// Trim ASCII whitespace from both ends.
std::string debugTrim(const std::string& s);

/**
 * Parse a breakpoint-set confirmation line.
 *
 * lldb: "Breakpoint 1: where = main() at main.cpp:10, address = ..."
 *       "Breakpoint 1: no locations (pending); will not get hit until ..."
 * gdb:  "Breakpoint 1 at 0x...: file main.cpp, line 10."
 *
 * @param line     The debugger output line.
 * @param bpNumber Receives the breakpoint number (> 0) on success.
 * @param pending  Receives true when lldb reports "no locations (pending)".
 * @return true if the line is a breakpoint confirmation.
 */
bool parseBreakpointConfirm(const std::string& line, int& bpNumber, bool& pending);

// ---------------------------------------------------------------------------
// LLDB parsers
// ---------------------------------------------------------------------------

/// lldb frame line: "  frame #0: 0x... <module>`<func> at <file>:<line>:<col>"
bool parseLldbFrameLine(const std::string& line, DebugStackFrame& out);

/// lldb local: "(int) myVar = 42"  →  type="int", name="myVar", value="42"
bool parseLldbLocalsLine(const std::string& line, DebugWatchValue& out);

/// lldb expression result: "(float) 0.8" or "(int) $0 = 42"  →
/// type="float", value="0.8" (the "$N = " result-register prefix is
/// stripped; the caller supplies the watch label)
bool parseLldbWatchLine(const std::string& line, DebugWatchValue& out);

/// lldb stop line: "* thread #1, ..., stop reason = breakpoint 1.1"
/// Fills reasonOut with the text after "stop reason = ".
bool parseLldbStopLine(const std::string& line, std::string& reasonOut);

// ---------------------------------------------------------------------------
// GDB parsers
// ---------------------------------------------------------------------------

/// gdb frame line: "#0  0x... in main () at /path/main.cpp:10"
bool parseGdbFrameLine(const std::string& line, DebugStackFrame& out);

/// gdb local: "x = 42"  →  name="x", value="42"
bool parseGdbLocalsLine(const std::string& line, DebugWatchValue& out);

/// gdb print result: "$1 = 42"  →  value="42"
bool parseGdbWatchLine(const std::string& line, DebugWatchValue& out);

/// gdb breakpoint-stop line: "Breakpoint 1, main () at main.cpp:10"
bool parseGdbStopLine(const std::string& line, std::string& functionOut);

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

namespace detail {

inline std::string rsplitTo(std::string s, char sep, std::string& restOut)
{
    // Takes the input by value so callers may pass the same string as
    // `restOut` without aliasing (C++ argument evaluation order is
    // unspecified — reading a mutated `s` through the reference would be UB).
    const auto pos = s.rfind(sep);
    if (pos == std::string::npos)
    {
        restOut = s;
        return {};
    }
    restOut = s.substr(0, pos);
    return s.substr(pos + 1);
}

} // namespace detail

inline std::string debugTrim(const std::string& s)
{
    std::string::size_type b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    std::string::size_type e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

inline bool parseBreakpointConfirm(const std::string& line, int& bpNumber, bool& pending)
{
    pending = false;
    std::string trimmed = debugTrim(line);

    if (trimmed.rfind("Breakpoint ", 0) != 0)
        return false;

    const std::string rest = trimmed.substr(std::string("Breakpoint ").size());
    std::string numStr;
    std::string afterNum;
    {
        const auto pos = rest.find_first_of(" :");
        if (pos == std::string::npos)
            return false;
        numStr = rest.substr(0, pos);
        afterNum = rest.substr(pos);
    }

    try
    {
        const std::size_t idx = numStr.find_first_not_of("0123456789");
        if (idx != std::string::npos)
            return false;
        bpNumber = std::stoi(numStr);
    }
    catch (...)
    {
        return false;
    }

    if (bpNumber <= 0)
        return false;

    // lldb: "Breakpoint 1: no locations (pending); will not get hit ..."
    if (afterNum.find("pending") != std::string::npos)
        pending = true;

    return true;
}

inline bool parseLldbFrameLine(const std::string& line, DebugStackFrame& out)
{
    std::string trimmed = debugTrim(line);

    // Format: "frame #0: 0x<addr> <module>`<func> at <file>:<line>:<col>"
    if (trimmed.rfind("frame #", 0) != 0)
        return false;

    const auto backtick = trimmed.rfind('`');
    if (backtick == std::string::npos)
        return false;

    // Function = text after the module backtick, up to " at ".
    std::string afterBt = trimmed.substr(backtick + 1);
    const auto atPos = afterBt.find(" at ");
    if (atPos == std::string::npos)
    {
        // Frame without source location (assembly/system frames) — keep the
        // function name, leave line/column unknown.
        out.function = debugTrim(afterBt);
        out.file.clear();
        out.line = 0;
        out.column = 0;
        return !out.function.empty();
    }

    out.function = debugTrim(afterBt.substr(0, atPos));
    std::string location = debugTrim(afterBt.substr(atPos + 4));
    if (location.empty())
        return false;

    // location = "<file>:<line>:<col>" (split from the right; column may be absent)
    std::string colStr = detail::rsplitTo(location, ':', location);
    std::string lineStr = detail::rsplitTo(location, ':', location);
    out.file = location;

    try
    {
        out.column = colStr.empty() ? 0 : std::stoi(colStr);
        out.line   = lineStr.empty() ? 0 : std::stoi(lineStr);
    }
    catch (...)
    {
        out.line = 0;
        out.column = 0;
    }

    return true;
}

inline bool parseLldbLocalsLine(const std::string& line, DebugWatchValue& out)
{
    std::string trimmed = debugTrim(line);
    if (trimmed.empty() || trimmed[0] != '(')
        return false;

    const auto closeParen = trimmed.find(')');
    if (closeParen == std::string::npos)
        return false;

    out.type = debugTrim(trimmed.substr(1, closeParen - 1));

    // Skip type-qualifier fragments lldb sometimes emits before the name
    // (e.g. "(int) $0 = 42" is a watch, not a local — handled elsewhere).
    std::string rest = debugTrim(trimmed.substr(closeParen + 1));
    const auto eqPos = rest.find(" = ");
    if (eqPos == std::string::npos)
        return false;

    out.name = debugTrim(rest.substr(0, eqPos));
    out.value = debugTrim(rest.substr(eqPos + 3));
    return true;
}

inline bool parseLldbWatchLine(const std::string& line, DebugWatchValue& out)
{
    std::string trimmed = debugTrim(line);
    if (trimmed.empty() || trimmed[0] != '(')
        return false;

    const auto closeParen = trimmed.find(')');
    if (closeParen == std::string::npos)
        return false;

    out.type = debugTrim(trimmed.substr(1, closeParen - 1));
    out.name.clear();

    std::string rest = debugTrim(trimmed.substr(closeParen + 1));

    // Strip the result-register prefix lldb assigns to expression results:
    //   "(int) $0 = 42"  →  value "42"
    if (rest.size() > 1 && rest[0] == '$')
    {
        const auto eqPos = rest.find(" = ");
        if (eqPos != std::string::npos)
            rest = debugTrim(rest.substr(eqPos + 3));
    }

    out.value = rest;
    return !out.value.empty();
}

inline bool parseLldbStopLine(const std::string& line, std::string& reasonOut)
{
    std::string trimmed = debugTrim(line);
    const auto pos = trimmed.find("stop reason = ");
    if (pos == std::string::npos)
        return false;
    reasonOut = debugTrim(trimmed.substr(pos + std::string("stop reason = ").size()));
    return true;
}

inline bool parseGdbFrameLine(const std::string& line, DebugStackFrame& out)
{
    std::string trimmed = debugTrim(line);
    if (trimmed.empty() || trimmed[0] != '#')
        return false;

    // "#<n>  <addr> in <func> () at <file>:<line>"   (line only, no column)
    const auto atPos = trimmed.find(" at ");
    if (atPos == std::string::npos)
        return false;

    std::string funcPart = debugTrim(trimmed.substr(0, atPos));
    const auto inPos = funcPart.find(" in ");
    if (inPos == std::string::npos)
        return false;

    out.function = debugTrim(funcPart.substr(inPos + 4));

    std::string location = debugTrim(trimmed.substr(atPos + 4));
    std::string lineStr = detail::rsplitTo(location, ':', location);
    out.file = location;

    try
    {
        out.line = lineStr.empty() ? 0 : std::stoi(lineStr);
    }
    catch (...)
    {
        out.line = 0;
    }
    out.column = 0;
    return true;
}

inline bool parseGdbLocalsLine(const std::string& line, DebugWatchValue& out)
{
    std::string trimmed = debugTrim(line);
    if (trimmed.empty())
        return false;

    const auto eqPos = trimmed.find(" = ");
    if (eqPos == std::string::npos)
        return false;

    out.name = debugTrim(trimmed.substr(0, eqPos));
    out.value = debugTrim(trimmed.substr(eqPos + 3));
    out.type.clear();
    return !out.name.empty();
}

inline bool parseGdbWatchLine(const std::string& line, DebugWatchValue& out)
{
    std::string trimmed = debugTrim(line);
    if (trimmed.empty() || trimmed[0] != '$')
        return false;

    const auto eqPos = trimmed.find(" = ");
    if (eqPos == std::string::npos)
        return false;

    out.name = debugTrim(trimmed.substr(0, eqPos));   // e.g. "$1"
    out.value = debugTrim(trimmed.substr(eqPos + 3));
    out.type.clear();
    return true;
}

inline bool parseGdbStopLine(const std::string& line, std::string& functionOut)
{
    std::string trimmed = debugTrim(line);
    if (trimmed.rfind("Breakpoint ", 0) != 0)
        return false;

    const auto commaPos = trimmed.find(',');
    if (commaPos == std::string::npos)
        return false;

    functionOut = debugTrim(trimmed.substr(commaPos + 1));
    return true;
}

} // namespace hathor::ui
