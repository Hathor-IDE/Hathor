// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * AcpLineReader.hpp — Growable, newline-delimited line reader for JSON-RPC 2.0.
 *
 * JUCE-free (POSIX + C++20 only) so it can be unit-tested by the standalone
 * acp_spike test harness without pulling in the JUCE message loop.
 *
 * Why this exists (issue A4):
 *   The original readerLoop() used a fixed 4096-byte `fgets` buffer and treated
 *   every returned chunk as a complete JSON-RPC line. A single line longer than
 *   4094 bytes gets split across multiple fgets() calls, each of which was
 *   parsed independently and silently dropped on JSON parse failure.
 *
 * This reader accumulates bytes across fgets() calls until a newline is found,
 * so arbitrarily long lines are reassembled intact before parsing.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Growable line assembler over a FILE* stream (issue A4).
//
// Reads one complete newline-terminated line into `out`, growing the internal
// staging buffer as needed. Returns true if a line was read (complete or
// partial-at-EOF). Returns false only on true EOF/error with no accumulated
// data. A line without a trailing newline (final partial line at EOF) is
// still returned with its content.
//
// Strip behaviour: a single trailing '\n' and any preceding '\r' are removed.
// ---------------------------------------------------------------------------
inline bool acpReadLine(FILE* fp, std::string& out)
{
    out.clear();

    if (fp == nullptr)
        return false;

    // Staging buffer. fgets() writes at most sizeof(buf)-1 chars + NUL.
    // The 4096 width here is only the read granularity, NOT a line limit:
    // accumulation below assembles complete lines of any length.
    char buf[4096];

    bool gotAny = false;

    while (true)
    {
        if (std::fgets(buf, static_cast<int>(sizeof(buf)), fp) == nullptr)
        {
            // EOF or read error. If we accumulated partial data (a line with
            // no trailing newline at EOF), hand it back so the caller can
            // attempt a final parse rather than silently dropping it.
            return gotAny;
        }

        gotAny = true;

        const std::size_t n = std::strlen(buf);
        const char* nl = static_cast<const char*>(std::memchr(buf, '\n', n));

        if (nl != nullptr)
        {
            // Complete line: append up to (not including) the newline.
            out.append(buf, static_cast<std::size_t>(nl - buf));
            // Strip a trailing '\r' (CRLF safety).
            if (!out.empty() && out.back() == '\r')
                out.pop_back();
            return true;
        }

        // No newline in this chunk — keep accumulating.
        out.append(buf, n);
    }
}

} // namespace hathor::ui
