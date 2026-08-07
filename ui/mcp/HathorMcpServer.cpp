// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * HathorMcpServer.cpp -- standalone hathor-mcp executable.
 *
 * Speaks MCP JSON-RPC stdio to the agent (MCP server role) and forwards
 * tool calls over a Unix domain socket to the Hathor process.
 *
 * Deliberately links NO JUCE modules (Req 31.1, 31.2).
 *
 * Requirements: 32.7
 */

#include <nlohmann/json.hpp>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Unix socket helpers
// ---------------------------------------------------------------------------

/** Connect to the Hathor Unix domain socket at @p path.
 *  Returns the file descriptor on success, or -1 on failure (prints to stderr). */
static int connectUnixSocket(const char* path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::fprintf(stderr, "hathor-mcp: socket() failed: %s\n", std::strerror(errno));
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::fprintf(stderr, "hathor-mcp: connect(%s) failed: %s\n", path, std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

/** Send a newline-terminated command to the Unix socket.
 *  Returns false on write error. */
static bool sendCommand(int fd, const std::string& cmd)
{
    std::string line = cmd + "\n";
    const char* ptr = line.c_str();
    std::size_t remaining = line.size();
    while (remaining > 0)
    {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written <= 0)
            return false;
        ptr += static_cast<std::size_t>(written);
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

/** Read one newline-terminated line from @p fd with a @p timeoutMs millisecond timeout.
 *  Returns the line (without trailing newline) on success, or an empty string on
 *  timeout / error.  Sets @p timedOut to true if the timeout expired. */
static std::string readLine(int fd, int timeoutMs, bool& timedOut)
{
    timedOut = false;
    std::string result;

    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    while (true)
    {
        int ret = ::poll(&pfd, 1, timeoutMs);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            return {};
        }
        if (ret == 0)
        {
            timedOut = true;
            return {};
        }

        char ch = '\0';
        ssize_t n = ::read(fd, &ch, 1);
        if (n <= 0)
            return {}; // EOF or error
        if (ch == '\n')
            return result;
        result += ch;
    }
}

// ---------------------------------------------------------------------------
// MCP tools/list response
// ---------------------------------------------------------------------------

static json makeToolsList()
{
    json setPatternSchema;
    setPatternSchema["type"] = "object";
    setPatternSchema["properties"]["slot"]["type"] = "string";
    setPatternSchema["properties"]["slot"]["description"] = "Slot name, e.g. d1";
    setPatternSchema["properties"]["notation"]["type"] = "string";
    setPatternSchema["properties"]["notation"]["description"] = "Mini-notation pattern string";
    setPatternSchema["required"] = json::array({"slot", "notation"});

    json bpmSchema;
    bpmSchema["type"] = "object";
    bpmSchema["properties"]["value"]["type"] = "number";
    bpmSchema["properties"]["value"]["description"] = "BPM value (20-400)";
    bpmSchema["required"] = json::array({"value"});

    json noArgSchema;
    noArgSchema["type"] = "object";
    noArgSchema["properties"] = json::object();

    json gainSchema;
    gainSchema["type"] = "object";
    gainSchema["properties"]["value"]["type"] = "number";
    gainSchema["properties"]["value"]["description"] = "Gain value (0.0-2.0)";
    gainSchema["required"] = json::array({"value"});

    json tools = json::array();

    json setPattern;
    setPattern["name"] = "set_pattern";
    setPattern["description"] = "Set the mini-notation pattern for a named slot";
    setPattern["inputSchema"] = setPatternSchema;
    tools.push_back(setPattern);

    json bpm;
    bpm["name"] = "bpm";
    bpm["description"] = "Set the global BPM";
    bpm["inputSchema"] = bpmSchema;
    tools.push_back(bpm);

    json play;
    play["name"] = "play";
    play["description"] = "Start pattern playback";
    play["inputSchema"] = noArgSchema;
    tools.push_back(play);

    json stop;
    stop["name"] = "stop";
    stop["description"] = "Stop pattern playback";
    stop["inputSchema"] = noArgSchema;
    tools.push_back(stop);

    json setGain;
    setGain["name"] = "set_gain";
    setGain["description"] = "Set master output gain (0.0-2.0)";
    setGain["inputSchema"] = gainSchema;
    tools.push_back(setGain);

    json result;
    result["tools"] = tools;
    return result;
}

// ---------------------------------------------------------------------------
// Tool call -> Hathor command string
// ---------------------------------------------------------------------------

/** Build the plain-text Hathor command for a tools/call request.
 *  Returns an empty string if the tool name is unknown or arguments are missing. */
static std::string buildHathorCommand(const std::string& toolName, const json& args)
{
    if (toolName == "set_pattern")
    {
        if (!args.contains("slot") || !args.contains("notation"))
            return {};
        const std::string slot     = args["slot"].get<std::string>();
        const std::string notation = args["notation"].get<std::string>();
        return "set-pattern " + slot + " " + notation;
    }
    if (toolName == "bpm")
    {
        if (!args.contains("value"))
            return {};
        double v = args["value"].get<double>();
        char buf[64];
        if (v == static_cast<double>(static_cast<long>(v)))
            std::snprintf(buf, sizeof(buf), "bpm %ld", static_cast<long>(v));
        else
            std::snprintf(buf, sizeof(buf), "bpm %g", v);
        return buf;
    }
    if (toolName == "play")
        return "play";
    if (toolName == "stop")
        return "stop";
    if (toolName == "set_gain")
    {
        if (!args.contains("value"))
            return {};
        double v = args["value"].get<double>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "set-gain %g", v);
        return buf;
    }
    return {};
}

// ---------------------------------------------------------------------------
// MCP response helpers
// ---------------------------------------------------------------------------

static void sendJson(const json& obj)
{
    std::cout << obj.dump() << "\n";
}

static json makeErrorResponse(const json& id, int code, const std::string& message)
{
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", json{{"code", code}, {"message", message}}}};
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

int main()
{
    // Unbuffered stdout so the agent receives responses immediately.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Ignore SIGPIPE -- handle write errors explicitly.
    std::signal(SIGPIPE, SIG_IGN);

    // Determine socket path from environment.
    const char* socketPath = std::getenv("HATHOR_SOCKET_PATH");
    if (socketPath == nullptr || socketPath[0] == '\0')
    {
        std::fprintf(stderr,
                     "hathor-mcp: HATHOR_SOCKET_PATH environment variable is not set.\n");
        return 1;
    }

    // Connect to Hathor Unix socket.
    int sockFd = connectUnixSocket(socketPath);
    if (sockFd < 0)
        return 1;

    // MCP JSON-RPC stdio loop.
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
            continue;

        // Parse incoming JSON-RPC message.
        json req;
        try
        {
            req = json::parse(line);
        }
        catch (const json::parse_error& e)
        {
            sendJson(json{{"jsonrpc", "2.0"},
                          {"id", nullptr},
                          {"error",
                           json{{"code", -32700},
                                {"message", std::string("Parse error: ") + e.what()}}}});
            continue;
        }

        // Extract method -- skip if absent (malformed message).
        if (!req.contains("method") || !req["method"].is_string())
            continue;

        const std::string method = req["method"].get<std::string>();

        // Notifications have no "id" field -- no response needed.
        bool isNotification = !req.contains("id");

        // ----------------------------------------------------------------
        // initialize
        // ----------------------------------------------------------------
        if (method == "initialize")
        {
            if (isNotification)
                continue;
            sendJson(json{
                {"jsonrpc", "2.0"},
                {"id", req["id"]},
                {"result",
                 json{{"protocolVersion", "2024-11-05"},
                      {"capabilities", json{{"tools", json::object()}}},
                      {"serverInfo", json{{"name", "hathor-mcp"}, {"version", "1.0.0"}}}}}});
        }
        // ----------------------------------------------------------------
        // notifications/initialized  (no response)
        // ----------------------------------------------------------------
        else if (method == "notifications/initialized")
        {
            // Notification -- no response required.
        }
        // ----------------------------------------------------------------
        // tools/list
        // ----------------------------------------------------------------
        else if (method == "tools/list")
        {
            if (isNotification)
                continue;
            sendJson(json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", makeToolsList()}});
        }
        // ----------------------------------------------------------------
        // tools/call
        // ----------------------------------------------------------------
        else if (method == "tools/call")
        {
            if (isNotification)
                continue;

            const json& id = req["id"];

            // Validate params.
            if (!req.contains("params") || !req["params"].is_object()
                || !req["params"].contains("name"))
            {
                sendJson(makeErrorResponse(id, -32602, "Invalid params: missing tool name"));
                continue;
            }

            const std::string toolName = req["params"]["name"].get<std::string>();
            const json        args     = req["params"].contains("arguments")
                                             ? req["params"]["arguments"]
                                             : json::object();

            // Build the Hathor command string.
            std::string cmd = buildHathorCommand(toolName, args);
            if (cmd.empty())
            {
                sendJson(json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     json{{"content",
                           json::array({json{{"type", "text"},
                                            {"text",
                                             "unknown tool or invalid arguments: " + toolName}}})},
                          {"isError", true}}}});
                continue;
            }

            // Forward command to Hathor over the Unix socket.
            if (sockFd < 0 || !sendCommand(sockFd, cmd))
            {
                sendJson(json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result",
                               json{{"content",
                                     json::array({json{{"type", "text"},
                                                       {"text", "hathor socket write error"}}})},
                                    {"isError", true}}}});
                if (sockFd >= 0)
                {
                    ::close(sockFd);
                    sockFd = -1;
                }
                continue;
            }

            // Wait up to 5 seconds for the response.
            bool        timedOut     = false;
            std::string responseLine = readLine(sockFd, 5000, timedOut);

            if (timedOut)
            {
                sendJson(json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result",
                               json{{"content",
                                     json::array({json{{"type", "text"},
                                                       {"text", "hathor tool call timed out"}}})},
                                    {"isError", true}}}});
                continue;
            }

            if (responseLine.empty())
            {
                sendJson(json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result",
                               json{{"content",
                                     json::array({json{{"type", "text"},
                                                       {"text", "hathor socket read error"}}})},
                                    {"isError", true}}}});
                continue;
            }

            // Parse the Hathor response to determine success or failure.
            bool isError = false;
            try
            {
                json hathorResp = json::parse(responseLine);
                if (hathorResp.contains("ok") && hathorResp["ok"].is_boolean()
                    && !hathorResp["ok"].get<bool>())
                {
                    isError = true;
                }
            }
            catch (...)
            {
                // Non-JSON response -- treat as error.
                isError = true;
            }

            sendJson(json{
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result",
                 json{{"content",
                       json::array({json{{"type", "text"}, {"text", responseLine}}})},
                      {"isError", isError}}}});
        }
        // ----------------------------------------------------------------
        // Unknown method
        // ----------------------------------------------------------------
        else
        {
            if (!isNotification)
                sendJson(makeErrorResponse(req["id"], -32601, "Method not found: " + method));
        }
    }

    // stdin EOF -- agent disconnected.
    if (sockFd >= 0)
        ::close(sockFd);

    return 0;
}
