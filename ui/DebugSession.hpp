// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * DebugSession.hpp — L-6: native/C++ debugging integration surface.
 *
 * Rather than implementing a custom C++ debugger engine (per PROGRAM.md
 * constraint #2: "native debugging → existing toolchain/debugger facilities"),
 * this class wraps the platform's native debugger CLI:
 *
 *   - macOS: LLDB via /usr/bin/lldb (command-line mode, --batch or -o pipes)
 *   - Linux: GDB via /usr/bin/gdb (with -i=mi or --batch)
 *   - Windows: not supported (explicitly) — returns a clear error
 *
 * The DebugSession launches the debugger as a child process (using the
 * existing TerminalProcess lifecycle — no new subprocess code) and communicates
 * via stdin/stdout pipes.  It parses the debugger's output to provide a
 * minimal, deterministic command surface:
 *
 *   - breakpoints (set / list / delete)
 *   - continue
 *   - pause (interrupt)
 *   - step over / step into / step out
 *   - call stack
 *   - locals / data inspection
 *   - watches (evaluate an expression)
 *
 * Threading boundary:
 *   - JUCE message thread: calls launch(), setBreakpoint(), continue_(),
 *     stepOver(), stepInto(), stepOut(), callStack(), locals(), evaluate().
 *     These methods are non-blocking — they send a command to the debugger's
 *     stdin and return immediately.  The result is collected async via
 *     pollResults() (called on a timer).
 *   - Audio thread: NEVER touches this class.
 *   - Worker thread (owned by TerminalProcess): reads debugger stdout.
 *
 * AI restriction (L-6 §AI RESTRICTION):
 *   - There is NO "AI Repair" button.  The debugger remains a deterministic
 *     IDE tool.  AI is available only through the existing Phase H–K
 *     contextual architecture.
 *
 * Requirement references: L-6 §Native/C++ Debugging
 */

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "TerminalProcess.hpp"

namespace hathor::ui {

/**
 * DebugSession — wraps a native debugger (LLDB/GDB) subprocess.
 *
 * JUCE-free: depends only on TerminalProcess (which is also JUCE-free).
 * Safe to call from any non-audio thread.  All command methods are
 * non-blocking.
 */
class DebugSession
{
public:
    /// The platform debugger available, or None if not supported.
    enum class DebuggerType {
        Lldb,   ///< macOS / Linux
        Gdb,    ///< Linux
        None,   ///< Windows or debugger not found
    };

    /// A single source-level frame in the call stack.
    struct StackFrame {
        int    line;       ///< 1-based line number
        int    column;     ///< 1-based column (0 if unknown)
        std::string function;  ///< function/method name
        std::string file;      ///< source file path
    };

    /// A single local variable or expression value at a breakpoint.
    struct WatchValue {
        std::string name;   ///< variable name or watch expression
        std::string type;   ///< type name (e.g. "int", "std::string")
        std::string value;  ///< current value (textual repr)
    };

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

    /// Continue execution from the current stop point.
    void continue_();

    /// Interrupt the running target (send SIGINT equivalent to the
    /// debugger's child process).  Non-blocking.
    void interrupt();

    /// Step over (next source line in the current function).
    void stepOver();

    /// Step into (enter the next function call).
    void stepInto();

    /// Step out (continue until the current function returns).
    void stepOut();

    /// Request the current call stack.  The result is delivered async
    /// via onStackFrames().
    void requestCallStack();

    /// Request the current local variables at the stop point.
    /// The result is delivered async via onLocals().
    void requestLocals();

    /// Evaluate a watch expression and get its current value.
    /// The result is delivered async via onWatchValue().
    /// @param expression  C++ expression to evaluate (e.g. "myVar.field").
    /// @param label       Optional label for the watch (for display).
    void evaluateWatch(const std::string& expression, const std::string& label);

    // -----------------------------------------------------------------------
    // Async results — called by the polling consumer
    // -----------------------------------------------------------------------

    /// Poll for async results from the debugger.  Call on a timer
    /// (e.g. 30 Hz) from the message thread.  Processes available output
    /// and fires the appropriate callback.
    void pollResults();

    // -----------------------------------------------------------------------
    // Callbacks — installed by the UI component
    // -----------------------------------------------------------------------

    /// Fired when the target stops (breakpoint hit, step complete, etc.).
    /// Provides the current call stack (top frame first).
    std::function<void(std::vector<StackFrame>)> onStopped;

    /// Fired when a watch/expression evaluation completes.
    std::function<void(WatchValue)> onWatchValue;

    /// Fired when the debugger process exits.
    std::function<void()> onExited;

private:
    /// Internal: send a command line to the debugger's stdin.
    bool sendCommand(const std::string& cmd);

    /// Parse LLDB output for a breakpoint-set confirmation.
    void parseBreakpointSet(const std::string& output);

    /// Parse LLDB/GDB output for a stop event + call stack.
    void parseStopEvent(const std::string& output);

    /// Parse LLDB/GDB output for a variable/watch evaluation.
    void parseEvaluate(const std::string& output, const std::string& label);

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    TerminalProcess process_;
    DebuggerType    debuggerType_ = DebuggerType::None;
    std::atomic<bool> running_{false};

    // Breakpoints (protected by a simple mutex — only accessed from the
    // message thread, never from the audio thread).
    mutable std::mutex bpMtx_;
    std::map<int, Breakpoint> breakpoints_;
    int nextBpId_ = 1;

    // Pending async state
    std::string lastError_;
    mutable std::mutex errMtx_;

    // Buffer for accumulating debugger output between polls.
    std::string outputBuffer_;
    std::mutex   outputMtx_;
};

} // namespace hathor::ui
