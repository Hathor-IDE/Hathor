// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * AcpAgentSession.hpp — ACP v1 agent subprocess lifecycle manager.
 *
 * Manages the lifecycle of an external ACP v1 agent subprocess:
 *   - Spawns the agent process using posix_spawn + pipes for bidirectional stdio
 *   - Creates a Unix domain socket at $TMPDIR/hathor-<pid>-<seq>.sock for hathor-mcp
 *   - Runs the initialization sequence (initialize → session/new) on the sender thread
 *   - Maintains a sender thread that dequeues and writes outgoing JSON-RPC requests
 *   - Maintains a reader thread that parses incoming JSON-RPC lines from agent stdout
 *   - Marshals all callbacks to the JUCE message thread via MessageManager::callAsync
 *
 * Thread model:
 *   - JUCE message thread: calls sendPrompt(), respondPermission(), stop(), restart()
 *   - Sender thread: dequeues requests from the outgoing queue and writes to agent stdin
 *   - Reader thread: reads agent stdout line-by-line, parses JSON-RPC, fires callbacks
 *   - Neither ACP thread touches the SPSC ring buffer or any JUCE Component directly
 *
 * Requirements: 32.1, 32.2, 32.3, 32.4, 32.5, 32.6, 32.8, 32.9, 30.1
 */

// POSIX / system headers
#include <spawn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

// C++ standard library
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

// Third-party
#include <nlohmann/json.hpp>

// JUCE — juce_events is needed for MessageManager::callAsync
// juce_gui_basics transitively includes juce_events; include it here so the
// header is self-contained when compiled as part of hathor-ui.
#include <juce_gui_basics/juce_gui_basics.h>

namespace hathor::ui {

/**
 * AcpAgentSession
 *
 * All public methods (except the constructor / destructor) are called from the
 * JUCE message thread.  Internal threads communicate back to the message thread
 * exclusively via juce::MessageManager::callAsync — never by calling JUCE
 * component methods directly.
 *
 * Callback interface (all callbacks are invoked on the JUCE message thread):
 *   onError(reason)            — start failure or unexpected subprocess exit info
 *   onAgentMessageChunk(text)  — chunk of agent text response
 *   onToolCallUpdate(update)   — tool_call / tool_call_update notification
 *   onPermissionRequest(id, options) — session/request_permission arrived
 *   onAgentDisconnected()      — subprocess stdout reached EOF unexpectedly
 *
 * Requirements: 32.1–32.9, 30.1
 */
class AcpAgentSession
{
public:
    // -----------------------------------------------------------------------
    // Callback types — all invoked on the JUCE message thread
    // -----------------------------------------------------------------------

    /// Called when the session fails to start or encounters a fatal error.
    using OnErrorFn             = std::function<void(std::string reason)>;

    /// Called for each "agent_message_chunk" session update.
    using OnAgentMessageChunkFn = std::function<void(std::string text)>;

    /// Called for "tool_call" / "tool_call_update" session updates.
    using OnToolCallUpdateFn    = std::function<void(nlohmann::json update)>;

    /// Called when a session/request_permission request arrives.
    /// @param requestId  The JSON-RPC request id (for responding)
    /// @param options    The permission options array from the request params
    using OnPermissionRequestFn = std::function<void(int requestId, nlohmann::json options)>;

    /// Called when the agent subprocess exits unexpectedly (reader EOF).
    using OnAgentDisconnectedFn = std::function<void()>;

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------

    AcpAgentSession();
    ~AcpAgentSession();

    // Non-movable (non-copyable is handled by JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR)
    AcpAgentSession(AcpAgentSession&&)                 = delete;
    AcpAgentSession& operator=(AcpAgentSession&&)      = delete;

    // -----------------------------------------------------------------------
    // Callback installation — call before start()
    // -----------------------------------------------------------------------

    void setOnError(OnErrorFn fn)                        { onError_             = std::move(fn); }
    void setOnAgentMessageChunk(OnAgentMessageChunkFn fn){ onAgentMessageChunk_ = std::move(fn); }
    void setOnToolCallUpdate(OnToolCallUpdateFn fn)      { onToolCallUpdate_    = std::move(fn); }
    void setOnPermissionRequest(OnPermissionRequestFn fn){ onPermissionRequest_ = std::move(fn); }
    void setOnAgentDisconnected(OnAgentDisconnectedFn fn){ onAgentDisconnected_ = std::move(fn); }

    // -----------------------------------------------------------------------
    // Session lifecycle — called from JUCE message thread
    // -----------------------------------------------------------------------

    /**
     * Start the agent session.
     *
     * Spawns the sender thread which:
     *   1. Creates the Unix socket listener
     *   2. Spawns the agent subprocess
     *   3. Runs initialize + session/new (blocking on sender thread, 5 s each)
     *   4. Starts the reader thread
     *
     * On any failure, onStartFailed() is marshalled to the JUCE message thread.
     *
     * @param agentExePath   Absolute path to the agent executable
     * @param projectDir     Current project directory (cwd for session/new)
     * @param hathorMcpPath  Absolute path to the hathor-mcp executable
     *
     * Requirements: 32.1, 32.2
     */
    void start(const std::string& agentExePath,
               const std::string& projectDir,
               const std::string& hathorMcpPath);

    /**
     * Re-run the start() sequence with a fresh socket path.
     * Safe to call after onAgentDisconnected or a previous start failure.
     *
     * Requirements: 32.8
     */
    void restart(const std::string& agentExePath,
                 const std::string& projectDir,
                 const std::string& hathorMcpPath);

    /**
     * Gracefully stop: kill subprocess, join threads, clean up socket.
     * Safe to call at any time, including before start().
     *
     * Requirements: 32.8
     */
    void stop();

    // -----------------------------------------------------------------------
    // Messaging — called from JUCE message thread
    // -----------------------------------------------------------------------

    /**
     * Enqueue a session/prompt request and return immediately (fire-and-forget).
     * MUST NOT block the JUCE message thread.
     *
     * Requirements: 32.3
     */
    void sendPrompt(const std::string& text);

    /**
     * Enqueue a JSON-RPC response to a session/request_permission request.
     * Called by ChatSidebar after the user picks an option.
     *
     * @param requestId  The id from the original request
     * @param optionId   The chosen option, or "cancelled"
     *
     * Requirements: 32.6
     */
    void respondPermission(int requestId, std::string optionId);

    // -----------------------------------------------------------------------
    // State query
    // -----------------------------------------------------------------------

    /// True once the session has been successfully initialised (session/new completed).
    bool isReady() const noexcept;

private:
    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /// Called on sender thread on any startup failure.
    /// Marshals to JUCE message thread, invokes onError_.
    void onStartFailed(std::string reason);

    /// Main loop for the sender thread.
    void senderLoop(const std::string& agentExePath,
                    const std::string& projectDir,
                    const std::string& hathorMcpPath);

    /// Main loop for the reader thread.
    void readerLoop();

    /**
     * Create the Unix domain socket listener.
     * @return true on success; false and sets lastError_ on failure.
     */
    bool createUnixSocketListener();

    /**
     * Remove the Unix socket file from the filesystem.
     */
    void removeUnixSocket();

    /**
     * Spawn the agent subprocess using posix_spawn + pipes.
     * @return true on success; false on failure.
     */
    bool spawnAgentProcess(const std::string& agentExePath);

    /**
     * Send a JSON-RPC request on the sender thread and block until a
     * matching response arrives or the timeout elapses.
     *
     * Only called during startup (on sender thread), never on message thread.
     *
     * @param id      JSON-RPC request id
     * @param method  Method name
     * @param params  Parameters object
     * @param timeoutMs  Timeout in milliseconds
     * @return Response JSON, or nullopt on timeout / EOF
     */
    std::optional<nlohmann::json> sendRequestBlocking(int id,
                                                      const std::string& method,
                                                      nlohmann::json params,
                                                      int timeoutMs = 5000);

    /**
     * Enqueue a raw JSON-RPC object for the sender thread to write.
     * Thread-safe: may be called from any thread.
     */
    void enqueueRaw(nlohmann::json msg);

    /**
     * Write a single JSON object as a newline-terminated line to agent stdin.
     * Called only from the sender thread.
     */
    void writeLineToAgent(const nlohmann::json& msg);

    /**
     * Start the 30-second auto-cancel timer for a permission request.
     * Creates a detached std::thread that sleeps and then, if the permission
     * hasn't been answered yet, enqueues a "cancelled" response.
     */
    void startPermissionTimer(int requestId);

    /**
     * Dispatch a JSON-RPC notification (no id) received from agent stdout.
     * Called from reader thread.
     */
    void handleNotification(const nlohmann::json& msg);

    /**
     * Dispatch an inbound JSON-RPC request (has id, requires response) from agent stdout.
     * Currently handles session/request_permission.
     * Called from reader thread.
     */
    void handleIncomingRequest(const nlohmann::json& msg);

    // -----------------------------------------------------------------------
    // Subprocess file descriptors
    //   stdinPipe_[1]  → write end → agent stdin
    //   stdoutPipe_[0] → read end  ← agent stdout
    // -----------------------------------------------------------------------
    int stdinPipe_[2]  = {-1, -1};
    int stdoutPipe_[2] = {-1, -1};

    /// PID of the spawned agent subprocess (0 if not running)
    pid_t agentPid_ = 0;

    // -----------------------------------------------------------------------
    // Unix socket
    // -----------------------------------------------------------------------
    int    listenerFd_  = -1;  ///< listening socket fd
    std::string socketPath_;   ///< path created at $TMPDIR/hathor-<pid>-<seq>.sock

    /// Monotonically increasing per-process sequence counter for socket paths.
    /// Incremented in restart() to guarantee fresh path on each session.
    static std::atomic<int> socketSeq_;

    // -----------------------------------------------------------------------
    // Outgoing request queue — shared between message thread (enqueue)
    // and sender thread (dequeue).
    // -----------------------------------------------------------------------
    std::mutex              queueMutex_;
    std::condition_variable queueCv_;
    std::deque<nlohmann::json> outgoingQueue_;

    /// Set to true to signal the sender thread to exit its dequeue loop.
    bool senderShouldStop_ = false;

    // -----------------------------------------------------------------------
    // Blocking response table — for init-sequence blocking sends only.
    // Maps expected response id → received response (or empty on timeout).
    // Protected by responseMutex_.
    // -----------------------------------------------------------------------
    std::mutex              responseMutex_;
    std::condition_variable responseCv_;
    std::unordered_map<int, nlohmann::json> pendingResponses_;

    // -----------------------------------------------------------------------
    // Reader → sender channel for incoming requests that need a response
    // (session/request_permission). Uses outgoingQueue_ for the response;
    // only needs to track which ids have been answered.
    // -----------------------------------------------------------------------
    std::mutex             permissionMutex_;
    std::set<int>          answeredPermissions_;  ///< ids already answered

    // -----------------------------------------------------------------------
    // Session state
    // -----------------------------------------------------------------------
    std::string sessionId_;
    std::atomic<bool> isReady_{false};
    std::atomic<bool> stopRequested_{false};

    /// Next JSON-RPC request id
    std::atomic<int> nextId_{1};

    // -----------------------------------------------------------------------
    // Threads
    // -----------------------------------------------------------------------
    std::thread senderThread_;
    std::thread readerThread_;

    // -----------------------------------------------------------------------
    // Callbacks (installed by caller before start())
    // -----------------------------------------------------------------------
    OnErrorFn             onError_;
    OnAgentMessageChunkFn onAgentMessageChunk_;
    OnToolCallUpdateFn    onToolCallUpdate_;
    OnPermissionRequestFn onPermissionRequest_;
    OnAgentDisconnectedFn onAgentDisconnected_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AcpAgentSession)
};

} // namespace hathor::ui
