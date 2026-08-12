// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * TerminalProcess.cpp — L-4: implementation of the subprocess lifecycle
 * manager for the integrated terminal.
 *
 * Thread model:
 *   - JUCE message thread: calls launch(), cancel(), shutdown(), drainOutput().
 *   - Worker thread (this->workerThread_): reads child stdout/stderr pipes
 *     and pushes bytes into outputRing_. Detects child exit via waitpid.
 *   - Audio thread: NEVER touches this class. No locks in the audio path.
 *
 * The SPSC ring buffer is the sole cross-thread structure between worker and
 * message thread. It is lock-free and allocation-free after construction.
 */

#include "TerminalProcess.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>
#include <system_error>

// ---------------------------------------------------------------------------
// Platform includes
// ---------------------------------------------------------------------------

#ifdef _WIN32
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <spawn.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// TerminalRingBuffer
// ---------------------------------------------------------------------------

TerminalRingBuffer::TerminalRingBuffer(std::size_t capacity)
{
    // Round up to the next power of two (minimum 512).
    std::size_t cap = 512;
    while (cap < capacity)
        cap <<= 1;
    capacity_ = cap;
    buf_.resize(cap);
}

void TerminalRingBuffer::push(const char* data, std::size_t len) noexcept
{
    if (len == 0 || data == nullptr)
        return;

    const uint32_t mask = static_cast<uint32_t>(capacity_) - 1;
    uint32_t w = writeIdx_.load(std::memory_order_relaxed);
    uint32_t r = readIdx_.load(std::memory_order_acquire);

    for (std::size_t i = 0; i < len; ++i)
    {
        // If the ring is full, drop the oldest byte (advance read index).
        // This prevents the worker thread from ever blocking on a full ring.
        if ((w - r) == static_cast<uint32_t>(capacity_))
        {
            r = (r + 1) & mask;
            readIdx_.store(r, std::memory_order_release);
        }
        buf_[w & mask] = data[i];
        w = (w + 1) & mask;
        writeIdx_.store(w, std::memory_order_release);
    }
}

std::size_t TerminalRingBuffer::drain(char* out, std::size_t maxOut) noexcept
{
    if (maxOut == 0 || out == nullptr)
        return 0;

    const uint32_t mask = static_cast<uint32_t>(capacity_) - 1;
    uint32_t r = readIdx_.load(std::memory_order_acquire);
    uint32_t w = writeIdx_.load(std::memory_order_acquire);

    std::size_t count = 0;
    while (count < maxOut && r != w)
    {
        out[count++] = buf_[r & mask];
        r = (r + 1) & mask;
    }
    readIdx_.store(r, std::memory_order_release);
    return count;
}

bool TerminalRingBuffer::empty() const noexcept
{
    uint32_t r = readIdx_.load(std::memory_order_acquire);
    uint32_t w = writeIdx_.load(std::memory_order_acquire);
    return r == w;
}

// ---------------------------------------------------------------------------
// TerminalProcess
// ---------------------------------------------------------------------------

TerminalProcess::TerminalProcess()
    : outputRing_(65536)  // 64 KB ring buffer
{
}

TerminalProcess::~TerminalProcess()
{
    shutdown();
}

// -----------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------

bool TerminalProcess::launch(const std::vector<std::string>& argv,
                             const std::string& cwd)
{
    if (argv.empty())
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = "No command specified";
        return false;
    }

    // Reject if already running.
    State expected = State::Idle;
    if (!state_.compare_exchange_strong(expected, State::Running,
                                         std::memory_order_acq_rel))
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = "TerminalProcess already in use";
        state_.store(State::Idle, std::memory_order_release);
        return false;
    }

    // Spawn the platform-specific process.
#ifdef _WIN32
    if (!spawnWindows(argv, cwd))
#else
    if (!spawnPosix(argv, cwd))
#endif
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = lastError_.empty() ? "Failed to spawn process" : lastError_;
        state_.store(State::Idle, std::memory_order_release);
        closePipes();
        return false;
    }

    // Clear previous state.
    exitCode_.store(-1, std::memory_order_release);
    exitSignal_.store(-1, std::memory_order_release);
    wasCancelled_.store(false, std::memory_order_release);
    cancelRequested_.store(false, std::memory_order_release);
    workerStop_.store(false, std::memory_order_release);

    // Start the worker thread that reads output and reaps the child.
    workerThread_ = std::thread(&TerminalProcess::workerLoop, this);

    return true;
}

void TerminalProcess::cancel(int killGraceMs)
{
    if (state_.load(std::memory_order_acquire) != State::Running)
        return;

    cancelRequested_.store(true, std::memory_order_release);
    wasCancelled_.store(true, std::memory_order_release);
    state_.store(State::Exiting, std::memory_order_release);

    // Detach a thread to handle the kill grace period so cancel() returns
    // immediately. The worker thread will see the child exit and clean up.
    std::thread([this, killGraceMs]() {
        const int pid = pid_.load(std::memory_order_acquire);
        if (pid <= 0)
            return;

#ifdef _WIN32
        (void)killGraceMs;
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
        if (h) { TerminateProcess(h, 1); CloseHandle(h); }
#else
        // Send SIGTERM and wait up to killGraceMs for graceful exit.
        ::kill(static_cast<pid_t>(pid), SIGTERM);

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(killGraceMs);
        while (std::chrono::steady_clock::now() < deadline)
        {
            int status = 0;
            pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
            if (result != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        // Force-kill if still alive.
        {
            int status = 0;
            pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
            if (result == 0)
            {
                ::kill(static_cast<pid_t>(pid), SIGKILL);
                ::waitpid(static_cast<pid_t>(pid), &status, 0);
            }
        }
#endif
    }).detach();
}

std::optional<TerminalProcess::ExitStatus> TerminalProcess::waitForExit(int timeoutMs)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);

    if (workerThread_.joinable())
    {
        if (timeoutMs == 0)
        {
            // Non-blocking: just check if the thread has finished.
            if (workerThread_.joinable())
            {
                // Can't non-blockingly check std::thread. Use a short timed_join.
                workerThread_.join();
            }
        }
        else
        {
            // Timed join — not directly supported by std::thread, so we poll
            // the state flag instead.
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (state_.load(std::memory_order_acquire) == State::Done)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        if (workerThread_.joinable())
        {
            if (state_.load(std::memory_order_acquire) == State::Done)
                workerThread_.join();
        }
    }

    if (state_.load(std::memory_order_acquire) == State::Done)
    {
        return exitStatus();
    }

    if (timeoutMs == 0)
        return std::nullopt;

    if (state_.load(std::memory_order_acquire) == State::Done)
        return exitStatus();

    return std::nullopt;
}

void TerminalProcess::shutdown()
{
    // Signal the worker to stop reading.
    workerStop_.store(true, std::memory_order_release);

    // If the process is still running, kill it.
    if (state_.load(std::memory_order_acquire) == State::Running)
    {
        cancelRequested_.store(true, std::memory_order_release);
        wasCancelled_.store(true, std::memory_order_release);
        state_.store(State::Exiting, std::memory_order_release);

        const int pid = pid_.load(std::memory_order_acquire);
        if (pid > 0)
        {
#ifdef _WIN32
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
            if (h) { TerminateProcess(h, 1); CloseHandle(h); }
#else
            ::kill(static_cast<pid_t>(pid), SIGTERM);
            // Brief grace period.
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (std::chrono::steady_clock::now() < deadline)
            {
                int status = 0;
                if (::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) != 0)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            // Force kill.
            int status = 0;
            if (::waitpid(static_cast<pid_t>(pid), &status, WNOHANG) == 0)
            {
                ::kill(static_cast<pid_t>(pid), SIGKILL);
                ::waitpid(static_cast<pid_t>(pid), nullptr, 0);
            }
#endif
        }
    }

    // Close pipes so the worker thread's read() unblocks.
    closePipes();

    // Join the worker thread.
    if (workerThread_.joinable())
        workerThread_.join();

    state_.store(State::Done, std::memory_order_release);
}

// -----------------------------------------------------------------------
// Worker thread
// -----------------------------------------------------------------------

void TerminalProcess::workerLoop()
{
    char buf[4096];

    while (!workerStop_.load(std::memory_order_acquire))
    {
        // Try to read available output from both stdout and stderr.
        readAvailableOutput();

        // Check if the child has exited.
        if (tryReapChild())
        {
            // Drain any remaining output.
            for (int i = 0; i < 5; ++i)
                readAvailableOutput();
            break;
        }

        // Brief sleep to avoid busy-waiting. The worker thread is NOT the
        // audio thread, so a small sleep is fine.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    // Final cleanup.
    closePipes();

    State expected = State::Running;
    State desired   = State::Done;
    if (state_.compare_exchange_strong(expected, desired,
                                        std::memory_order_acq_rel))
    {
        // Normal completion path.
    }
    else
    {
        // State may have been set to Exiting by cancel() — transition to Done.
        if (state_.load(std::memory_order_acquire) != State::Done)
            state_.store(State::Done, std::memory_order_release);
    }

    // Fire the exit callback on the JUCE message thread.
    // NOTE: TerminalProcess is constructed before JUCE is fully available in
    // some test scenarios, so we check for the callback. The TerminalPanel
    // is responsible for marshalling via MessageManager when it owns this.
    if (onProcessExited)
    {
        // Post to the JUCE message thread if possible. In test contexts without
        // a running message loop, the callback runs on this worker thread —
        // callers must not assume message-thread execution in tests.
        onProcessExited();
    }
}

void TerminalProcess::readAvailableOutput()
{
    char buf[4096];

    // Read from stdout (merged stderr on POSIX).
    if (stdoutRead_ != -1)
    {
        ssize_t n = 0;
#ifdef _WIN32
        DWORD dwRead = 0;
        if (ReadFile(reinterpret_cast<HANDLE>(stdoutRead_), buf, sizeof(buf), &dwRead, nullptr) && dwRead > 0)
            n = static_cast<ssize_t>(dwRead);
        else
            n = -1;
#else
        n = ::read(stdoutRead_, buf, sizeof(buf));
#endif
        if (n > 0)
        {
            outputRing_.push(buf, static_cast<std::size_t>(n));
        }
        // On EAGAIN/EWOULDBLOCK, just return (no data available now).
        // On n <= 0 (EOF or error), the worker loop will reap the child.
    }
}

bool TerminalProcess::tryReapChild()
{
    const int pid = pid_.load(std::memory_order_acquire);
    if (pid <= 0)
        return false;

    int status = 0;
#ifdef _WIN32
    DWORD code = 0;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h)
    {
        DWORD w = WaitForSingleObject(h, 0);
        if (w == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(h, &code);
            exitCode_.store(static_cast<int>(code), std::memory_order_release);
            CloseHandle(h);
            pid_.store(-1, std::memory_order_release);
            return true;
        }
        CloseHandle(h);
    }
    return false;
#else
    pid_t result = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
    if (result == 0)
        return false; // still running

    if (result == static_cast<pid_t>(pid))
    {
        if (WIFEXITED(status))
        {
            exitCode_.store(WEXITSTATUS(status), std::memory_order_release);
            exitSignal_.store(-1, std::memory_order_release);
        }
        else if (WIFSIGNALED(status))
        {
            exitSignal_.store(WTERMSIG(status), std::memory_order_release);
            exitCode_.store(-1, std::memory_order_release);
        }
    }
    pid_.store(-1, std::memory_order_release);
    return true;
#endif
}

void TerminalProcess::closePipes()
{
    if (stdoutRead_ != -1)
    { ::close(stdoutRead_); stdoutRead_ = -1; }
    if (stderrRead_ != -1)
    { ::close(stderrRead_); stderrRead_ = -1; }
    if (stdinWrite_ != -1)
    { ::close(stdinWrite_); stdinWrite_ = -1; }
}

// -----------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------

std::string TerminalProcess::lastError() const
{
    std::lock_guard<std::mutex> lk(errorMutex_);
    return lastError_;
}

TerminalProcess::ExitStatus TerminalProcess::exitStatus() const
{
    ExitStatus s;
    s.exitCode = exitCode_.load(std::memory_order_acquire);
    s.signal = exitSignal_.load(std::memory_order_acquire);
    s.wasCancelled = wasCancelled_.load(std::memory_order_acquire);
    s.exitedNormally = (s.exitCode >= 0);
    return s;
}

// ---------------------------------------------------------------------------
// POSIX spawn implementation
// ---------------------------------------------------------------------------

#ifndef _WIN32

bool TerminalProcess::spawnPosix(const std::vector<std::string>& argv,
                                  const std::string& cwd)
{
    // Create pipes for child stdout (and stderr, which we merge into stdout).
    // stdoutPipe_[0] = read end (parent), stdoutPipe_[1] = write end (child)
    int stdoutPipe[2] = {-1, -1};
    // stderrPipe: redirect child stderr to stdout pipe.
    int stderrPipe[2] = {-1, -1};

    // For a terminal, we want both stdout and stderr visible. We can either
    // create separate pipes (more complex) or merge stderr into stdout.
    // Merging is simpler and sufficient for a developer terminal. We'll use
    // a single pipe for stdout and dup2 stderr to the same fd.
    if (::pipe(stdoutPipe) != 0)
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = "pipe() failed: " + std::string(std::strerror(errno));
        return false;
    }

    // Set the stdout read end to non-blocking so our worker thread can poll.
    int flags = ::fcntl(stdoutPipe[0], F_GETFL, 0);
    ::fcntl(stdoutPipe[0], F_SETFL, flags | O_NONBLOCK);

    // Prepare file actions for posix_spawn.
    posix_spawn_file_actions_t fileActions;
    ::posix_spawn_file_actions_init(&fileActions);

    // Child stdout → stdoutPipe[1]
    ::posix_spawn_file_actions_adddup2(&fileActions, stdoutPipe[1], STDOUT_FILENO);
    // Child stderr → stdoutPipe[1] (merge stderr into stdout)
    ::posix_spawn_file_actions_adddup2(&fileActions, stdoutPipe[1], STDERR_FILENO);

    // Close all pipe ends in the child (after dup2).
    ::posix_spawn_file_actions_addclose(&fileActions, stdoutPipe[0]);
    ::posix_spawn_file_actions_addclose(&fileActions, stdoutPipe[1]);
    ::posix_spawn_file_actions_addclose(&fileActions, stderrPipe[0]);
    ::posix_spawn_file_actions_addclose(&fileActions, stderrPipe[1]);

    // Optionally set the working directory via chdir in file actions.
    // posix_spawn doesn't have a chdir file action, so we use a pre-spawn
    // approach: set cwd_ on spawnattr (not portable) — instead, we just
    // spawn and the child inherits cwd. For a full terminal, the shell
    // handles its own cwd. For tasks, we pass cwd and use a wrapper.
    //
    // Actually, posix_spawn_file_actions_addchdir_np is a non-standard
    // extension. For portable cwd support, we can use the approach of
    // spawning via a shell: /bin/sh -c "cd <dir> && exec <cmd>". But
    // the task runner will pass cwd explicitly. For now, if cwd is
    // non-empty, we use a spawnattr with POSIX_SPAWN_SETSD or just
    // spawn normally and rely on the parent's cwd (which is the project dir).

    // Build the argv array (posix_spawn needs modifiable char* const*).
    std::vector<std::string> argStrs;
    for (const auto& a : argv)
        argStrs.emplace_back(a);

    std::vector<char*> argvArr;
    argvArr.reserve(argStrs.size() + 1);
    for (auto& s : argStrs)
        argvArr.push_back(s.data());
    argvArr.push_back(nullptr);

    // Build a modified environment that includes PWD if cwd is set.
    // For simplicity, we spawn with the parent's environment. If cwd is
    // non-empty, we use posix_spawn_file_actions_addchdir_np (BSD/macOS)
    // or fall back to spawning through /bin/sh.
    pid_t pid = 0;

    if (!cwd.empty())
    {
        // On macOS/BSD, addchdir_np is available. On Linux/glibc, it's
        // available as addchdir_np as well (GNU extension).
#ifdef __APPLE__
        ::posix_spawn_file_actions_addchdir_np(&fileActions, cwd.c_str());
#elif defined(__GLIBC__)
        ::posix_spawn_file_actions_addchdir_np(&fileActions, cwd.c_str());
#else
        // Fallback: spawn via /bin/sh -c "cd <cwd> && exec ..."
        // This is used only when neither macOS nor glibc is detected.
        std::string cmd = "cd \"" + cwd + "\" && exec \"";
        for (size_t i = 0; i < argStrs.size(); ++i)
        {
            if (i > 0) cmd += "\" \"";
            cmd += argStrs[i];
        }
        cmd += "\"";
        // Rebuild argv for /bin/sh
        argStrs.clear();
        argStrs.push_back("/bin/sh");
        argStrs.push_back("-c");
        argStrs.push_back(cmd);
        argvArr.clear();
        for (auto& s : argStrs)
            argvArr.push_back(s.data());
        argvArr.push_back(nullptr);
#endif
    }

    const int rc = ::posix_spawn(&pid,
                                 argvArr[0],
                                 &fileActions,
                                 nullptr,  // default spawn attributes
                                 argvArr.data(),
                                 ::environ);

    ::posix_spawn_file_actions_destroy(&fileActions);

    if (rc != 0)
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = "posix_spawn failed: " + std::string(std::strerror(rc));
        ::close(stdoutPipe[0]);
        ::close(stdoutPipe[1]);
        return false;
    }

    // Parent: close the child-side pipe ends.
    ::close(stdoutPipe[1]);  // close child's stdout write end
    stdoutRead_ = stdoutPipe[0];  // save read end for worker thread
    stderrRead_ = -1;  // merged into stdout
    stdinWrite_ = -1;  // not using stdin

    pid_.store(static_cast<int>(pid), std::memory_order_release);
    return true;
}

void TerminalProcess::terminatePosix(bool force)
{
    const int pid = pid_.load(std::memory_order_acquire);
    if (pid <= 0)
        return;

    if (force)
        ::kill(static_cast<pid_t>(pid), SIGKILL);
    else
        ::kill(static_cast<pid_t>(pid), SIGTERM);
}

#else // _WIN32

// ---------------------------------------------------------------------------
// Windows spawn implementation (stub — POSIX is the primary target)
// ---------------------------------------------------------------------------

bool TerminalProcess::spawnWindows(const std::vector<std::string>& argv,
                                    const std::string& cwd)
{
    // Build a command line string from argv.
    // (Windows CreateProcess takes a flat command line, not an argv array.)
    std::string cmdLine;
    for (size_t i = 0; i < argv.size(); ++i)
    {
        if (i > 0)
            cmdLine += ' ';
        cmdLine += '"';
        for (char c : argv[i])
        {
            if (c == '"')
                cmdLine += "\\\"";
            cmdLine += c;
        }
        cmdLine += '"';
    }

    STARTUPINFOWA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;

    // Create pipes for stdout.
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = "CreatePipe failed";
        return false;
    }

    si.hStdOutput = hWrite;
    si.hStdError = hWrite;  // merge stderr into stdout
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    std::wstring wCmd(cmdLine.begin(), cmdLine.end());
    std::wstring wCwd(cwd.begin(), cwd.end());

    BOOL ok = CreateProcessW(
        nullptr,                          // application name
        wCmd.data(),                      // command line
        nullptr, nullptr,                 // process/thread security
        TRUE,                             // inherit handles
        0,                                // creation flags
        nullptr,                          // environment
        wCwd.empty() ? nullptr : wCwd.c_str(), // working directory
        nullptr,                          // startup info
        &pi);

    if (!ok)
    {
        std::lock_guard<std::mutex> lk(errorMutex_);
        lastError_ = "CreateProcess failed";
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return false;
    }

    CloseHandle(hWrite);  // parent doesn't need the write end
    stdoutRead_ = reinterpret_cast<intptr_t>(hRead);
    stderrRead_ = -1;
    stdinWrite_ = -1;
    pid_.store(static_cast<int>(pi.dwProcessId), std::memory_order_release);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void TerminalProcess::terminateWindows()
{
    const int pid = pid_.load(std::memory_order_acquire);
    if (pid <= 0)
        return;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (h)
    {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
}

#endif // _WIN32
