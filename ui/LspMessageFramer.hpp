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

#include <algorithm>
#include <cctype>
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
     * Spec-strict: headers terminate with "\r\n\r\n" only (a bare "\n\n"
     * inside a JSON body must never split a message).
     * @return FramedMessage if a complete message is available, std::nullopt
     *         if more data is needed.
     */
    std::optional<FramedMessage> tryNextMessage()
    {
        // Defensive cap: a peer claiming absurd lengths can't OOM us, and
        // garbage without any terminator can't grow the buffer forever.
        if (buffer_.size() > kMaxBufferedBytes)
        {
            buffer_.clear();
            state_ = ParseState::AwaitingHeader;
            contentLength_ = 0;
            return std::nullopt;
        }
        while (true)
        {
            if (state_ == ParseState::AwaitingHeader)
            {
                const auto headerEnd = buffer_.find("\r\n\r\n");
                if (headerEnd == std::string::npos)
                    return std::nullopt; // need more data

                const auto header = buffer_.substr(0, headerEnd);
                contentLength_ = parseContentLength(header);
                buffer_.erase(0, headerEnd + 4);
                if (contentLength_ <= 0
                    || contentLength_ > kMaxMessageBytes)
                {
                    // Malformed or absurd header: skip exactly this header
                    // block and keep any pipelined bytes after it.
                    state_ = ParseState::AwaitingHeader;
                    contentLength_ = 0;
                    continue;
                }
                state_ = ParseState::AwaitingBody;
            }
            else // AwaitingBody
            {
                if (static_cast<std::size_t>(contentLength_) > buffer_.size())
                    return std::nullopt; // Need more data

                FramedMessage msg;
                msg.body = buffer_.substr(0,
                                          static_cast<size_t>(contentLength_));
                msg.contentLength = contentLength_;
                buffer_.erase(0, static_cast<size_t>(contentLength_));
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

    /// Largest single message accepted (64 MB — far above any real LSP
    /// payload, far below OOM territory).
    static constexpr int kMaxMessageBytes = 64 * 1024 * 1024;
    /// Largest total buffered bytes before the buffer is dropped.
    static constexpr std::size_t kMaxBufferedBytes =
        static_cast<std::size_t>(kMaxMessageBytes) * 2;

    static int parseContentLength(std::string_view header)
    {
        // Case-insensitive "Content-Length", first occurrence wins (takes
        // the header block's value, not a smuggled duplicate's).
        std::string lower(header);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        const std::string key = "content-length";
        auto pos = lower.find(key);
        if (pos == std::string::npos)
            return -1;
        pos += key.size();
        // Skip optional whitespace and the colon separator.
        while (pos < lower.size()
               && (lower[pos] == ' ' || lower[pos] == '\t'
                   || lower[pos] == ':'))
            ++pos;
        if (pos >= lower.size() || !std::isdigit(
                static_cast<unsigned char>(lower[pos])))
            return -1;

        // Accumulate with overflow + cap checks (stoi would throw/overflow
        // on "99999999999").
        long long value = 0;
        while (pos < lower.size()
               && std::isdigit(static_cast<unsigned char>(lower[pos])))
        {
            value = value * 10 + (lower[pos] - '0');
            if (value > kMaxMessageBytes)
                return -1;
            ++pos;
        }
        return static_cast<int>(value);
    }

    ParseState  state_         = ParseState::AwaitingHeader;
    std::string buffer_;
    int         contentLength_ = 0;
};

} // namespace hathor::lsp
