// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * DebugSession.cpp — L-6: native/C++ debugging integration implementation.
 *
 * Wraps the platform debugger CLI (LLDB on macOS/Linux, GDB on Linux) as a
 * child process with a stdin command pipe.  On Windows the session is
 * unsupported and launch() returns an explicit error — no fake debugger.
 *
 * Commands are written to the debugger's stdin (non-blocking) and results
 * are parsed from stdout/stderr via DebugOutputParser.hpp.  All parsing and
 * callback delivery happens in pollResults(), which the UI drives from a
 * timer on the JUCE message thread.  The audio thread never touches this
 * class.
 *
 * Requirement references: L-6 §Native/C++ Debugging
 */

#include "DebugSession.hpp"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#else
#  include <signal.h>
#  include <sys/types.h>
#endif

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Platform detection
// ---------------------------------------------------------------------------

DebugSession::DebuggerType DebugSession::detectDebugger() noexcept
{
#if defined(_WIN32)
    // Windows: no native GDB/LLDB CLI integration here.  This is explicit,
    // not a stub — the UI surfaces this message when the panel is opened.
    return DebuggerType::None;
#else
    // macOS and Linux: prefer LLDB if available, fall back to GDB.
    if (std::filesystem::exists("/usr/bin/lldb") ||
        std::filesystem::exists("/usr/local/bin/lldb") ||
        std::filesystem::exists("/opt/homebrew/bin/lldb"))
        return DebuggerType::Lldb;
    if (std::filesystem::exists("/usr/bin/gdb") ||
        std::filesystem::exists("/usr/local/bin/gdb"))
        return DebuggerType::Gdb;
    return DebuggerType::None;
#endif
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DebugSession::DebugSession() = default;

DebugSession::~DebugSession()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Error access
// ---------------------------------------------------------------------------

std::string DebugSession::lastError() const
{
    std::lock_guard<std::mutex> lock(errMtx_);
    return lastError_;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::string DebugSession::launch(const Config& config)
{
    debuggerType_ = detectDebugger();

    if (debuggerType_ == DebuggerType::None) {
        std::lock_guard<std::mutex> lock(errMtx_);
        lastError_ = "No native debugger (LLDB/GDB) is available on this platform. "
                     "Native C++ debugging is not supported here.";
        return lastError_;
    }

    if (config.executable.empty()) {
        std::lock_guard<std::mutex> lock(errMtx_);
        lastError_ = "No executable path specified for debugging.";
        return lastError_;
    }

    // Build the debugger command vector.
    std::vector<std::string> argv;
    if (debuggerType_ == DebuggerType::Lldb) {
        // lldb -- <target> <args...>   ("--" ends option parsing)
        argv = {"lldb", "--", config.executable};
    } else {
        // gdb --args <target> <args...>
        argv = {"gdb", "--args", config.executable};
    }
    argv.insert(argv.end(), config.args.begin(), config.args.end());

    // Launch via TerminalProcess with a stdin pipe so we can drive the
    // debugger interactively.  This is non-blocking — the debugger's
    // stdout/stderr stream through the lock-free output ring.
    if (!process_.launch(argv, config.sourceDir, /*needStdin=*/true)) {
        std::lock_guard<std::mutex> lock(errMtx_);
        lastError_ = "Failed to launch debugger: " + process_.lastError();
        return lastError_;
    }

    running_.store(true, std::memory_order_relaxed);
    hasStarted_ = false;
    pendingStop_ = false;
    pendingLocals_ = false;
    pendingFrames_.clear();
    pendingLocalValues_.clear();
    pendingWatchLabel_.clear();
    lastCommand_.clear();
    pendingLocalsCmd_.clear();
    quietPolls_ = 0;

    // Send non-interactive-mode init commands (gdb only — lldb does not
    // paginate when stdin is not a tty).
    if (debuggerType_ == DebuggerType::Gdb) {
        sendCommand("set pagination off");
        sendCommand("set confirm off");
    }

    return {};
}

void DebugSession::shutdown()
{
    if (!running_.load(std::memory_order_relaxed))
        return;

    running_.store(false, std::memory_order_relaxed);
    process_.shutdown();

    pendingStop_ = false;
    pendingLocals_ = false;
    pendingWatchLabel_.clear();
}

// ---------------------------------------------------------------------------
// Debug commands (non-blocking)
// ---------------------------------------------------------------------------

int DebugSession::setBreakpoint(const std::string& file, int line)
{
    if (!running_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(errMtx_);
        lastError_ = "Debugger is not running. Launch a session first.";
        return -1;
    }

    if (file.empty() || line <= 0) {
        std::lock_guard<std::mutex> lock(errMtx_);
        lastError_ = "Breakpoint requires a file and a line number >= 1.";
        return -1;
    }

    const int bpId = nextBpId_++;

    std::string cmd;
    if (debuggerType_ == DebuggerType::Lldb)
        cmd = "breakpoint set --file \"" + file + "\" --line " + std::to_string(line);
    else
        cmd = "break \"" + file + "\":" + std::to_string(line);

    if (!sendCommand(cmd)) {
        std::lock_guard<std::mutex> lock(errMtx_);
        lastError_ = "Failed to send breakpoint command to the debugger.";
        --nextBpId_;
        return -1;
    }

    // Store optimistically; the debugger's confirmation (parsed in
    // pollResults) carries the authoritative number.
    {
        std::lock_guard<std::mutex> lock(bpMtx_);
        Breakpoint bp;
        bp.id = bpId;
        bp.file = file;
        bp.line = line;
        bp.enabled = true;
        breakpoints_[bpId] = std::move(bp);
        pendingBpConfirms_.push_back(bpId);
    }
    if (onBreakpoints)
        onBreakpoints(listBreakpoints());
    return bpId;
}

bool DebugSession::deleteBreakpoint(int id)
{
    if (!running_.load(std::memory_order_relaxed))
        return false;

    // Use the debugger-assigned number (may differ from our optimistic id).
    int debuggerNum = id;
    {
        std::lock_guard<std::mutex> lock(bpMtx_);
        const auto it = breakpoints_.find(id);
        if (it != breakpoints_.end())
            debuggerNum = it->second.id;
    }

    const std::string cmd = (debuggerType_ == DebuggerType::Lldb)
        ? "breakpoint delete " + std::to_string(debuggerNum)
        : "delete " + std::to_string(debuggerNum);

    if (!sendCommand(cmd))
        return false;

    {
        std::lock_guard<std::mutex> lock(bpMtx_);
        breakpoints_.erase(id);
    }
    if (onBreakpoints)
        onBreakpoints(listBreakpoints());
    return true;
}

std::vector<DebugSession::Breakpoint> DebugSession::listBreakpoints() const
{
    std::lock_guard<std::mutex> lock(bpMtx_);
    std::vector<Breakpoint> result;
    result.reserve(breakpoints_.size());
    for (const auto& [id, bp] : breakpoints_)
        result.push_back(bp);
    return result;
}

void DebugSession::continue_()
{
    if (!running_.load(std::memory_order_relaxed))
        return;

    // First continue starts the target (run); later ones resume from a stop.
    if (!hasStarted_)
    {
        hasStarted_ = true;
        sendCommand("run");
    }
    else
    {
        sendCommand("continue");
    }
}

void DebugSession::interrupt()
{
    if (!running_.load(std::memory_order_relaxed))
        return;

    if (debuggerType_ == DebuggerType::Lldb) {
        // Ask lldb to halt the inferior (equivalent to Ctrl-C in a terminal).
        sendCommand("process interrupt");
    } else {
#ifndef _WIN32
        // gdb: SIGINT to the gdb process is forwarded to the inferior.
        const int pid = process_.pid();
        if (pid > 0)
            ::kill(static_cast<pid_t>(pid), SIGINT);
#endif
    }
}

void DebugSession::stepOver()
{
    if (!running_.load(std::memory_order_relaxed))
        return;
    hasStarted_ = true;
    sendCommand("next");
}

void DebugSession::stepInto()
{
    if (!running_.load(std::memory_order_relaxed))
        return;
    hasStarted_ = true;
    sendCommand("step");
}

void DebugSession::stepOut()
{
    if (!running_.load(std::memory_order_relaxed))
        return;
    hasStarted_ = true;
    sendCommand("finish");
}

void DebugSession::requestCallStack()
{
    if (!running_.load(std::memory_order_relaxed))
        return;
    if (debuggerType_ == DebuggerType::Lldb)
        sendCommand("thread backtrace -c 32");
    else
        sendCommand("bt 32");
}

void DebugSession::requestLocals()
{
    if (!running_.load(std::memory_order_relaxed))
        return;
    pendingLocals_ = true;
    pendingLocalValues_.clear();
    quietPolls_ = 0;
    if (debuggerType_ == DebuggerType::Lldb) {
        pendingLocalsCmd_ = "frame variable";
        sendCommand("frame variable");
    } else {
        pendingLocalsCmd_ = "info locals";
        sendCommand("info locals");
    }
}

void DebugSession::evaluateWatch(const std::string& expression, const std::string& label)
{
    if (!running_.load(std::memory_order_relaxed))
        return;
    if (expression.empty())
        return;
    pendingWatchLabel_ = label.empty() ? expression : label;
    if (debuggerType_ == DebuggerType::Lldb)
        sendCommand("expression -- " + expression);
    else
        sendCommand("print " + expression);
}

// ---------------------------------------------------------------------------
// Async result polling
// ---------------------------------------------------------------------------

void DebugSession::pollResults()
{
    if (!running_.load(std::memory_order_relaxed))
        return;

    // Drain available output from the debugger's stdout.
    char buf[4096];
    std::size_t n = process_.drainOutput(buf, sizeof(buf));
    if (n == 0) {
        // No new output this poll.  lldb/gdb write no trailing prompt after
        // a result block when stdin is a pipe, so a short quiet period is
        // the only reliable end-of-results marker for pending collections
        // (locals requests, and gdb stop events without a terminating line).
        const bool pendingCollection =
            pendingStop_ || (pendingLocals_ && !pendingLocalValues_.empty());
        if (pendingCollection) {
            if (++quietPolls_ >= 3) {
                flushStopEvent();
                flushLocalsIfComplete();
            }
        } else {
            quietPolls_ = 0;
        }

        // Check if the debugger process exited.
        if (process_.state() == TerminalProcess::State::Done ||
            process_.state() == TerminalProcess::State::Idle) {
            running_.store(false, std::memory_order_relaxed);
            if (onExited) {
                auto cb = onExited;
                onExited = nullptr;   // fire once
                cb();
            }
        }
        return;
    }

    // New output arrived — reset the quiet-period counter.
    quietPolls_ = 0;

    // Extract complete lines, then process them outside the lock (parsing
    // may fire callbacks that re-enter this class).
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(outputMtx_);
        outputBuffer_.append(buf, n);
        std::size_t pos = 0;
        while ((pos = outputBuffer_.find('\n')) != std::string::npos) {
            lines.push_back(outputBuffer_.substr(0, pos));
            outputBuffer_.erase(0, pos + 1);
        }
    }

    for (const auto& line : lines)
        handleOutputLine(line);

    // A bare debugger prompt with no trailing newline (lldb/gdb write
    // "(lldb) " and wait) marks the end of a command-result block.  Treat
    // it as a flush point so pending stop/locals collections complete even
    // when the results were not followed by a blank line.
    bool promptSeen = false;
    {
        std::lock_guard<std::mutex> lock(outputMtx_);
        const std::string tail = debugTrim(outputBuffer_);
        if (tail == "(lldb)" || tail == "(gdb)") {
            outputBuffer_.clear();
            promptSeen = true;
        }
    }
    if (promptSeen) {
        flushStopEvent();
        flushLocalsIfComplete();
    }
}

// ---------------------------------------------------------------------------
// Internal: sending commands
// ---------------------------------------------------------------------------

bool DebugSession::sendCommand(const std::string& cmd)
{
    if (!running_.load(std::memory_order_relaxed))
        return false;

    // Remember what we sent so we can skip the debugger's echo of it.
    lastCommand_ = debugTrim(cmd);
    lastCommandEchoed_ = false;

    std::string line = cmd;
    if (line.empty() || line.back() != '\n')
        line += '\n';
    return process_.writeStdin(line.data(), line.size());
}

// ---------------------------------------------------------------------------
// Internal: output parsing
// ---------------------------------------------------------------------------

void DebugSession::handleOutputLine(const std::string& rawLine)
{
    std::string line = rawLine;

    // Strip debugger prompts.
    static constexpr const char* kLldbPrompt = "(lldb) ";
    static constexpr const char* kGdbPrompt  = "(gdb) ";
    if (line.rfind(kLldbPrompt, 0) == 0)
        line.erase(0, std::strlen(kLldbPrompt));
    else if (line.rfind(kGdbPrompt, 0) == 0)
        line.erase(0, std::strlen(kGdbPrompt));

    const bool lldb = (debuggerType_ == DebuggerType::Lldb);

    // A blank line (e.g. after a prompt) is a natural flush point for any
    // pending stop/locals collections.
    if (debugTrim(line).empty()) {
        flushStopEvent();
        flushLocalsIfComplete();
        return;
    }

    // Skip the debugger's echo of our own command (lldb/gdb echo the
    // command line back when stdin is not a tty).  This must not be treated
    // as a result block terminator, or pending collections would flush
    // before the real results arrive.
    if (!lastCommand_.empty() && line == lastCommand_) {
        // The echo of a NEW command (different from the one that started a
        // pending locals request) proves the previous request's results
        // block has ended — deliver them before consuming this echo.
        if (pendingLocals_ && !pendingLocalsCmd_.empty() &&
            lastCommand_ != pendingLocalsCmd_) {
            pendingLocals_ = false;
            std::vector<WatchValue> values = std::move(pendingLocalValues_);
            pendingLocalValues_.clear();
            if (onLocals)
                onLocals(std::move(values));
        }
        lastCommand_.clear();
        lastCommandEchoed_ = true;
        return;
    }

    if (onOutput)
        onOutput(line);

    // --- Breakpoint confirmation ---
    {
        int bpNum = 0;
        bool pending = false;
        if (parseBreakpointConfirm(line, bpNum, pending)) {
            {
                std::lock_guard<std::mutex> lock(bpMtx_);
                if (!pendingBpConfirms_.empty()) {
                    const int ourId = pendingBpConfirms_.front();
                    pendingBpConfirms_.pop_front();
                    auto it = breakpoints_.find(ourId);
                    if (it != breakpoints_.end())
                        it->second.id = bpNum;   // authoritative debugger number
                } else if (bpNum > 0) {
                    // A confirmation without a pending request — adopt the
                    // debugger's number into the optimistic entry with that id.
                    auto it = breakpoints_.find(bpNum);
                    if (it != breakpoints_.end())
                        it->second.id = bpNum;
                }
            }
            if (onBreakpoints)
                onBreakpoints(listBreakpoints());
            return;
        }
    }

    // --- Stop detection (lldb) ---
    if (lldb) {
        std::string reason;
        if (parseLldbStopLine(line, reason) ||
            (line.rfind("Process ", 0) == 0 && line.find(" stopped") != std::string::npos)) {
            pendingStop_ = true;
            pendingFrames_.clear();
            return;
        }
    } else {
        // --- Stop detection (gdb) ---
        std::string fn;
        if (parseGdbStopLine(line, fn) ||
            line.rfind("Program received signal", 0) == 0 ||
            line.rfind("Program exited", 0) == 0) {
            pendingStop_ = true;
            pendingFrames_.clear();
            return;
        }
    }

    // --- Call-stack frame collection ---
    if (pendingStop_) {
        DebugStackFrame frame;
        const bool ok = lldb ? parseLldbFrameLine(line, frame)
                             : parseGdbFrameLine(line, frame);
        if (ok) {
            pendingFrames_.push_back(frame);
            return;
        }
        // A non-frame line ends the backtrace block.
        flushStopEvent();
        // Fall through so the line can still be interpreted below.
    }

    // --- Locals collection ---
    if (pendingLocals_) {
        DebugWatchValue v;
        const bool ok = lldb ? parseLldbLocalsLine(line, v)
                             : parseGdbLocalsLine(line, v);
        if (ok) {
            pendingLocalValues_.push_back(v);
            return;
        }
        flushLocalsIfComplete();
    }

    // --- Watch/expression evaluation ---
    if (!pendingWatchLabel_.empty()) {
        DebugWatchValue v;
        const bool ok = lldb ? parseLldbWatchLine(line, v)
                             : parseGdbWatchLine(line, v);
        if (ok) {
            v.name = pendingWatchLabel_;
            pendingWatchLabel_.clear();
            if (onWatchValue)
                onWatchValue(v);
            return;
        }
        if (line.rfind("error:", 0) == 0) {
            WatchValue v;
            v.name = pendingWatchLabel_;
            v.type = "error";
            v.value = line;
            pendingWatchLabel_.clear();
            if (onWatchValue)
                onWatchValue(v);
            return;
        }
    }

    // --- Generic debugger error ---
    if (line.rfind("error:", 0) == 0) {
        if (onError)
            onError(line);
    }
}

void DebugSession::flushStopEvent()
{
    if (!pendingStop_)
        return;
    pendingStop_ = false;
    std::vector<StackFrame> frames = std::move(pendingFrames_);
    pendingFrames_.clear();
    if (!frames.empty() && onStopped)
        onStopped(std::move(frames));
}

void DebugSession::flushLocalsIfComplete()
{
    // Only deliver a locals result once values were actually collected.
    // A blank line or debugger prompt arriving between the command echo and
    // the results (e.g. gdb writes its "(gdb) " prompt as a blank line)
    // must not flush an empty locals list and drop the real results that
    // follow.  The echo-of-a-different-command path deliberately delivers
    // an empty list instead, to avoid leaving the request stuck.
    if (pendingLocalValues_.empty())
        return;
    if (!pendingLocals_)
        return;
    pendingLocals_ = false;
    lastCommandEchoed_ = false;
    std::vector<WatchValue> values = std::move(pendingLocalValues_);
    pendingLocalValues_.clear();
    if (onLocals)
        onLocals(std::move(values));
}

} // namespace hathor::ui
