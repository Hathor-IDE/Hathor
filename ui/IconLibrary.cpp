// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * IconLibrary.cpp — embedded Lucide SVG icon rendering (Agent 0.6).
 *
 * Icons ship as BinaryData resources (hathor_icon_data target). Tinting is
 * done by substituting the Lucide "currentColor" stroke with the requested
 * theme colour before parsing, then caching the parsed Drawable per
 * (icon, colour) pair — repaints after a theme switch simply miss the cache.
 */

#include "IconLibrary.hpp"

#include <BinaryData.h>

#include <map>
#include <memory>

namespace hathor::ui {

namespace {

using DrawableCache = std::map<std::uint64_t, std::unique_ptr<juce::Drawable>>;

DrawableCache& cache()
{
    static DrawableCache c;
    return c;
}

std::uint64_t cacheKey(IconLibrary::Icon icon, juce::Colour colour)
{
    const auto h1 = std::hash<int> {}(static_cast<int>(icon));
    const auto h2 = static_cast<std::uint64_t>(colour.getARGB());
    return h1 * 0x9E3779B97F4A7C15ull ^ h2;
}

} // namespace

const char* IconLibrary::resourceName(Icon icon)
{
    switch (icon)
    {
        case Icon::Explorer:    return "folder-open_svg";
        case Icon::Search:      return "search_svg";
        case Icon::GitBranch:   return "git-branch_svg";
        case Icon::Bug:         return "bug_svg";
        case Icon::Terminal:    return "terminal_svg";
        case Icon::Warning:     return "triangle-alert_svg";
        case Icon::Bot:         return "bot_svg";
        case Icon::Settings:    return "settings_svg";
        case Icon::Close:       return "x_svg";
        case Icon::Zap:         return "zap_svg";
        case Icon::Columns:     return "columns-2_svg";
        case Icon::Play:        return "play_svg";
        case Icon::Stop:        return "square_svg";
        case Icon::Music:       return "music-2_svg";
        case Icon::Activity:    return "activity_svg";
        case Icon::Folder:      return "folder_svg";
        case Icon::FileHathor:  return "music-2_svg";
        case Icon::FileChuck:   return "file-code-2_svg";
        case Icon::AudioWave:   return "audio-waveform_svg";
        case Icon::FileGeneric: return "file_svg";
    }
    return "file_svg";
}

const juce::Drawable* IconLibrary::cachedDrawable(Icon icon, juce::Colour colour)
{
    auto& c = cache();
    const auto key = cacheKey(icon, colour);
    if (auto it = c.find(key); it != c.end())
        return it->second.get();

    int dataSize = 0;
    const char* data = BinaryData::getNamedResource(resourceName(icon), dataSize);
    if (data == nullptr || dataSize <= 0)
        return nullptr;

    // Tint: swap Lucide's stroke="currentColor" for the requested colour and
    // drop any class attribute noise before parsing.
    juce::String svg(juce::CharPointer_UTF8(data), static_cast<size_t>(dataSize));
    svg = svg.replace("stroke=\"currentColor\"",
                      "stroke=\"" + colour.toDisplayString(true) + "\"");

    auto drawable = juce::Drawable::createFromImageData(
        svg.toRawUTF8(), svg.getNumBytesAsUTF8());
    if (drawable == nullptr)
        return nullptr;

    auto* raw = drawable.get();
    c.emplace(key, std::move(drawable));
    return raw;
}

void IconLibrary::drawIcon(juce::Graphics& g, Icon icon,
                           juce::Rectangle<float> bounds, juce::Colour colour)
{
    if (const juce::Drawable* d = cachedDrawable(icon, colour))
    {
        // Lucide viewBox is 24×24; preserve aspect while fitting bounds.
        const float side = std::min(bounds.getWidth(), bounds.getHeight());
        const juce::Rectangle<float> fitted(
            bounds.getCentreX() - side / 2.0f,
            bounds.getCentreY() - side / 2.0f,
            side, side);
        d->drawWithin(g, fitted, juce::RectanglePlacement::centred, 1.0f);
    }
}

void IconLibrary::drawIcon(juce::Graphics& g, Icon icon,
                           juce::Rectangle<int> bounds, juce::Colour colour)
{
    drawIcon(g, icon, bounds.toFloat(), colour);
}

} // namespace hathor::ui
