// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * AcpAgentPath.hpp — Resolve + validate an ACP agent command string.
 *
 * JUCE-free (POSIX + C++20 only) so the standalone acp_spike harness can
 * unit-test PATH resolution against real files on disk without the JUCE
 * message loop (issue A1).
 *
 * Behaviour:
 *  - The raw command is split on whitespace into argv tokens.
 *  - tokens[0] is the program. If it contains '/', it is treated as a path
 *    and validated directly. Otherwise it is resolved against $PATH.
 *  - Additional tokens are pass-through args (e.g. "--experimental-acp"
 *    for `gemini`), so bare names with args work end-to-end.
 *  - On success, outExe is the resolved absolute path and outArgv is the
 *    full argv (outExe first). On failure outError names what was searched.
 */

#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace hathor::ui {

inline bool isExecutableFile(const std::string& path)
{
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return false;
    if (!S_ISREG(st.st_mode))
        return false;
    return ::access(path.c_str(), X_OK) == 0;
}

inline bool resolveAgentCommand(const std::string& rawCmd,
                                std::string& outExe,
                                std::vector<std::string>& outArgv,
                                std::string& outError)
{
    if (rawCmd.empty())
    {
        outError = "No agent executable configured.";
        return false;
    }

    // Tokenise on whitespace (simple split; no quote handling — Agent 1.2
    // owns preset argv construction, this only supports ad-hoc commands).
    std::vector<std::string> tokens;
    {
        std::string cur;
        for (char c : rawCmd)
        {
            if (c == ' ' || c == '\t')
            {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            }
            else
            {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) tokens.push_back(cur);
    }

    if (tokens.empty())
    {
        outError = "Agent executable is empty.";
        return false;
    }

    const std::string& program = tokens[0];
    const bool isPath = program.find('/') != std::string::npos;

    std::string resolved;
    std::vector<std::string> searched;

    if (isPath)
    {
        resolved = program;
        if (!isExecutableFile(resolved))
        {
            outError = "Agent executable not found or not executable: " + resolved;
            return false;
        }
    }
    else
    {
        // Bare name: search $PATH (issue A1).
        const char* pathEnv = ::getenv("PATH");
        if (pathEnv == nullptr || pathEnv[0] == '\0')
        {
            outError = "No agent executable path provided and $PATH is unset; "
                       "cannot resolve bare agent name '" + program + "'.";
            return false;
        }

        std::vector<std::string> pathDirs;
        {
            std::string token;
            for (const char* p = pathEnv; ; ++p)
            {
                if (*p == '\0' || *p == ':')
                {
                    if (!token.empty()) pathDirs.push_back(token);
                    token.clear();
                    if (*p == '\0') break;
                }
                else
                {
                    token.push_back(*p);
                }
            }
        }

        bool found = false;
        for (const auto& dir : pathDirs)
        {
            std::string candidate = dir + "/" + program;
            searched.push_back(candidate);

            struct stat st{};
            if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode)
                && isExecutableFile(candidate))
            {
                resolved = candidate;
                found = true;
                break;
            }
        }

        if (!found)
        {
            std::string msg = "Agent '" + program + "' not found in $PATH.";
            if (!searched.empty())
            {
                msg += " Searched:";
                for (const auto& s : searched)
                    msg += " " + s;
            }
            outError = msg;
            return false;
        }
    }

    outExe    = resolved;
    outArgv   = tokens;
    outArgv[0] = resolved;   // argv[0] is the resolved path
    return true;
}

} // namespace hathor::ui
