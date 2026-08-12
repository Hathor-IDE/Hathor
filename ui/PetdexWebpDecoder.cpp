// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexWebpDecoder.hpp"

#include <webp/decode.h>

#include <cstdlib>

namespace hathor::ui {

PetdexWebpDecoder::DecodeResult
PetdexWebpDecoder::decode(const std::uint8_t* data, std::size_t size)
{
    DecodeResult result;

    if (data == nullptr || size == 0)
    {
        result.error = "empty WebP data";
        return result;
    }

    // Probe the header first: is this actually WebP, and are the dimensions
    // sane? WebPGetInfo is cheap and fails cleanly on non-WebP bytes.
    int width = 0;
    int height = 0;
    if (WebPGetInfo(data, size, &width, &height) != 1)
    {
        result.error = "not a valid WebP image";
        return result;
    }

    if (width <= 0 || height <= 0)
    {
        result.error = "invalid WebP dimensions";
        return result;
    }

    // Decode the full frame to unpremultiplied RGBA.
    std::uint8_t* pixels = WebPDecodeRGBA(data, size, &width, &height);
    if (pixels == nullptr)
    {
        result.error = "WebP decode failed (corrupt or truncated image)";
        return result;
    }

    const std::size_t byteCount =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;
    result.rgba.assign(pixels, pixels + byteCount);
    WebPFree(pixels);

    result.width  = width;
    result.height = height;
    result.ok     = true;
    return result;
}

} // namespace hathor::ui
