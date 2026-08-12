// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * TerminalProcess.hpp — L-4: lightweight subprocess lifecycle manager for the
 * integrated terminal.
 *
 * Responsibilities:
 *   - Launch an ordinary command (argv vector) as a child process.
 *   - Capture stdout/stderr asynchronously on a dedicated worker thread
 *     (never the JUCE message thread, never the audio thread).
 *   - Stream captured bytes through a lock-free SPSC ring buffer so the
 *     JUCE message thread can poll and append to the terminal UI.
 *   - Track process lifecycle (Running → Exited).
 *   - Support graceful cancellation (SIGTERM) and hard termination (SIGKILL).
 *   - Report exit status (code or signal).
 *
 * Threading / ownership boundary:
 *   - JUCE message thread: calls launch(), cancel(), and pollOutput() (via timer).
 *     Never blocks on process I/O.
 *   - Worker thread (owned by this object): reads the child's stdout/stderr
 *     pipes and pushes bytes into the SPSC ring. Terminates when the pipes
 *     close or cancellation is requested.
 *   - Audio thread: never touches this class. No locks are ever taken on the
 *       audio callback; the SPSC ring is the only shared structure and it is
 *       lock-free with no allocation.
 *
 * Platform support:
 *   - POSIX (macOS, Linux): posix_spawn + pipes (following the AcpAgentSession
 *     pattern — safer and faster than fork on modern systems).
 *   - Windows: CreateProcess + anonymous pipes (stubbed for now; the project
 *     targets macOS/Linux primarily).
 *
 * Requirement references: L-4 §Architecture, L-4 Acceptance
 */

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Lock-free SPSC ring buffer for terminal output bytes.
//
// This is a simple, self-contained SPSC ring (not the audio SpscSampleRing,
// which uses seqlock sequence counters for real-time guarantees we don't need
// here). It is used to stream output from the worker thread into the JUCE
// message thread's polling timer.
//
// Capacity must be a power of two. When full, oldest bytes are dropped.
// ---------------------------------------------------------------------------

class TerminalRingBuffer
{
public:
    explicit TerminalRingBuffer(std::size_t capacity);
    ~TerminalRingBuffer() = default;

    TerminalRingBuffer(const TerminalRingBuffer&) = delete;
    TerminalRingBuffer& operator=(const TerminalRingBuffer&) = delete;

    /// Push bytes from the worker thread. Never blocks; drops oldest bytes if full.
    void push(const char* data, std::size_t len) noexcept;

    /// Drain up to `maxOut` bytes from the ring into `out` (message thread only).
    /// Returns the number of bytes copied.
    std::size_t drain(char* out, std::size_t maxOut) noexcept;

    /// True if no unread bytes are available.
    bool empty() const noexcept;

    /// Reset the buffer for reuse (discards all buffered data).
    void reset() noexcept;

private:
    std::vector<char>   buf_;
    std::size_t         capacity_;
    std::atomic<uint32_t> writeIdx_{0};
    std::atomic<uint32_t> readIdx_{0};
};

// ---------------------------------------------------------------------------
// TerminalProcess
// ---------------------------------------------------------------------------

class TerminalProcess
{
public:
    /// Process exit status.
    struct ExitStatus
    {
        bool   exitedNormally;   ///< true if the process exited (vs killed by signal / never started)
        int    exitCode;         ///< exit code if exitedNormally
        int    signal;           ///< signal number if killed by signal (-1 if unknown)
        bool   wasCancelled;     ///< true if cancellation was requested
        std::string errorMessage;///< human-readable error if launch failed
    };

    enum class State
    {
        Idle,       ///< no process running (or never launched)
        Running,    ///< process is alive and producing output
        Exiting,    ///< cancellation requested, waiting for process to die
        Done,       ///< process has exited; exit status available
    };

    TerminalProcess();
    ~TerminalProcess();

    TerminalProcess(const TerminalProcess&) = delete;
    TerminalProcess& operator=(const TerminalProcess&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle (called from the JUCE message thread)
    // -----------------------------------------------------------------------

    /**
     * Launch a command.
     *
     * @param argv      Argument vector: argv[0] = program name, argv[1..] = args.
     * @param cwd       Working directory for the child process (empty = inherit parent).
     * @param needStdin If true, create a stdin pipe for the child so the
     *                  parent can write commands via writeStdin().  Used by
     *                  the L-6 DebugSession to drive a native debugger CLI.
     * @return true on successful spawn; false and sets lastError() otherwise.
     */
    bool launch(const std::vector<std::string>& argv,
                const std::string& cwd = {},
                bool needStdin = false);

    /**
     * Request graceful cancellation of the running process.
     * Sends SIGTERM; if the process doesn't exit within killGraceMs, sends SIGKILL.
     * Returns immediately — the actual termination happens asynchronously.
     */
    void cancel(int killGraceMs = 2000);

    /**
     * Wait for the process to exit (blocking, with timeout).
     * Intended for use during shutdown or testing, NOT on the audio thread.
     * @param timeoutMs  Maximum milliseconds to wait; 0 = non-blocking check.
     * @return ExitStatus if the process has exited, or nullopt on timeout.
     */
    std::optional<ExitStatus> waitForExit(int timeoutMs = 0);

    /**
     * Force-cleanup: terminate the process (if still alive) and join the
     * worker thread. Safe to call from the JUCE message thread during
     * shutdown. Never touches the audio thread.
     */
    void shutdown();

    // -----------------------------------------------------------------------
    // Polling (called from the JUCE message thread via a timer)
    // -----------------------------------------------------------------------

    /**
     * Drain available output bytes into the provided buffer.
     * Thread-safe (SPSC). Returns the number of bytes copied.
     */
    std::size_t drainOutput(char* out, std::size_t maxOut) noexcept
    {
        return outputRing_.drain(out, maxOut);
    }

    /** Current process state. Thread-safe. */
    State state() const noexcept { return state_.load(std::memory_order_acquire); }

    /** Process ID (or -1 if not running). */
    int pid() const noexcept { return pid_.load(std::memory_order_acquire); }

    /** Last error message (e.g. from a failed launch). */
    std::string lastError() const;

    /**
     * Get the exit status. Only valid after state() == Done.
     * Returns a default-constructed ExitStatus if not yet available.
     */
    ExitStatus exitStatus() const;

    // -----------------------------------------------------------------------
    // Callback (invoked on the JUCE message thread via MessageManager)
    // -----------------------------------------------------------------------

    /**
     * Installed by the TerminalPanel. Called when the process exits, on the
     * JUCE message thread (marshalled from the worker thread).
     */
    std::function<void()> onProcessExited;

    // -----------------------------------------------------------------------
    // JUCE-free access for testing
    // -----------------------------------------------------------------------

    TerminalRingBuffer& outputRing() noexcept { return outputRing_; }

    // -----------------------------------------------------------------------
    // Write to the child's stdin (L-6: debugger command integration)
    // -----------------------------------------------------------------------
    // Sends data to the child process's stdin.  Non-blocking on the JUCE
    // message thread.  Returns true if the write succeeded (child stdin open),
    // false otherwise (child exited or stdin not available).
    bool writeStdin(const char* data, std::size_t len) noexcept;

private:
    // -----------------------------------------------------------------------
    // Worker thread entry point
    // -----------------------------------------------------------------------
    void workerLoop();

    /** Read from the child's stdout/stderr fd until EOF. Pushes to ring. */
    void readAvailableOutput();

    /** Poll waitpid to check if the child has exited. Non-blocking. */
    bool tryReapChild();

    /** Close all pipe file descriptors in the parent. */
    void closePipes();

    /** Resolve an executable name against $PATH. POSIX only. */
    static std::string resolvePath(const std::string& name);

    // -----------------------------------------------------------------------
    // Platform-specific spawn
    // -----------------------------------------------------------------------
#ifdef _WIN32
    bool spawnWindows(const std::vector<std::string>& argv, const std::string& cwd,
                      bool needStdin);
    void terminateWindows();
#else
    bool spawnPosix(const std::vector<std::string>& argv, const std::string& cwd,
                    bool needStdin);
    void terminatePosix(bool force);
#endif

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------

    TerminalRingBuffer        outputRing_;
    std::atomic<State>        state_{State::Idle};

    // Process handles — written during launch, read during worker/cancel.
    std::atomic<int>          pid_{-1};
    int                       stdoutRead_ = -1;   // parent reads child stdout
    int                       stderrRead_ = -1;   // parent reads child stderr (merged into stdout ring)
    int                       stdinWrite_ = -1;   // parent writes child stdin (unused but kept for future)

    // Exit status — written by worker when child exits, read by message thread.
    std::atomic<int>          exitCode_{-1};
    std::atomic<int>          exitSignal_{-1};
    std::atomic<bool>         wasCancelled_{false};
    std::string               lastError_;
    mutable std::mutex        errorMutex_;

    // Cancellation state
    std::atomic<bool>         cancelRequested_{false};

    // Worker thread
    std::thread               workerThread_;
    std::atomic<bool>         workerStop_{false};
};

} // namespace hathor::ui
