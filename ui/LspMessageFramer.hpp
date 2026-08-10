// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * LspMessageFramer.hpp — JUCE-free Content-Length message framing.
 *
 * Implements the LSP/LSIF message framing protocol over stdio streams:
 *   - Messages are prefixed with a "Content-Length: <N>\r\n\r\n" header block.
 *   - The body is exactly N bytes of UTF-8 JSON.
 *
 * This class is JUCE-free and designed to be used with the hathor-ui-tests
 * target (no JUCE dependencies). The framer is byte-oriented and works
 * on raw string buffers, making it trivially testable.
 *
 * The companion LspJsonRpc class wraps this framer + JSON (de)serialization.
 *
 * Requirement references: AI-4
 */

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// LspMessageFramer — reads and writes LSP framed messages
// ---------------------------------------------------------------------------

/**
 * A decoded LSP message. The JSON body is returned as a std::string;
 * the caller is responsible for parsing it (typically via nlohmann::json).
 */
struct FramedMessage {
    std::string body;          ///< JSON body (exactly contentLength bytes)
    int         contentLength; ///< byte length of body
};

/**
 * LspMessageFramer
 *
 * Incremental Content-Length framer for LSP over std::io streams.
 *
 * Usage:
 *   - Write:  frameWrite(jsonBody) returns a complete framed message string.
 *   - Read:   feed(data); then tryNextMessage() returns FramedMessage when a
 *             complete message is available.
 *
 * Handles partial reads (messages split across multiple feed calls)
 * and multiple messages in a single feed (pipelining).
 */
class LspMessageFramer
{
public:
    LspMessageFramer() = default;
    ~LspMessageFramer() = default;

    LspMessageFramer(const LspMessageFramer&)            = delete;
    LspMessageFramer& operator=(const LspMessageFramer&) = delete;
    LspMessageFramer(LspMessageFramer&&) noexcept            = default;
    LspMessageFramer& operator=(LspMessageFramer&&) noexcept   = default;

    // -----------------------------------------------------------------------
    // Writing (server → client or client → server)
    // -----------------------------------------------------------------------

    /**
     * Frame a JSON body string into a complete LSP message with Content-Length header.
     * @param jsonBody  The JSON string to send.
     * @return A string ready to write to stdio: "Content-Length: N\r\n\r\n{body}".
     */
    static std::string frameWrite(std::string_view jsonBody)
    {
        auto len = static_cast<int>(jsonBody.size());
        std::string header = "Content-Length: " + std::to_string(len) + "\r\n\r\n";
        return header + std::string(jsonBody);
    }

    // -----------------------------------------------------------------------
    // Reading (incremental)
    // -----------------------------------------------------------------------

    /**
     * Feed raw bytes from the stream into the framer's buffer.
     */
    void feed(std::string_view data)
    {
        if (!data.empty())
            buffer_ += data;
    }

    /**
     * Attempt to extract the next complete message from the buffer.
     * @return FramedMessage if a complete message is available, std::nullopt
     *         if more data is needed.
     */
    std::optional<FramedMessage> tryNextMessage()
    {
        while (true)
        {
            if (state_ == ParseState::AwaitingHeader)
            {
                // Look for Content-Length header terminator
                auto headerEnd = buffer_.find("\r\n\r\n");
                if (headerEnd == std::string::npos)
                {
                    // Might have a bare \n without \r\n — also try \n\n
                    headerEnd = buffer_.find("\n\n");
                    if (headerEnd == std::string::npos)
                        return std::nullopt;
                    // Adjust for \n\n (2 bytes instead of 4)
                    // Check for Content-Length before this
                    auto header = buffer_.substr(0, headerEnd);
                    auto match = header.find("Content-Length");
                    if (match == std::string::npos)
                    {
                        // Skip this unknown header block
                        buffer_.erase(0, headerEnd + 2);
                        continue;
                    }
                    contentLength_ = parseContentLength(header);
                    if (contentLength_ <= 0)
                    {
                        buffer_.clear();
                        return std::nullopt;
                    }
                    state_ = ParseState::AwaitingBody;
                    buffer_.erase(0, headerEnd + 2);
                    continue;
                }

                auto header = buffer_.substr(0, headerEnd);
                contentLength_ = parseContentLength(header);
                if (contentLength_ <= 0)
                {
                    // Malformed header — skip to next potential header
                    buffer_.erase(0, headerEnd + 4);
                    continue;
                }
                state_ = ParseState::AwaitingBody;
                buffer_.erase(0, headerEnd + 4);
            }
            else // AwaitingBody
            {
                if (static_cast<int>(buffer_.size()) < contentLength_)
                    return std::nullopt; // Need more data

                FramedMessage msg;
                msg.body = buffer_.substr(0, contentLength_);
                msg.contentLength = contentLength_;
                buffer_.erase(0, contentLength_);
                state_ = ParseState::AwaitingHeader;
                contentLength_ = 0;
                return msg;
            }
        }
    }

    /** True if the framer has buffered data (incomplete header or body). */
    bool hasBufferedData() const noexcept { return !buffer_.empty(); }

    /** Number of bytes currently buffered (for diagnostics/debugging). */
    std::size_t bufferedBytes() const noexcept { return buffer_.size(); }

private:
    enum class ParseState {
        AwaitingHeader,
        AwaitingBody,
    };

    static int parseContentLength(std::string_view header)
    {
        auto pos = header.rfind("Content-Length");
        if (pos == std::string::npos)
            return -1;

        auto valueStart = header.find_first_of("0123456789", pos);
        if (valueStart == std::string::npos)
            return -1;

        auto valueEnd = header.find_first_not_of("0123456789", valueStart);
        auto numStr = header.substr(valueStart, valueEnd - valueStart);

        try
        {
            return std::stoi(std::string(numStr));
        }
        catch (...)
        {
            return -1;
        }
    }

    ParseState  state_         = ParseState::AwaitingHeader;
    std::string buffer_;
    int         contentLength_ = 0;
};

} // namespace hathor::lsp
