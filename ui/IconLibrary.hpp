// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * IconLibrary.hpp — central monochrome SVG icon system (Agent 0.6, audit I1).
 *
 * Icons are embedded Lucide SVGs (ISC licence — see ui/resources/icons/LICENSE)
 * compiled in via JUCE BinaryData. drawIcon() renders a tinted juce::Drawable
 * into a Graphics target; drawables are cached per (icon, colour) pair so
 * theme switches simply tint with the new palette colour.
 *
 * All icons are 24×24 stroke glyphs; they scale to any requested bounds and
 * recolour from the active theme (accent / foreground) at paint time.
 */

#include <juce_gui_basics/juce_gui_basics.h>

namespace hathor::ui {

class IconLibrary
{
public:
    /// The embedded icon set (Lucide names).
    enum class Icon
    {
        // ActivityRibbon
        Explorer,      ///< folder-open
        Search,        ///< search magnifier
        GitBranch,     ///< git-branch
        Bug,           ///< bug (debug panel)
        Terminal,      ///< terminal prompt
        Warning,       ///< triangle-alert (problems)
        Bot,           ///< bot (AI agent)
        Settings,      ///< settings gear
        // ChatSidebar
        Close,         ///< x
        // ChatSidebar header
        Refresh,       ///< refresh-cw (reconnect button)
        // BreadcrumbsBar
        Zap,           ///< command palette
        Columns,       ///< split editor
        // StatusRibbon
        Play,          ///< transport play
        Stop,          ///< transport stop
        Music,         ///< BPM indicator
        Activity,      ///< worker/pulse indicator
        // Explorer file types
        Folder,        ///< folder
        FileHathor,    ///< .hathor song (music-2)
        FileChuck,     ///< .ck ChucK source (file-code-2)
        AudioWave,     ///< baked .wav asset (audio-waveform)
        FileGeneric,   ///< generic file
    };

    /// Draw @p icon tinted with @p colour inside @p bounds.
    /// Must be called on the JUCE message thread.
    static void drawIcon(juce::Graphics& g, Icon icon,
                         juce::Rectangle<float> bounds, juce::Colour colour);

private:
    static const char* resourceName(Icon icon);
    static const juce::Drawable* cachedDrawable(Icon icon, juce::Colour colour);
};

} // namespace hathor::ui
