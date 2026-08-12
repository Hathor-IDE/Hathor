// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetdexWebpDecoder.hpp — WebP decoding via libwebp (Phase G / D2).
 *
 * JUCE-free (links the `webp` static library). Decodes WebP bytes into
 * unpremultiplied RGBA, which the UI layer converts into a juce::Image.
 *
 * Dependency evidence (docs/design/petdex-d1-d4-decision.md): the live Petdex
 * spritesheet assets are WebP (verified: a real pet's spritesheet.webp is
 * 1536x1872 WebP; the zip package contains the same WebP, no better-supported
 * representation), and JUCE 8.0.4 ships no WebP decoder. libwebp (BSD-3) is
 * the small, maintained, appropriately licensed decoder this project uses.
 *
 * All decoding happens off the audio path (background thread); this class
 * performs no I/O and allocates only the output buffer.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hathor::ui {

class PetdexWebpDecoder
{
public:
    struct DecodeResult
    {
        bool   ok = false;
        int    width  = 0;
        int    height = 0;
        std::vector<std::uint8_t> rgba;   ///< width*height*4, R,G,B,A unpremultiplied
        std::string error;
    };

    /// Decode WebP data. Returns ok=false (never throws, never crashes) for
    /// corrupt, truncated, or non-WebP input.
    static DecodeResult decode(const std::uint8_t* data, std::size_t size);
};

} // namespace hathor::ui
