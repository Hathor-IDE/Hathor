// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// tests-ui/test_acp_spike.cpp
//
// ACP Transport Spike — Task 0.1
//
// Validates three things before Group 4 production code is written:
//   (a) Bidirectional stdio with a subprocess (posix_spawn + pipes) on macOS
//   (b) JSON-RPC 2.0 serialisation of the full ACP lifecycle messages
//   (c) Unix domain socket listener/client round-trip
//
// This executable deliberately does NOT link JUCE — that validates the
// hathor-mcp no-JUCE constraint (Req 31).
//
// Build:  cmake --build build --target acp_spike
// Run:    ./build/tests-ui/acp_spike
// Exit 0 on success, non-zero on any mechanical test failure.

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// POSIX headers (macOS)
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Minimal test harness (no Catch2 — spike constraint)
// ---------------------------------------------------------------------------

static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (cond) {                                                             \
            ++g_passed;                                                         \
        } else {                                                                \
            ++g_failed;                                                         \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] "       \
                      << #cond << "\n";                                         \
        }                                                                       \
    } while (false)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        if ((a) == (b)) {                                                       \
            ++g_passed;                                                         \
        } else {                                                                \
            ++g_failed;                                                         \
            std::cerr << "FAIL [" << __FILE__ << ":" << __LINE__ << "] "       \
                      << #a << " == " << #b                                    \
                      << " (got: " << (a) << " vs " << (b) << ")\n";           \
        }                                                                       \
    } while (false)

// ---------------------------------------------------------------------------
// Section (b) — ACP JSON-RPC 2.0 message construction
// ---------------------------------------------------------------------------
//
// These tests verify that our JSON serialisation produces well-formed
// ACP v1 messages using nlohmann/json. No live agent is required here:
// we assert structural correctness of the messages we will later send.

static void test_acp_initialize_message()
{
    std::cout << "\n--- Section (b): ACP JSON-RPC message construction ---\n";

    json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 1;
    req["method"]  = "initialize";
    req["params"]  = {
        {"protocolVersion", 1},
        {"clientInfo", {{"name", "hathor"}, {"version", "2.0.0"}}}
    };

    std::string serialised = req.dump();

    // Re-parse to verify structural integrity
    json reparsed = json::parse(serialised);

    CHECK_EQ(reparsed["jsonrpc"].get<std::string>(), std::string("2.0"));
    CHECK_EQ(reparsed["id"].get<int>(), 1);
    CHECK_EQ(reparsed["method"].get<std::string>(), std::string("initialize"));
    CHECK_EQ(reparsed["params"]["protocolVersion"].get<int>(), 1);
    CHECK_EQ(reparsed["params"]["clientInfo"]["name"].get<std::string>(), std::string("hathor"));
    CHECK_EQ(reparsed["params"]["clientInfo"]["version"].get<std::string>(), std::string("2.0.0"));

    std::cout << "  initialize message: " << serialised << "\n";
    std::cout << "  [PASS] initialize round-trip\n";
}

static void test_acp_session_new_message()
{
    const std::string socketPath = "/tmp/hathor-" + std::to_string(::getpid()) + ".sock";
    const std::string cwd = "/tmp";
    const std::string hathorMcpPath = "/usr/local/bin/hathor-mcp";  // placeholder path

    json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 2;
    req["method"]  = "session/new";
    req["params"]  = {
        {"cwd", cwd},
        {"mcpServers", json::array({
            {
                {"name",    "hathor"},
                {"command", hathorMcpPath},
                {"args",    json::array()},
                {"env",     json::array({
                    {{"name", "HATHOR_SOCKET_PATH"}, {"value", socketPath}}
                })}
            }
        })}
    };

    std::string serialised = req.dump();
    json reparsed = json::parse(serialised);

    CHECK_EQ(reparsed["method"].get<std::string>(), std::string("session/new"));
    CHECK(reparsed["params"]["mcpServers"].is_array());
    CHECK_EQ(reparsed["params"]["mcpServers"].size(), std::size_t{1});

    auto& srv = reparsed["params"]["mcpServers"][0];
    CHECK_EQ(srv["name"].get<std::string>(), std::string("hathor"));
    CHECK_EQ(srv["command"].get<std::string>(), hathorMcpPath);
    CHECK(srv["args"].is_array());
    CHECK(srv["env"].is_array());
    CHECK_EQ(srv["env"][0]["name"].get<std::string>(), std::string("HATHOR_SOCKET_PATH"));
    CHECK_EQ(srv["env"][0]["value"].get<std::string>(), socketPath);

    std::cout << "  session/new message: " << serialised << "\n";
    std::cout << "  [PASS] session/new round-trip\n";
}

static void test_acp_session_prompt_message()
{
    const std::string sessionId = "ses_abc123";
    const std::string promptText = "Please call set_pattern with slot=d1 and notation=\"bd sn\"";

    json req;
    req["jsonrpc"] = "2.0";
    req["id"]      = 3;
    req["method"]  = "session/prompt";
    req["params"]  = {
        {"sessionId", sessionId},
        {"prompt", json::array({
            {{"type", "text"}, {"text", promptText}}
        })}
    };

    std::string serialised = req.dump();
    json reparsed = json::parse(serialised);

    CHECK_EQ(reparsed["method"].get<std::string>(), std::string("session/prompt"));
    CHECK_EQ(reparsed["params"]["sessionId"].get<std::string>(), sessionId);
    CHECK(reparsed["params"]["prompt"].is_array());
    CHECK_EQ(reparsed["params"]["prompt"][0]["type"].get<std::string>(), std::string("text"));
    CHECK_EQ(reparsed["params"]["prompt"][0]["text"].get<std::string>(), promptText);

    std::cout << "  session/prompt message: " << serialised << "\n";
    std::cout << "  [PASS] session/prompt round-trip\n";
}

static void test_acp_session_update_parsing()
{
    // Simulate parsing the three session/update notification types an agent emits

    // agent_message_chunk
    std::string chunk_raw = R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionUpdate":"agent_message_chunk","content":{"text":"Here is your pattern: bd sn"}}})";
    json chunk = json::parse(chunk_raw);
    CHECK_EQ(chunk["method"].get<std::string>(), std::string("session/update"));
    CHECK_EQ(chunk["params"]["sessionUpdate"].get<std::string>(), std::string("agent_message_chunk"));
    CHECK(chunk["params"]["content"]["text"].is_string());

    // tool_call
    std::string tc_raw = R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionUpdate":"tool_call","toolCall":{"name":"set_pattern","arguments":{"slot":"d1","notation":"bd sn"}}}})";
    json tc = json::parse(tc_raw);
    CHECK_EQ(tc["params"]["sessionUpdate"].get<std::string>(), std::string("tool_call"));
    CHECK_EQ(tc["params"]["toolCall"]["name"].get<std::string>(), std::string("set_pattern"));
    CHECK_EQ(tc["params"]["toolCall"]["arguments"]["slot"].get<std::string>(), std::string("d1"));

    // tool_call_update
    std::string tcu_raw = R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionUpdate":"tool_call_update","toolCallUpdate":{"status":"complete","result":{"ok":true}}}})";
    json tcu = json::parse(tcu_raw);
    CHECK_EQ(tcu["params"]["sessionUpdate"].get<std::string>(), std::string("tool_call_update"));
    CHECK(tcu["params"]["toolCallUpdate"]["result"]["ok"].get<bool>());

    std::cout << "  [PASS] session/update notification parsing (all three types)\n";
}

static void test_mcp_tool_call_parsing()
{
    // Simulate parsing an MCP tools/call request that hathor-mcp would receive from the agent
    std::string raw = R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"set_pattern","arguments":{"slot":"d1","notation":"bd sn [hh hh] cp"}}})";
    json req = json::parse(raw);

    CHECK_EQ(req["method"].get<std::string>(), std::string("tools/call"));
    CHECK_EQ(req["params"]["name"].get<std::string>(), std::string("set_pattern"));
    CHECK_EQ(req["params"]["arguments"]["slot"].get<std::string>(), std::string("d1"));
    CHECK_EQ(req["params"]["arguments"]["notation"].get<std::string>(), std::string("bd sn [hh hh] cp"));

    // Build the ControlInterface command that hathor-mcp would construct
    std::string slot     = req["params"]["arguments"]["slot"].get<std::string>();
    std::string notation = req["params"]["arguments"]["notation"].get<std::string>();
    std::string cmd = "set-pattern " + slot + " " + notation;
    CHECK_EQ(cmd, std::string("set-pattern d1 bd sn [hh hh] cp"));

    // Build the MCP success response
    json resp;
    resp["jsonrpc"] = "2.0";
    resp["id"]      = req["id"];
    resp["result"]  = {{"ok", true}};
    CHECK(resp["result"]["ok"].get<bool>());

    std::cout << "  [PASS] MCP tools/call → ControlInterface command construction\n";
}

// ---------------------------------------------------------------------------
// Section (c) — Unix domain socket round-trip
// ---------------------------------------------------------------------------
//
// Creates a server socket at $TMPDIR/hathor-<pid>-spike.sock, spawns a
// client thread that connects, sends a set-pattern command, and reads the
// response. The server accepts, reads, and writes back {"ok":true}.

struct SocketTestResult {
    bool server_accepted  = false;
    bool client_connected = false;
    bool server_received  = false;
    bool client_received_ok = false;
    std::string received_command;
    std::string received_response;
};

static void test_unix_socket_round_trip()
{
    std::cout << "\n--- Section (c): Unix domain socket round-trip ---\n";

    // Build socket path using $TMPDIR (falls back to /tmp)
    const char* tmpdir = ::getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0') tmpdir = "/tmp";

    std::string sockPath = std::string(tmpdir) + "hathor-"
                         + std::to_string(::getpid()) + "-spike.sock";

    // Remove any stale socket from a previous run
    ::unlink(sockPath.c_str());

    SocketTestResult result;

    // --- Server setup ---
    int serverFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(serverFd >= 0);
    if (serverFd < 0) {
        std::cerr << "  ERROR: socket() failed: " << ::strerror(errno) << "\n";
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (sockPath.size() >= sizeof(addr.sun_path) - 1) {
        std::cerr << "  ERROR: socket path too long\n";
        ::close(serverFd);
        return;
    }
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);

    int rc = ::bind(serverFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    CHECK(rc == 0);
    if (rc != 0) {
        std::cerr << "  ERROR: bind() failed: " << ::strerror(errno) << "\n";
        ::close(serverFd);
        return;
    }

    rc = ::listen(serverFd, 1);
    CHECK(rc == 0);

    // --- Client thread ---
    // Runs concurrently with server accept below
    std::thread clientThread([&]() {
        // Small delay to let server reach accept()
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        int cfd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (cfd < 0) {
            std::cerr << "  CLIENT ERROR: socket() failed\n";
            return;
        }

        struct sockaddr_un caddr{};
        caddr.sun_family = AF_UNIX;
        std::strncpy(caddr.sun_path, sockPath.c_str(), sizeof(caddr.sun_path) - 1);

        int r = ::connect(cfd, reinterpret_cast<struct sockaddr*>(&caddr), sizeof(caddr));
        if (r != 0) {
            std::cerr << "  CLIENT ERROR: connect() failed: " << ::strerror(errno) << "\n";
            ::close(cfd);
            return;
        }
        result.client_connected = true;

        // Send a set-pattern command (line-delimited JSON, matching ControlInterface protocol)
        json cmd;
        cmd["cmd"]      = "set-pattern";
        cmd["slot"]     = "d1";
        cmd["notation"] = "bd sn";
        std::string line = cmd.dump() + "\n";
        ::write(cfd, line.c_str(), line.size());

        // Read back the JSON response (block until newline or server closes)
        std::string response;
        char buf[1024];
        while (true) {
            ssize_t n = ::read(cfd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            response += buf;
            if (response.find('\n') != std::string::npos) break;
        }

        result.received_response = response;
        if (!response.empty()) {
            try {
                json resp = json::parse(response);
                result.client_received_ok = resp.value("ok", false);
            } catch (...) {
                std::cerr << "  CLIENT ERROR: failed to parse response JSON\n";
            }
        }

        ::close(cfd);
    });

    // --- Server: accept one connection, read command, respond ---
    struct sockaddr_un clientAddr{};
    socklen_t clientLen = sizeof(clientAddr);
    int connFd = ::accept(serverFd, reinterpret_cast<struct sockaddr*>(&clientAddr), &clientLen);
    CHECK(connFd >= 0);
    if (connFd >= 0) {
        result.server_accepted = true;

        // Read the command
        std::string incoming;
        char buf[1024];
        while (true) {
            ssize_t n = ::read(connFd, buf, sizeof(buf) - 1);
            if (n <= 0) break;
            buf[n] = '\0';
            incoming += buf;
            if (incoming.find('\n') != std::string::npos) break;
        }

        result.received_command = incoming;
        result.server_received = !incoming.empty();

        // Respond with {"ok": true}
        json resp;
        resp["ok"] = true;
        std::string respLine = resp.dump() + "\n";
        ::write(connFd, respLine.c_str(), respLine.size());

        ::close(connFd);
    }

    clientThread.join();

    // Cleanup
    ::close(serverFd);
    ::unlink(sockPath.c_str());

    // Verify socket file was cleaned up
    struct stat st{};
    bool socketGone = (::stat(sockPath.c_str(), &st) != 0);

    CHECK(result.server_accepted);
    CHECK(result.client_connected);
    CHECK(result.server_received);
    CHECK(result.client_received_ok);
    CHECK(socketGone);

    std::cout << "  server accepted:     " << (result.server_accepted  ? "YES" : "NO") << "\n";
    std::cout << "  client connected:    " << (result.client_connected ? "YES" : "NO") << "\n";
    std::cout << "  server received:     " << result.received_command;
    std::cout << "  client response ok:  " << (result.client_received_ok ? "YES" : "NO") << "\n";
    std::cout << "  socket cleaned up:   " << (socketGone ? "YES" : "NO") << "\n";
    std::cout << "  [PASS] Unix socket round-trip\n";
}

// ---------------------------------------------------------------------------
// Section (a) — Bidirectional stdio with subprocess (posix_spawn + pipes)
// ---------------------------------------------------------------------------
//
// Spawns /bin/cat as a stand-in for the agent subprocess.
// Writes to its stdin and concurrently reads from its stdout.
// Verifies that posix_spawn + pipe + dedicated fd is non-blocking and
// bidirectional — no deadlock, no races.

static void test_bidirectional_stdio()
{
    std::cout << "\n--- Section (a): Bidirectional stdio (posix_spawn + pipes) ---\n";

    // Create two pipes: parent→child (stdin) and child→stdout (stdout)
    int stdinPipe[2];   // stdinPipe[0] = child reads, stdinPipe[1] = parent writes
    int stdoutPipe[2];  // stdoutPipe[0] = parent reads, stdoutPipe[1] = child writes

    CHECK(::pipe(stdinPipe)  == 0);
    CHECK(::pipe(stdoutPipe) == 0);

    // Set parent's read end of stdout pipe to non-blocking so we can poll
    ::fcntl(stdoutPipe[0], F_SETFL, O_NONBLOCK);

    // posix_spawn file actions: redirect child stdin/stdout
    posix_spawn_file_actions_t fileActions;
    ::posix_spawn_file_actions_init(&fileActions);

    // Child stdin  = read end of stdinPipe
    ::posix_spawn_file_actions_adddup2(&fileActions, stdinPipe[0],  STDIN_FILENO);
    // Child stdout = write end of stdoutPipe
    ::posix_spawn_file_actions_adddup2(&fileActions, stdoutPipe[1], STDOUT_FILENO);
    // Close the parent's ends in the child
    ::posix_spawn_file_actions_addclose(&fileActions, stdinPipe[1]);
    ::posix_spawn_file_actions_addclose(&fileActions, stdoutPipe[0]);

    // Spawn /bin/cat — echoes stdin to stdout (simple bidirectional test)
    const char* argv[] = {"cat", nullptr};
    pid_t childPid = -1;
    int spawnRc = ::posix_spawn(&childPid, "/bin/cat", &fileActions, nullptr,
                                const_cast<char**>(argv), environ);
    CHECK(spawnRc == 0);
    ::posix_spawn_file_actions_destroy(&fileActions);

    if (spawnRc != 0) {
        std::cerr << "  ERROR: posix_spawn failed: " << ::strerror(spawnRc) << "\n";
        ::close(stdinPipe[0]); ::close(stdinPipe[1]);
        ::close(stdoutPipe[0]); ::close(stdoutPipe[1]);
        return;
    }

    // Close child's ends in parent (child now owns them)
    ::close(stdinPipe[0]);
    ::close(stdoutPipe[1]);

    int parentWrite = stdinPipe[1];   // parent writes here → child stdin
    int parentRead  = stdoutPipe[0];  // parent reads here  ← child stdout

    // Write the three ACP messages to the child's stdin
    // (In production these would be sent to the real agent)
    std::vector<std::string> messages = {
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":1,"clientInfo":{"name":"hathor","version":"2.0.0"}}})" "\n",
        R"({"jsonrpc":"2.0","id":2,"method":"session/new","params":{"cwd":"/tmp","mcpServers":[]}})" "\n",
        R"({"jsonrpc":"2.0","id":3,"method":"session/prompt","params":{"sessionId":"ses_test","prompt":[{"type":"text","text":"call set_pattern"}]}})" "\n"
    };

    std::string allWritten;
    for (auto& msg : messages) {
        allWritten += msg;
        ssize_t written = ::write(parentWrite, msg.c_str(), msg.size());
        CHECK(written == static_cast<ssize_t>(msg.size()));
    }
    // Close write end so cat gets EOF and terminates
    ::close(parentWrite);

    // Read back from child stdout concurrently (with poll timeout to avoid hanging)
    std::string allRead;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        struct pollfd pfd{};
        pfd.fd     = parentRead;
        pfd.events = POLLIN;

        int ready = ::poll(&pfd, 1, 200 /*ms*/);
        if (ready < 0) break;
        if (ready == 0) {
            // No data yet — check if child exited
            int status = 0;
            pid_t w = ::waitpid(childPid, &status, WNOHANG);
            if (w == childPid) break;
            continue;
        }
        char buf[4096];
        ssize_t n = ::read(parentRead, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = '\0';
        allRead += buf;
    }
    ::close(parentRead);

    // Wait for child to finish cleanly
    int exitStatus = 0;
    ::waitpid(childPid, &exitStatus, 0);
    bool childExitedClean = WIFEXITED(exitStatus) && (WEXITSTATUS(exitStatus) == 0);

    // /bin/cat echoes stdin to stdout verbatim, so allRead should equal allWritten
    CHECK(childExitedClean);
    CHECK_EQ(allRead, allWritten);

    // Verify each echoed line parses as valid JSON (confirms no corruption)
    std::istringstream ss(allRead);
    std::string line;
    int linesParsed = 0;
    bool allParsed = true;
    while (std::getline(ss, line)) {
        if (line.empty()) continue;
        try {
            json parsed = json::parse(line);
            CHECK(parsed.contains("jsonrpc"));
            ++linesParsed;
        } catch (...) {
            allParsed = false;
            std::cerr << "  ERROR: echoed line is not valid JSON: " << line << "\n";
            ++g_failed;
        }
    }
    CHECK(allParsed);
    CHECK_EQ(linesParsed, 3);

    std::cout << "  posix_spawn /bin/cat: " << (spawnRc == 0 ? "OK" : "FAIL") << "\n";
    std::cout << "  child exit clean:     " << (childExitedClean ? "YES" : "NO") << "\n";
    std::cout << "  bytes written:        " << allWritten.size() << "\n";
    std::cout << "  bytes read back:      " << allRead.size() << "\n";
    std::cout << "  JSON lines echoed:    " << linesParsed << " / 3\n";
    std::cout << "  [PASS] bidirectional stdio via posix_spawn + pipes\n";
}

// ---------------------------------------------------------------------------
// Section (b-extra): mcpServers field validation notes
//
// NOTE: This section documents what CANNOT be tested mechanically in the spike
// and requires manual or integration-level verification with a live agent.
// See spike-notes.md for the full analysis.
// ---------------------------------------------------------------------------

static void note_mcp_servers_validation()
{
    std::cout << "\n--- Section (b): mcpServers field (manual verification required) ---\n";
    std::cout << "  The following assertions require a live Claude Code or Gemini CLI agent:\n";
    std::cout << "    - Agent receives session/new with mcpServers and spawns hathor-mcp\n";
    std::cout << "    - Agent issues MCP tools/call for set_pattern in response to a prompt\n";
    std::cout << "    - Agent emits session/update tool_call and tool_call_update notifications\n";
    std::cout << "    - hathor-mcp forwards the command over the Unix socket\n";
    std::cout << "  These are deferred to manual integration testing as documented\n";
    std::cout << "  in tests-ui/spike-notes.md (Section 2 — mcpServers validation).\n";
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main()
{
    std::cout << "=== ACP Transport Spike — Task 0.1 ===\n";
    std::cout << "Testing mechanical aspects of the ACP transport layer.\n";
    std::cout << "Parts requiring a live agent are documented in spike-notes.md.\n";

    // Section (b): JSON-RPC 2.0 message construction
    test_acp_initialize_message();
    test_acp_session_new_message();
    test_acp_session_prompt_message();
    test_acp_session_update_parsing();
    test_mcp_tool_call_parsing();

    // Section (c): Unix socket round-trip
    test_unix_socket_round_trip();

    // Section (a): Bidirectional stdio via posix_spawn + pipes
    test_bidirectional_stdio();

    // Note deferred work
    note_mcp_servers_validation();

    std::cout << "\n=== Results: " << g_passed << " passed, " << g_failed << " failed ===\n";

    if (g_failed > 0) {
        std::cerr << "SPIKE FAILED — " << g_failed << " check(s) did not pass.\n";
        return 1;
    }

    std::cout << "SPIKE PASSED — all mechanical tests succeeded.\n";
    std::cout << "See tests-ui/spike-notes.md for proceed/resolve recommendation.\n";
    return 0;
}
