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
#include <sys/stat.h>
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
#include <vector>

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

    /// Called with a human-readable status during the init handshake
    /// ("Connecting to agent…", "Initializing…", "Creating session…").
    /// Allows the UI to show a visible "connecting" state (issue A6).
    using OnConnectingFn       = std::function<void(std::string status)>;

    /// Called when the init handshake completes successfully.
    using OnAgentReadyFn       = std::function<void()>;

    /// Called when a post-init prompt error response (JSON-RPC "error") is
    /// received that is not matched to a blocking init request. Allows
    /// ChatThread to surface the failure to the user (issue A5/A6).
    using OnPromptErrorFn      = std::function<void(std::string error)>;

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

    /// Called with status text during the init handshake (issue A6).
    void setOnConnecting(OnConnectingFn fn)                 { onConnecting_       = std::move(fn); }

    /// Called once when the session becomes ready after a successful handshake.
    void setOnAgentReady(OnAgentReadyFn fn)                 { onAgentReady_       = std::move(fn); }

    /// Called when a post-init prompt error response is received (issue A5).
    void setOnPromptError(OnPromptErrorFn fn)               { onPromptError_      = std::move(fn); }

    /**
     * Install the dispatcher used to handle MCP/control commands received on
     * the Unix socket (set-pattern, play, stop, bpm, set-gain).  The handler
     * is invoked on the socket accept-loop worker thread with a command line,
     * and MUST route it through hathor::control::ControlInterface::dispatchWithCallback
     * (or equivalent) so that the response sink delivers the JSON result back
     * over the socket.  Call before start().
     *
     * Requirements: Phase 2.5 H0
     */
    using McpCommandHandlerFn = std::function<void(
        std::string commandLine,
        std::function<void(std::string response)> respond)>;
    void setMcpCommandHandler(McpCommandHandlerFn fn) { mcpCommandHandler_ = std::move(fn); }

    // -----------------------------------------------------------------------
    // Session lifecycle — called from JUCE message thread
    // -----------------------------------------------------------------------

    /**
     * Start the agent session.
     *
     * Spawns the sender thread which:
     *   1. Resolves the agent command (PATH lookup / arg split — issue A1)
     *   2. Creates the Unix socket listener
     *   3. Spawns the agent subprocess (stderr captured to a temp file)
     *   4. Runs initialize + session/new (blocking on sender thread, timeout
     *      configurable, default 15 s — issue A6) with protocolVersion
     *      negotiation (issue A5)
     *   5. Starts the reader thread and enters the dequeue loop
     *
     * On any failure, onStartFailed() is marshalled to the JUCE message thread.
     *
     * @param agentExePath   Path or bare name of the agent executable
     *                       (resolved against $PATH if no '/' — issue A1).
     *                       May include command-line args (e.g.
     *                       "gemini --experimental-acp").
     * @param projectDir     Current project directory (cwd for session/new)
     * @param hathorMcpPath  Absolute path to the hathor-mcp executable
     * @param initTimeoutMs  Override for the init handshake timeout in ms
     *                       (0 = use the configured default, 15000 ms — issue A6).
     *
     * Requirements: 32.1, 32.2
     */
    void start(const std::string& agentExePath,
               const std::string& projectDir,
               const std::string& hathorMcpPath,
               int initTimeoutMs = 0);

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
     * Configure the init-handshake timeout (issue A6). Default 15000 ms.
     * A value <= 0 falls back to the default. Safe to call before start().
     */
    void setInitTimeoutMs(int ms) { initTimeoutMs_ = ms > 0 ? ms : kDefaultInitTimeoutMs; }

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

    /// Marshal a handshake status string to the JUCE message thread via
    /// onConnecting_ (issue A6). Safe to call from the sender thread.
    void notifyConnecting(std::string status);

    /// Marshal the agent-ready signal to the JUCE message thread (issue A6).
    void notifyReady();

    /// Main loop for the sender thread.
    void senderLoop(const std::string& rawAgentCmd,
                    const std::string& projectDir,
                    const std::string& hathorMcpPath);

    /// Main loop for the reader thread.
    void readerLoop();

    /**
     * Read up to `maxLines` trailing lines from the agent stderr temp file
     * captured during spawn. Returns an empty string if none was captured.
     * Called on the sender/reader thread; never on the audio thread.
     */
    std::string readStderrTail(int maxLines) const;

    /**
     * Reap the spawned agent subprocess (if still running) and return its
     * exit code. Called from the reader thread on abnormal exit; safe to call
     * from stop().
     *
     * @return exit status (WEXITSTATUS) or -1 if killed by signal / not run.
     */
    int reapExitedAgent();

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
     * Accept/read loop for the Unix listener created by createUnixSocketListener().
     * Runs on mcpServerThread_ (a worker thread, never the JUCE message thread or
     * audio thread).  Dispatches each command through mcpCommandHandler_.
     */
    void mcpServerLoop();

    /**
     * Spawn the agent subprocess using posix_spawn + pipes.
     * @param argv  Fully resolved argv (argv[0] is the executable path).
     *              stderr is redirected to a captured temp file (issue A7).
     * @return true on success; false on failure (lastError_ / onStartFailed reason).
     */
    bool spawnAgentProcess(const std::vector<std::string>& argv);

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
     *
     * Issue A5: the original implementation spawned a detached std::thread
     * capturing bare `this`, risking use-after-free if the session was
     * destroyed within the 30 s window. This version launches a session-owned
     * std::jthread stored in permissionTimers_; stop() requests stop on every
     * timer and joins, so no background thread can outlive the session.
     */
    void startPermissionTimer(int requestId);

    /**
     * Request stop + join on every owned permission timer thread (issue A5).
     * Called from stop() and the destructor path; safe to call repeatedly.
     */
    void stopPermissionTimers();

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

    /// Temp-file path capturing the agent's stderr for diagnostics on
    /// abnormal exit (issue A7).
    std::string stderrPath_;

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

    /// Init handshake timeout in ms. Default 15000 (issue A6). Zero means
    /// "use default" so callers that pass through can omit it.
    int initTimeoutMs_ = kDefaultInitTimeoutMs;

public:
    // -----------------------------------------------------------------------
    /// ACP protocol version this client negotiates (issue A5/A6).
    /// Currently 1 — sent in initialize; the agent's response is checked for
    /// a matching version instead of assuming success.
    // -----------------------------------------------------------------------
    static constexpr int kAcpProtocolVersion       = 1;
    static constexpr int kDefaultInitTimeoutMs     = 15000;
    static constexpr int kPermissionTimeoutSec     = 30;

    // -----------------------------------------------------------------------
    // MCP Unix-socket accept loop (Phase 2.5 H0)
    // -----------------------------------------------------------------------
    std::thread mcpServerThread_;   ///< accept/read loop worker thread
    McpCommandHandlerFn mcpCommandHandler_; ///< routes socket commands to ControlInterface

    // -----------------------------------------------------------------------
    // Threads
    // -----------------------------------------------------------------------
    std::thread senderThread_;
    std::thread readerThread_;

    /// Owned permission auto-cancel timers (issue A5). These are joined in
    /// stopPermissionTimers() (called from stop() and the destructor), so a
    /// timer can never fire against a destroyed session.
    std::mutex timerMutex_;
    std::vector<std::thread> permissionTimers_;

    // -----------------------------------------------------------------------
    // Callbacks (installed by caller before start())
    // -----------------------------------------------------------------------
    OnErrorFn             onError_;
    OnAgentMessageChunkFn onAgentMessageChunk_;
    OnToolCallUpdateFn    onToolCallUpdate_;
    OnPermissionRequestFn onPermissionRequest_;
    OnAgentDisconnectedFn onAgentDisconnected_;
    OnConnectingFn        onConnecting_;
    OnAgentReadyFn        onAgentReady_;
    OnPromptErrorFn       onPromptError_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AcpAgentSession)
};

} // namespace hathor::ui
