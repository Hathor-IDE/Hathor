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

namespace {

/// Raw SVG data + size for an icon, via typed BinaryData symbols.
/// Compile-time checked: a missing/renamed resource breaks the build
/// instead of silently blanking the icon at runtime (the old string
/// lookup failed exactly that way).
std::pair<const char*, int> iconData(IconLibrary::Icon icon) noexcept
{
    switch (icon)
    {
        case IconLibrary::Icon::Explorer:    return {BinaryData::folderopen_svg, BinaryData::folderopen_svgSize};
        case IconLibrary::Icon::Search:      return {BinaryData::search_svg, BinaryData::search_svgSize};
        case IconLibrary::Icon::GitBranch:   return {BinaryData::gitbranch_svg, BinaryData::gitbranch_svgSize};
        case IconLibrary::Icon::Bug:         return {BinaryData::bug_svg, BinaryData::bug_svgSize};
        case IconLibrary::Icon::Terminal:    return {BinaryData::terminal_svg, BinaryData::terminal_svgSize};
        case IconLibrary::Icon::Warning:     return {BinaryData::trianglealert_svg, BinaryData::trianglealert_svgSize};
        case IconLibrary::Icon::Bot:         return {BinaryData::bot_svg, BinaryData::bot_svgSize};
        case IconLibrary::Icon::Settings:    return {BinaryData::settings_svg, BinaryData::settings_svgSize};
        case IconLibrary::Icon::Close:       return {BinaryData::x_svg, BinaryData::x_svgSize};
        case IconLibrary::Icon::Refresh:     return {BinaryData::refreshcw_svg, BinaryData::refreshcw_svgSize};
        case IconLibrary::Icon::Zap:         return {BinaryData::zap_svg, BinaryData::zap_svgSize};
        case IconLibrary::Icon::Columns:     return {BinaryData::columns2_svg, BinaryData::columns2_svgSize};
        case IconLibrary::Icon::Play:        return {BinaryData::play_svg, BinaryData::play_svgSize};
        case IconLibrary::Icon::Stop:        return {BinaryData::square_svg, BinaryData::square_svgSize};
        case IconLibrary::Icon::Music:       return {BinaryData::music2_svg, BinaryData::music2_svgSize};
        case IconLibrary::Icon::Activity:    return {BinaryData::activity_svg, BinaryData::activity_svgSize};
        case IconLibrary::Icon::Folder:      return {BinaryData::folder_svg, BinaryData::folder_svgSize};
        case IconLibrary::Icon::FileHathor:  return {BinaryData::music2_svg, BinaryData::music2_svgSize};
        case IconLibrary::Icon::FileChuck:   return {BinaryData::filecode2_svg, BinaryData::filecode2_svgSize};
        case IconLibrary::Icon::AudioWave:   return {BinaryData::audiowaveform_svg, BinaryData::audiowaveform_svgSize};
        case IconLibrary::Icon::FileGeneric: return {BinaryData::file_svg, BinaryData::file_svgSize};
    }
    return {BinaryData::file_svg, BinaryData::file_svgSize};
}

} // namespace

const juce::Drawable* IconLibrary::cachedDrawable(Icon icon, juce::Colour colour)
{
    auto& c = cache();
    const auto key = cacheKey(icon, colour);
    if (auto it = c.find(key); it != c.end())
        return it->second.get();

    const auto [data, dataSize] = iconData(icon);
    if (data == nullptr || dataSize <= 0)
    {
        std::fprintf(stderr,
                     "[hathor:icons] empty BinaryData for icon %d — "
                     "renders as a placeholder\n",
                     static_cast<int>(icon));
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
