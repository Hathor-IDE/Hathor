// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * DebugSession.hpp — L-6: native/C++ debugging integration surface.
 *
 * Rather than implementing a custom C++ debugger engine (per PROGRAM.md
 * constraint #2: "native debugging → existing toolchain/debugger facilities"),
 * this class wraps the platform's native debugger CLI over stdio pipes:
 *
 *   - macOS: LLDB (preferred), GDB if lldb is unavailable
 *   - Linux: LLDB or GDB
 *   - Windows: not supported (explicitly) — launch() returns a clear error
 *
 * The debugger runs as a child process via the existing TerminalProcess
 * lifecycle (no new subprocess code) with a stdin pipe for commands and a
 * lock-free output ring.  Output is parsed by DebugOutputParser.hpp and
 * delivered through async callbacks polled from the JUCE message thread:
 *
 *   - breakpoints (set / list / delete)
 *   - continue / pause (interrupt) / step over / step into / step out
 *   - call stack, locals/data inspection, watches (expression evaluation)
 *
 * Threading boundary:
 *   - JUCE message thread: launch(), setBreakpoint(), continue_(), step*(),
 *     requestCallStack(), requestLocals(), evaluateWatch(), pollResults().
 *     Command methods are non-blocking — they write one line to the
 *     debugger's stdin and return.  Results are delivered asynchronously
 *     via pollResults() (called on a timer).
 *   - Audio thread: NEVER touches this class.
 *   - Worker thread (owned by TerminalProcess): reads debugger stdout.
 *
 * If a capability is not available on the platform (e.g. Windows), the
 * session reports it explicitly instead of pretending to work.
 *
 * AI restriction (L-6 §AI RESTRICTION):
 *   - There is NO "AI Repair" button.  The debugger remains a deterministic
 *     IDE tool.  AI is available only through the existing Phase H–K
 *     contextual architecture.
 *
 * Requirement references: L-6 §Native/C++ Debugging
 */

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "TerminalProcess.hpp"
#include "DebugOutputParser.hpp"

namespace hathor::ui {

/**
 * DebugSession — wraps a native debugger (LLDB/GDB) subprocess.
 *
 * JUCE-free: depends only on TerminalProcess + DebugOutputParser (both
 * JUCE-free).  Safe to call from any non-audio thread.  All command methods
 * are non-blocking.
 */
class DebugSession
{
public:
    /// The platform debugger available, or None if not supported.
    enum class DebuggerType {
        Lldb,   ///< macOS / Linux
        Gdb,    ///< Linux
        None,   ///< Windows or no debugger found
    };

    /// A single source-level frame in the call stack.
    using StackFrame = DebugStackFrame;

    /// A single local variable or expression value.
    using WatchValue = DebugWatchValue;

    /// A breakpoint entry.
    struct Breakpoint {
        int    id;             ///< debugger-assigned breakpoint number
        std::string file;      ///< source file path
        int    line;           ///< 1-based line number
        bool   enabled;        ///< true if the breakpoint is active
    };

    /// Session configuration — describes what to debug.
    struct Config {
        std::string executable;      ///< path to the binary to debug
        std::string sourceDir;       ///< working directory / source root
        std::vector<std::string> args; ///< command-line args for the target
    };

    DebugSession();
    ~DebugSession();

    DebugSession(const DebugSession&) = delete;
    DebugSession& operator=(const DebugSession&) = delete;

    // -----------------------------------------------------------------------
    // Platform detection
    // -----------------------------------------------------------------------

    /// Returns the debugger type supported on this platform, or None.
    static DebuggerType detectDebugger() noexcept;

    // -----------------------------------------------------------------------
    // Lifecycle (message thread)
    // -----------------------------------------------------------------------

    /// Launch the debugger with the given target.
    /// Returns an error string on failure (empty = success).
    /// Does NOT block on the debugger process — commands are sent async.
    std::string launch(const Config& config);

    /// Shut down the debugger session if running.
    /// Safe to call if not running.
    void shutdown();

    /// Returns true if a debugger process is currently attached.
    bool isRunning() const noexcept { return running_.load(std::memory_order_relaxed); }

    /// Returns the last error message (if any), or empty.
    std::string lastError() const;

    /// Returns the detected debugger type (for UI display).
    DebuggerType debuggerType() const noexcept { return debuggerType_; }

    // -----------------------------------------------------------------------
    // Debug commands (all non-blocking — message thread safe)
    // -----------------------------------------------------------------------

    /// Set a breakpoint at (file, line).  Returns a breakpoint ID > 0 on
    /// success, or -1 on failure (check lastError()).
    int setBreakpoint(const std::string& file, int line);

    /// Remove a breakpoint by ID.
    bool deleteBreakpoint(int id);

    /// List all breakpoints.
    std::vector<Breakpoint> listBreakpoints() const;

    /// Continue execution from the current stop point (starts the target
    /// on the first call).
    void continue_();

    /// Interrupt the running target.  Non-blocking: asks the debugger to
    /// halt the inferior (lldb `process interrupt` / SIGINT to gdb).
    void interrupt();

    /// Step over (next source line in the current function).
    void stepOver();

    /// Step into (enter the next function call).
    void stepInto();

    /// Step out (continue until the current function returns).
    void stepOut();

    /// Request the current call stack.  Result delivered async via onStopped
    /// (the stop callback also carries the stack when we stop).
    void requestCallStack();

    /// Request the current local variables at the stop point.
    /// Result delivered async via onLocals().
    void requestLocals();

    /// Evaluate a watch expression and get its current value.
    /// Result delivered async via onWatchValue().
    /// @param expression  C++ expression to evaluate (e.g. "myVar.field").
    /// @param label       Optional label for the watch (for display).
    void evaluateWatch(const std::string& expression, const std::string& label);

    // -----------------------------------------------------------------------
    // Async results — called by the polling consumer
    // -----------------------------------------------------------------------

    /// Poll for async results from the debugger.  Call on a timer
    /// (e.g. 30 Hz) from the message thread.  Processes available output
    /// and fires the appropriate callbacks.
    void pollResults();

    // -----------------------------------------------------------------------
    // Callbacks — installed by the UI component
    // -----------------------------------------------------------------------

    /// Fired when the target stops (breakpoint hit, step complete, etc.).
    /// Provides the current call stack (top frame first).
    std::function<void(std::vector<StackFrame>)> onStopped;

    /// Fired when local-variable inspection completes.
    std::function<void(std::vector<WatchValue>)> onLocals;

    /// Fired when a watch/expression evaluation completes.
    std::function<void(WatchValue)> onWatchValue;

    /// Fired when the breakpoint list changes (confirmation/removal).
    std::function<void(std::vector<Breakpoint>)> onBreakpoints;

    /// Fired for every line of raw debugger output (for the output view).
    std::function<void(std::string line)> onOutput;

    /// Fired on a debugger-side error (e.g. invalid command).
    std::function<void(std::string error)> onError;

    /// Fired when the debugger process exits.
    std::function<void()> onExited;

private:
    /// Internal: send a command line to the debugger's stdin.
    bool sendCommand(const std::string& cmd);

    /// Process a single complete output line.
    void handleOutputLine(const std::string& rawLine);

    /// Fire onStopped with the frames collected for the current stop.
    void flushStopEvent();

    /// Fire onLocals with the values collected for the pending request.
    void flushLocals();

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    TerminalProcess process_;
    DebuggerType    debuggerType_ = DebuggerType::None;
    std::atomic<bool> running_{false};
    bool            hasStarted_ = false;   ///< target has been run once

    // Breakpoints (message-thread only).
    mutable std::mutex bpMtx_;
    std::map<int, Breakpoint> breakpoints_;
    int nextBpId_ = 1;
    /// Optimistic ids awaiting the debugger's confirmation (FIFO).
    std::deque<int> pendingBpConfirms_;

    std::string lastError_;
    mutable std::mutex errMtx_;

    // Buffer for accumulating debugger output between polls.
    std::string outputBuffer_;
    std::mutex   outputMtx_;

    // --- Async parse state (message thread, set inside pollResults) ---
    bool pendingStop_ = false;
    std::vector<StackFrame> pendingFrames_;
    bool pendingLocals_ = false;
    std::vector<WatchValue> pendingLocalValues_;
    std::string pendingWatchLabel_;
};

} // namespace hathor::ui
