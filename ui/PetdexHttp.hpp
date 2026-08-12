// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexHttp.hpp — minimal shared HTTP GET used by the Petdex services.
 *
 * This is deliberately NOT a general-purpose remote-resource framework: it is
 * one function, used by exactly the two Petdex services, with a byte cap and a
 * timeout so a hostile/slow endpoint cannot stall the caller. Runs on the
 * caller's background thread (never the JUCE message/audio thread).
 *
 * Platform note (documented in docs/design/petdex-d1-d4-decision.md):
 * juce::URL works on macOS (NSURLSession) and Windows (WinInet); on Linux JUCE
 * lazily loads libcurl symbols and fails cleanly when unavailable.
 */

#include <cstdint>
#include <string>

namespace hathor::ui {

class PetdexHttp
{
public:
    enum class Error
    {
        None,
        Network,      ///< connection/DNS/TLS failure (or no transport available)
        Http,         ///< non-success HTTP status
        TooLarge,     ///< body exceeded the caller's byte cap
        Empty,        ///< zero-length body
    };

    struct Response
    {
        bool       ok = false;
        Error      error = Error::None;
        int        statusCode = 0;
        std::string body;
    };

    /// Perform a GET request. Must be called off the message thread.
    static Response get(const std::string& url,
                        std::int64_t maxBytes,
                        int timeoutMs,
                        const std::string& userAgent);
};

} // namespace hathor::ui
