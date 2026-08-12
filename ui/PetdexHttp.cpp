// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexHttp.hpp"

#include <juce_core/juce_core.h>

namespace hathor::ui {

PetdexHttp::Response PetdexHttp::get(const std::string& url,
                                     std::int64_t maxBytes,
                                     int timeoutMs,
                                     const std::string& userAgent)
{
    Response response;

    juce::URL juceUrl(url);
    int statusCode = 0;
    auto stream = juceUrl.createInputStream(
        juce::URL::InputStreamOptions(juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs(timeoutMs)
            .withNumRedirectsToFollow(4)
            .withExtraHeaders("User-Agent: " + userAgent + "\r\n")
            .withStatusCode(&statusCode));

    if (stream == nullptr)
    {
        response.error = Error::Network;
        return response;
    }

    if (statusCode != 0 && statusCode != 200)
    {
        response.error = Error::Http;
        response.statusCode = statusCode;
        return response;
    }

    response.body.reserve(static_cast<std::size_t>(std::min<std::int64_t>(maxBytes, 256 * 1024)));
    char buf[8192];
    for (;;)
    {
        const int n = stream->read(buf, static_cast<int>(sizeof(buf)));
        if (n <= 0)
            break;
        response.body.append(buf, static_cast<std::size_t>(n));
        if (static_cast<std::int64_t>(response.body.size()) > maxBytes)
        {
            response.error = Error::TooLarge;
            return response;
        }
    }

    if (response.body.empty())
    {
        response.error = Error::Empty;
        return response;
    }

    response.ok = true;
    return response;
}

} // namespace hathor::ui
