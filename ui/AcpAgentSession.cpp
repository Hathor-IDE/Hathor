// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * AcpAgentSession.cpp — ACP v1 agent subprocess lifecycle manager.
 *
 * Implementation notes:
 *
 * Thread safety contract:
 *   - outgoingQueue_ / queueMutex_ / queueCv_   shared by: message thread (writers) + sender thread (reader)
 *   - pendingResponses_ / responseMutex_ / responseCv_  shared by: sender thread (writer during init) + reader thread (notifier)
 *   - answeredPermissions_ / permissionMutex_   shared by: message thread + permission-timer threads
 *   - stdinPipe_[1]   written exclusively by sender thread after spawn
 *   - stdoutPipe_[0]  read exclusively by reader thread after spawn
 *   - sessionId_      written once by sender thread during init, read by message thread after isReady_
 *   - isReady_        atomic bool
 *   - stopRequested_  atomic bool, set by stop() on message thread
 *
 * No mutex held by the audio thread is ever acquired here.
 * No JUCE Component methods are called from either ACP thread directly.
 * No SPSC ring buffer access from ACP threads.
 *
 * Requirements: 32.1–32.9, 30.1
 */

#include "AcpAgentSession.hpp"
#include "../control/SocketServer.hpp"

// POSIX
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>

// Declare environ in the global namespace so we can reference it as ::environ
// from within namespace hathor::ui below.
// On macOS, <unistd.h> declares environ but not in the global namespace when
// compiling C++ with namespaces.
extern char** environ; // NOLINT(readability-redundant-declaration)

// JUCE message thread marshaling
#include <juce_core/juce_core.h>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Static member definition
// ---------------------------------------------------------------------------

std::atomic<int> AcpAgentSession::socketSeq_{0};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AcpAgentSession::AcpAgentSession() = default;

AcpAgentSession::~AcpAgentSession()
{
    stop();
}

// ---------------------------------------------------------------------------
// State query
// ---------------------------------------------------------------------------

bool AcpAgentSession::isReady() const noexcept
{
    return isReady_.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

void AcpAgentSession::start(const std::string& agentExePath,
                            const std::string& projectDir,
                            const std::string& hathorMcpPath)
{
    // Ensure any previous session is cleaned up first.
    stop();

    stopRequested_.store(false, std::memory_order_release);
    isReady_.store(false, std::memory_order_release);

    // Launch the sender thread; it runs the blocking init sequence, then
    // starts the reader thread and enters the dequeue loop.
    senderThread_ = std::thread([this, agentExePath, projectDir, hathorMcpPath]
    {
        senderLoop(agentExePath, projectDir, hathorMcpPath);
    });
}

void AcpAgentSession::restart(const std::string& agentExePath,
                              const std::string& projectDir,
                              const std::string& hathorMcpPath)
{
    // Increment sequence counter so a fresh socket path is used (Req 32.8).
    socketSeq_.fetch_add(1, std::memory_order_relaxed);
    start(agentExePath, projectDir, hathorMcpPath);
}

void AcpAgentSession::stop()
{
    // Signal threads to stop.
    stopRequested_.store(true, std::memory_order_release);

    // Wake the sender thread if it's blocked on the condition variable.
    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        senderShouldStop_ = true;
        queueCv_.notify_all();
    }

    // Wake any sendRequestBlocking waiters.
    {
        std::lock_guard<std::mutex> lk(responseMutex_);
        responseCv_.notify_all();
    }

    // Kill subprocess gracefully (SIGTERM, then wait briefly).
    if (agentPid_ > 0)
    {
        ::kill(agentPid_, SIGTERM);

        // Wait up to 2 seconds, then SIGKILL.
        for (int i = 0; i < 20; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            int status = 0;
            pid_t result = ::waitpid(agentPid_, &status, WNOHANG);
            if (result != 0) break;
        }
        ::kill(agentPid_, SIGKILL);
        ::waitpid(agentPid_, nullptr, 0);
        agentPid_ = 0;
    }

    // Close pipe file descriptors so reader thread's read() unblocks.
    for (int& fd : stdinPipe_)
    {
        if (fd != -1) { ::close(fd); fd = -1; }
    }
    for (int& fd : stdoutPipe_)
    {
        if (fd != -1) { ::close(fd); fd = -1; }
    }

    // Join threads.
    if (senderThread_.joinable()) senderThread_.join();
    if (readerThread_.joinable()) readerThread_.join();

    // Close listener socket and remove socket file.
    if (listenerFd_ != -1)
    {
        ::close(listenerFd_);
        listenerFd_ = -1;
    }
    removeUnixSocket();

    // ----------------------------------------------------------------------
    // MCP accept loop teardown.  Closing the listener (above) unblocks the
    // accept-loop worker; joining here guarantees no background thread touches
    // AcpAgentSession/ControlInterface state after this returns (Req 32.8).
    // ----------------------------------------------------------------------
    if (mcpServerThread_.joinable())
        mcpServerThread_.join();

    // Reset state.
    isReady_.store(false, std::memory_order_release);
    sessionId_.clear();

    {
        std::lock_guard<std::mutex> lk(queueMutex_);
        outgoingQueue_.clear();
        senderShouldStop_ = false;
    }
    {
        std::lock_guard<std::mutex> lk(responseMutex_);
        pendingResponses_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(permissionMutex_);
        answeredPermissions_.clear();
    }
}

// ---------------------------------------------------------------------------
// Messaging — called from JUCE message thread
// ---------------------------------------------------------------------------

void AcpAgentSession::sendPrompt(const std::string& text)
{
    if (!isReady())
        return;

    const int id = nextId_.fetch_add(1, std::memory_order_relaxed);

    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"method",  "session/prompt"},
        {"params",  {
            {"sessionId", sessionId_},
            {"prompt",    nlohmann::json::array({{{"type","text"},{"text",text}}})}
        }}
    };

    enqueueRaw(std::move(req));
}

void AcpAgentSession::respondPermission(int requestId, std::string optionId)
{
    // Mark as answered to prevent the auto-cancel timer from firing.
    {
        std::lock_guard<std::mutex> lk(permissionMutex_);
        answeredPermissions_.insert(requestId);
    }

    nlohmann::json resp = {
        {"jsonrpc", "2.0"},
        {"id",      requestId},
        {"result",  {
            {"outcome",  optionId}
        }}
    };

    enqueueRaw(std::move(resp));
}

// ---------------------------------------------------------------------------
// Internal — enqueue and write
// ---------------------------------------------------------------------------

void AcpAgentSession::enqueueRaw(nlohmann::json msg)
{
    std::lock_guard<std::mutex> lk(queueMutex_);
    outgoingQueue_.push_back(std::move(msg));
    queueCv_.notify_one();
}

void AcpAgentSession::writeLineToAgent(const nlohmann::json& msg)
{
    if (stdinPipe_[1] == -1)
        return;

    const std::string line = msg.dump() + "\n";
    const char* ptr = line.c_str();
    std::size_t remaining = line.size();

    while (remaining > 0)
    {
        const ssize_t written = ::write(stdinPipe_[1], ptr, remaining);
        if (written <= 0) break;  // pipe closed or error
        ptr       += static_cast<std::size_t>(written);
        remaining -= static_cast<std::size_t>(written);
    }
}

// ---------------------------------------------------------------------------
// Internal — Unix socket
// ---------------------------------------------------------------------------

bool AcpAgentSession::createUnixSocketListener()
{
    // Build a unique socket path: $TMPDIR/hathor-<pid>-<seq>.sock
    const char* tmpdir = ::getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0')
        tmpdir = "/tmp";

    const int seq = socketSeq_.load(std::memory_order_relaxed);
    socketPath_ = std::string(tmpdir) + "/hathor-"
                + std::to_string(::getpid())
                + "-" + std::to_string(seq) + ".sock";

    listenerFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenerFd_ == -1)
        return false;

    // Remove stale socket file if any.
    ::unlink(socketPath_.c_str());

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    // sockaddr_un.sun_path is char[104] on macOS — safe for our path length.
    std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listenerFd_,
               reinterpret_cast<const struct sockaddr*>(&addr),
               sizeof(addr)) != 0)
    {
        ::close(listenerFd_);
        listenerFd_ = -1;
        return false;
    }

    if (::listen(listenerFd_, /*backlog=*/4) != 0)
    {
        ::close(listenerFd_);
        listenerFd_ = -1;
        return false;
    }

    return true;
}

void AcpAgentSession::removeUnixSocket()
{
    if (!socketPath_.empty())
    {
        ::unlink(socketPath_.c_str());
        socketPath_.clear();
    }
}

// ---------------------------------------------------------------------------
// Internal — MCP Unix-socket accept loop (Phase 2.5 H0)
// ---------------------------------------------------------------------------

void AcpAgentSession::mcpServerLoop()
{
    const int fd = listenerFd_;
    if (fd == -1 || !mcpCommandHandler_)
        return;

    // Snapshot the handler; the accept loop may outlive a handler reassignment
    // but never the session (stop() joins this thread first).
    McpCommandHandlerFn handler = mcpCommandHandler_;

    hathor::control::runSocketAcceptLoop(fd, stopRequested_, std::move(handler));
}

// ---------------------------------------------------------------------------
// Internal — subprocess spawn
// ---------------------------------------------------------------------------

bool AcpAgentSession::spawnAgentProcess(const std::string& agentExePath)
{
    // Create pipes for bidirectional stdio:
    //   stdinPipe_[0]  → read end (child stdin)
    //   stdinPipe_[1]  → write end (parent writes → child reads)
    //   stdoutPipe_[0] → read end (parent reads ← child writes)
    //   stdoutPipe_[1] → write end (child stdout)

    if (::pipe(stdinPipe_)  != 0) return false;
    if (::pipe(stdoutPipe_) != 0)
    {
        ::close(stdinPipe_[0]); ::close(stdinPipe_[1]);
        stdinPipe_[0] = stdinPipe_[1] = -1;
        return false;
    }

    // Set up posix_spawn file actions to connect the pipe ends to child
    // stdin (fd 0) and stdout (fd 1).
    posix_spawn_file_actions_t fileActions;
    posix_spawn_file_actions_init(&fileActions);

    // Child stdin  = stdinPipe_[0]  (read end)
    posix_spawn_file_actions_adddup2(&fileActions, stdinPipe_[0],  STDIN_FILENO);
    // Child stdout = stdoutPipe_[1] (write end)
    posix_spawn_file_actions_adddup2(&fileActions, stdoutPipe_[1], STDOUT_FILENO);

    // Close all pipe ends in child (they've been dup2'd already).
    posix_spawn_file_actions_addclose(&fileActions, stdinPipe_[0]);
    posix_spawn_file_actions_addclose(&fileActions, stdinPipe_[1]);
    posix_spawn_file_actions_addclose(&fileActions, stdoutPipe_[0]);
    posix_spawn_file_actions_addclose(&fileActions, stdoutPipe_[1]);

    // Build argv: ["<agent>", nullptr]
    // posix_spawn expects a non-const char* const* argv.
    std::string exeCopy = agentExePath;
    char* argv[] = { exeCopy.data(), nullptr };

    // Inherit parent's environment.
    // environ is the global POSIX environment array declared in <unistd.h>.
    // We use the global-scope ::environ explicitly since we're inside a namespace.

    const int rc = ::posix_spawn(&agentPid_,
                                 agentExePath.c_str(),
                                 &fileActions,
                                 nullptr,   // posix_spawnattr_t (default)
                                 argv,
                                 ::environ);

    posix_spawn_file_actions_destroy(&fileActions);

    if (rc != 0)
    {
        agentPid_ = 0;
        ::close(stdinPipe_[0]);  ::close(stdinPipe_[1]);
        ::close(stdoutPipe_[0]); ::close(stdoutPipe_[1]);
        stdinPipe_[0]  = stdinPipe_[1]  = -1;
        stdoutPipe_[0] = stdoutPipe_[1] = -1;
        return false;
    }

    // Close the child-side pipe ends in the parent process.
    ::close(stdinPipe_[0]);  stdinPipe_[0]  = -1;
    ::close(stdoutPipe_[1]); stdoutPipe_[1] = -1;

    return true;
}

// ---------------------------------------------------------------------------
// Internal — blocking request / response (init sequence only)
// ---------------------------------------------------------------------------

std::optional<nlohmann::json>
AcpAgentSession::sendRequestBlocking(int id,
                                     const std::string& method,
                                     nlohmann::json params,
                                     int timeoutMs)
{
    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id",      id},
        {"method",  method},
        {"params",  std::move(params)}
    };

    // Write the request directly (we are on the sender thread, not yet in the
    // dequeue loop, so we bypass the queue to avoid deadlock).
    writeLineToAgent(req);

    // Wait for the reader thread to deliver the matching response.
    std::unique_lock<std::mutex> lk(responseMutex_);

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeoutMs);

    responseCv_.wait_until(lk, deadline, [&]
    {
        return stopRequested_.load(std::memory_order_acquire)
            || pendingResponses_.count(id) > 0;
    });

    if (stopRequested_.load(std::memory_order_acquire))
        return std::nullopt;

    auto it = pendingResponses_.find(id);
    if (it == pendingResponses_.end())
        return std::nullopt;  // timeout

    nlohmann::json result = std::move(it->second);
    pendingResponses_.erase(it);
    return result;
}

// ---------------------------------------------------------------------------
// Internal — sender thread
// ---------------------------------------------------------------------------

void AcpAgentSession::senderLoop(const std::string& agentExePath,
                                 const std::string& projectDir,
                                 const std::string& hathorMcpPath)
{
    // ------------------------------------------------------------------
    // Step 1: Create Unix socket listener (BEFORE session/new)
    // ------------------------------------------------------------------
    if (!createUnixSocketListener())
    {
        onStartFailed("Failed to create Unix socket listener: "
                      + std::string(std::strerror(errno)));
        return;
    }

    // ----------------------------------------------------------------------
    // Phase 2.5 H0: start the MCP socket accept/read loop.  It runs on its own
    // worker thread (mcpServerThread_), never on the JUCE message thread nor
    // the audio thread, and forwards each command to ControlInterface via
    // mcpCommandHandler_.  It is torn down in stop().
    // ----------------------------------------------------------------------
    if (mcpCommandHandler_)
        mcpServerThread_ = std::thread([this] { mcpServerLoop(); });

    // ------------------------------------------------------------------
    // Step 2: Spawn agent subprocess
    // ------------------------------------------------------------------
    if (!spawnAgentProcess(agentExePath))
    {
        onStartFailed("Failed to spawn agent subprocess");
        return;
    }

    // ------------------------------------------------------------------
    // Step 3: Start reader thread NOW so it can deliver responses
    //         needed by the blocking sendRequestBlocking calls below.
    // ------------------------------------------------------------------
    readerThread_ = std::thread([this] { readerLoop(); });

    // ------------------------------------------------------------------
    // Step 4: initialize (blocking, 5 s timeout)
    // ------------------------------------------------------------------
    const int initId = nextId_.fetch_add(1, std::memory_order_relaxed);
    auto initResp = sendRequestBlocking(
        initId,
        "initialize",
        {
            {"protocolVersion", 1},
            {"clientInfo", {{"name", "hathor"}, {"version", "2.0.0"}}}
        },
        5000);

    if (!initResp)
    {
        onStartFailed("Agent did not respond to initialize");
        return;
    }

    // ------------------------------------------------------------------
    // Step 5: session/new (blocking, 5 s timeout)
    // ------------------------------------------------------------------
    const int newId = nextId_.fetch_add(1, std::memory_order_relaxed);
    auto newResp = sendRequestBlocking(
        newId,
        "session/new",
        {
            {"cwd", projectDir},
            {"mcpServers", nlohmann::json::array({{
                {"name",    "hathor"},
                {"command", hathorMcpPath},
                {"args",    nlohmann::json::array()},
                {"env",     nlohmann::json::array({{
                    {"name",  "HATHOR_SOCKET_PATH"},
                    {"value", socketPath_}
                }})}
            }})}
        },
        5000);

    if (!newResp)
    {
        onStartFailed("Agent did not respond to session/new");
        return;
    }

    // Extract sessionId from response.
    try
    {
        sessionId_ = (*newResp)["result"]["sessionId"].get<std::string>();
    }
    catch (const nlohmann::json::exception&)
    {
        onStartFailed("session/new response missing sessionId");
        return;
    }

    // Signal that the session is open for user interaction.
    isReady_.store(true, std::memory_order_release);

    // ------------------------------------------------------------------
    // Step 6: Enter the dequeue loop — write outgoing requests until stop.
    // ------------------------------------------------------------------
    while (true)
    {
        std::unique_lock<std::mutex> lk(queueMutex_);
        queueCv_.wait(lk, [this]
        {
            return senderShouldStop_ || !outgoingQueue_.empty();
        });

        if (senderShouldStop_)
            break;

        // Dequeue one request at a time.
        nlohmann::json msg = std::move(outgoingQueue_.front());
        outgoingQueue_.pop_front();
        lk.unlock();

        writeLineToAgent(msg);
    }
}

// ---------------------------------------------------------------------------
// Internal — reader thread
// ---------------------------------------------------------------------------

void AcpAgentSession::readerLoop()
{
    // Read stdout pipe line-by-line.
    // Use a FILE* wrapper over the fd for buffered line reading.
    if (stdoutPipe_[0] == -1)
        return;

    // Duplicate the fd so fclose doesn't interfere with our explicit close
    // on teardown. (We close stdoutPipe_[0] in stop() to unblock this thread.)
    FILE* fpRaw = ::fdopen(stdoutPipe_[0], "r");
    if (!fpRaw)
        return;

    // fdopen takes ownership of the fd — mark our copy as -1 so stop() won't
    // double-close it. We'll let fclose() handle it when the loop exits.
    stdoutPipe_[0] = -1;

    std::string line;
    line.reserve(4096);

    char buf[4096];

    while (!stopRequested_.load(std::memory_order_acquire))
    {
        if (::fgets(buf, static_cast<int>(sizeof(buf)), fpRaw) == nullptr)
        {
            // EOF or error — subprocess exited or pipe closed.
            break;
        }

        // Strip trailing newline.
        const std::size_t len = std::strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
            buf[len - 1] = '\0';

        // Skip empty lines.
        if (buf[0] == '\0')
            continue;

        // Parse as JSON-RPC 2.0.
        nlohmann::json parsed;
        try
        {
            parsed = nlohmann::json::parse(buf);
        }
        catch (const nlohmann::json::parse_error&)
        {
            // Malformed line — ignore and continue.
            continue;
        }

        // Dispatch based on message type:
        //   - Has "method" but no "id" → notification
        //   - Has "method" and "id"    → request (session/request_permission)
        //   - Has "result" or "error"  → response to a blocking init send

        const bool hasMethod = parsed.contains("method");
        const bool hasId     = parsed.contains("id");
        const bool hasResult = parsed.contains("result");
        const bool hasError  = parsed.contains("error");

        if (hasMethod && !hasId)
        {
            // ----------------------------------------------------------------
            // JSON-RPC Notification
            // ----------------------------------------------------------------
            handleNotification(parsed);
        }
        else if (hasMethod && hasId)
        {
            // ----------------------------------------------------------------
            // JSON-RPC Request (inbound, requires a response)
            // e.g. session/request_permission
            // ----------------------------------------------------------------
            handleIncomingRequest(parsed);
        }
        else if (hasResult || hasError)
        {
            // ----------------------------------------------------------------
            // JSON-RPC Response — deliver to sendRequestBlocking waiters
            // (used only during init sequence)
            // ----------------------------------------------------------------
            if (hasId)
            {
                const int id = parsed["id"].get<int>();
                std::lock_guard<std::mutex> lk(responseMutex_);
                pendingResponses_[id] = std::move(parsed);
                responseCv_.notify_all();
            }
        }
    }

    ::fclose(fpRaw);

    // If we exited the loop and stop wasn't requested, the subprocess exited
    // unexpectedly — marshal onAgentDisconnected to the JUCE message thread.
    if (!stopRequested_.load(std::memory_order_acquire))
    {
        OnAgentDisconnectedFn cb = onAgentDisconnected_;
        if (cb)
        {
            juce::MessageManager::callAsync([cb]() mutable
            {
                cb();
            });
        }
    }
}

// ---------------------------------------------------------------------------
// Internal — notification dispatch (called from reader thread)
// ---------------------------------------------------------------------------

void AcpAgentSession::handleNotification(const nlohmann::json& msg)
{
    // All session/update notifications.
    std::string method;
    try { method = msg["method"].get<std::string>(); }
    catch (...) { return; }

    if (method != "session/update")
        return;

    nlohmann::json params;
    try { params = msg["params"]; }
    catch (...) { return; }

    std::string sessionUpdate;
    try { sessionUpdate = params["update"]["sessionUpdate"].get<std::string>(); }
    catch (...) { return; }

    if (sessionUpdate == "agent_message_chunk")
    {
        // Extract content.text and marshal to message thread (Req 32.5).
        std::string text;
        try { text = params["update"]["content"]["text"].get<std::string>(); }
        catch (...) { return; }

        OnAgentMessageChunkFn cb = onAgentMessageChunk_;
        if (cb)
        {
            juce::MessageManager::callAsync([cb, text = std::move(text)]() mutable
            {
                cb(std::move(text));
            });
        }
    }
    else if (sessionUpdate == "tool_call" || sessionUpdate == "tool_call_update")
    {
        // Marshal the full update object to message thread (Req 32.5).
        nlohmann::json update;
        try { update = params["update"]; }
        catch (...) { return; }

        OnToolCallUpdateFn cb = onToolCallUpdate_;
        if (cb)
        {
            juce::MessageManager::callAsync([cb, update = std::move(update)]() mutable
            {
                cb(std::move(update));
            });
        }
    }
}

// ---------------------------------------------------------------------------
// Internal — incoming request dispatch (called from reader thread)
// ---------------------------------------------------------------------------

void AcpAgentSession::handleIncomingRequest(const nlohmann::json& msg)
{
    std::string method;
    try { method = msg["method"].get<std::string>(); }
    catch (...) { return; }

    if (method != "session/request_permission")
        return;

    int requestId = -1;
    try { requestId = msg["id"].get<int>(); }
    catch (...) { return; }

    nlohmann::json options;
    try { options = msg["params"]["options"]; }
    catch (...) { options = nlohmann::json::array(); }

    // Marshal the permission request to the JUCE message thread (Req 32.6).
    OnPermissionRequestFn cb = onPermissionRequest_;
    if (cb)
    {
        juce::MessageManager::callAsync([cb, requestId, options = std::move(options)]() mutable
        {
            cb(requestId, std::move(options));
        });
    }

    // Start the 30-second auto-cancel timer (Req 32.6).
    startPermissionTimer(requestId);
}

// ---------------------------------------------------------------------------
// Internal — 30-second permission auto-cancel timer (Req 32.6)
// ---------------------------------------------------------------------------

void AcpAgentSession::startPermissionTimer(int requestId)
{
    // Detached thread: sleeps 30 s, then checks if the permission was answered.
    // If not, enqueues a "cancelled" response.
    std::thread([this, requestId]
    {
        std::this_thread::sleep_for(std::chrono::seconds(30));

        if (stopRequested_.load(std::memory_order_acquire))
            return;

        bool alreadyAnswered = false;
        {
            std::lock_guard<std::mutex> lk(permissionMutex_);
            alreadyAnswered = answeredPermissions_.count(requestId) > 0;
        }

        if (!alreadyAnswered)
        {
            // Mark as answered to prevent respondPermission() from double-responding.
            {
                std::lock_guard<std::mutex> lk(permissionMutex_);
                answeredPermissions_.insert(requestId);
            }

            nlohmann::json resp = {
                {"jsonrpc", "2.0"},
                {"id",      requestId},
                {"result",  {{"outcome", "cancelled"}}}
            };

            enqueueRaw(std::move(resp));
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// Internal — onStartFailed (marshals to JUCE message thread)
// ---------------------------------------------------------------------------

void AcpAgentSession::onStartFailed(std::string reason)
{
    // Kill subprocess if it was spawned.
    if (agentPid_ > 0)
    {
        ::kill(agentPid_, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::kill(agentPid_, SIGKILL);
        ::waitpid(agentPid_, nullptr, 0);
        agentPid_ = 0;
    }

    // Close pipe fds.
    for (int& fd : stdinPipe_)
        if (fd != -1) { ::close(fd); fd = -1; }
    for (int& fd : stdoutPipe_)
        if (fd != -1) { ::close(fd); fd = -1; }

    // Marshal the error callback to the JUCE message thread (Req 32.1).
    OnErrorFn cb = onError_;
    if (cb)
    {
        juce::MessageManager::callAsync([cb, reason = std::move(reason)]() mutable
        {
            cb(std::move(reason));
        });
    }
}

} // namespace hathor::ui
