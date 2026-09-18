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

#include <algorithm>
#include <cstdio>
#include <cstring>
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
        // NOTE: JUCE BinaryData strips dashes from resource names
        // (audio-waveform.svg -> audiowaveform_svg). These strings must
        // match BinaryData.h exactly — getNamedResource() does exact match
        // and silently returns null on mismatch (blank icon).
        case Icon::Explorer:    return "folderopen_svg";
        case Icon::Search:      return "search_svg";
        case Icon::GitBranch:   return "gitbranch_svg";
        case Icon::Bug:         return "bug_svg";
        case Icon::Terminal:    return "terminal_svg";
        case Icon::Warning:     return "trianglealert_svg";
        case Icon::Bot:         return "bot_svg";
        case Icon::Settings:    return "settings_svg";
        case Icon::Close:       return "x_svg";
        case Icon::Refresh:     return "refreshcw_svg";
        case Icon::Zap:         return "zap_svg";
        case Icon::Columns:     return "columns2_svg";
        case Icon::Play:        return "play_svg";
        case Icon::Stop:        return "square_svg";
        case Icon::Music:       return "music2_svg";
        case Icon::Activity:    return "activity_svg";
        case Icon::Folder:      return "folder_svg";
        case Icon::FileHathor:  return "music2_svg";
        case Icon::FileChuck:   return "filecode2_svg";
        case Icon::AudioWave:   return "audiowaveform_svg";
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

    // JUCE BinaryData strips dashes (audio-waveform.svg →
    // audiowaveform_svg). Accept either form, but log a loud warning on
    // fallback so mapping drift gets fixed instead of silently blanking.
    const char* wanted = resourceName(icon);
    int dataSize = 0;
    const char* data = BinaryData::getNamedResource(wanted, dataSize);
    if ((data == nullptr || dataSize <= 0) && std::strchr(wanted, '-') != nullptr)
    {
        std::string stripped = wanted;
        stripped.erase(std::remove(stripped.begin(), stripped.end(), '-'),
                       stripped.end());
        data = BinaryData::getNamedResource(stripped.c_str(), dataSize);
        if (data != nullptr && dataSize > 0)
            std::fprintf(stderr,
                         "[hathor:icons] resourceName \"%s\" mismatches "
                         "BinaryData (want \"%s\") — fix the mapping\n",
                         wanted, stripped.c_str());
    }
    if (data == nullptr || dataSize <= 0)
    {
        std::fprintf(stderr,
                     "[hathor:icons] missing BinaryData resource \"%s\" — "
                     "icon will render as a placeholder\n",
                     wanted);
        return nullptr;
    }

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
    const float side = std::min(bounds.getWidth(), bounds.getHeight());
    const juce::Rectangle<float> fitted(
        bounds.getCentreX() - side / 2.0f,
        bounds.getCentreY() - side / 2.0f,
        side, side);
    if (const juce::Drawable* d = cachedDrawable(icon, colour))
    {
        // Lucide viewBox is 24×24; preserve aspect while fitting bounds.
        d->drawWithin(g, fitted, juce::RectanglePlacement::centred, 1.0f);
        return;
    }
    // Missing glyph: never blank — draw a theme-visible placeholder box.
    g.setColour(colour.withAlpha(0.5f));
    g.drawRect(fitted, 1.0f);
    g.drawLine(fitted.getX(), fitted.getY(), fitted.getRight(),
               fitted.getBottom(), 1.0f);
    g.drawLine(fitted.getX(), fitted.getBottom(), fitted.getRight(),
               fitted.getY(), 1.0f);
}

void IconLibrary::clearCache() noexcept
{
    cache().clear();
}

} // namespace hathor::ui
