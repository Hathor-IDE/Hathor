// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * HathorLookAndFeel.hpp — central juce::LookAndFeel_V4 subclass that is the
 * single source of truth for Hathor's visual design tokens.
 *
 * Design system derived from the Stitch HTML/CSS mockup
 * (stitch_hathor_ide_dj_workspace/code.html + DESIGN.md).
 *
 * Key token categories:
 *   - Colours:  Material-3-inspired dark palette with a tactical neon-green
 *               accent (#00FF41).
 *   - Typography: JetBrains Mono across all weights and sizes.
 *   - Spacing:  4 px base unit (4/8/12/16/24).
 *   - Radius:   4 px default, 8 px large, pill/circle (full).
 *   - Elevation: flat — tonal layers only, no drop shadows.
 *
 * WCAG AA (Req 20.2):
 *   - Background luminance  #0e0e0e ≈ 0.2% of white  (≤ 15% ✓)
 *   - Text primary  #e5e2e1 on #0e0e0e  ≈ 12.6:1    (≥ 4.5:1 ✓)
 *   - Text secondary #b9ccb2 on #0e0e0e ≈ 11.1:1    (≥ 4.5:1 ✓)
 *
 * Font embedding (Req 5):
 *   JetBrains Mono is a Google Font — not guaranteed pre-installed on
 *   macOS/Linux. Four weights (Regular 400, Medium 500, SemiBold 600,
 *   Bold 700) are embedded as BinaryData resources and loaded via
 *   juce::Typeface::createSystemTypefaceFor().
 *
 * Requirements: 20.2, 20.4, 5
 */

#include <juce_gui_basics/juce_gui_basics.h>

// ---------------------------------------------------------------------------
// HathorLookAndFeel
// ---------------------------------------------------------------------------

class HathorLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HathorLookAndFeel();
    ~HathorLookAndFeel() override = default;

    // ========================================================================
    // Custom JUCE drawing overrides
    // ========================================================================

    /** Draw a button background matching the dark theme.
        Active buttons get the green accent; inactive buttons get a subtle
        hover tint. Buttons are drawn with 4 px corner radius (mockup rounded-md).
        JUCE 8.0 signature — bounds come from button.getLocalBounds().
    */
    void drawButtonBackground(juce::Graphics& g, juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted,
                              bool shouldDrawButtonAsDown) override;

    /** Draw button text using the design-token colours. */
    void drawButtonText(juce::Graphics& g, juce::TextButton& button,
                        bool shouldDrawButtonAsHighlighted,
                        bool shouldDrawButtonAsDown) override;

    /** Draw a horizontal slider matching the mockup's pill-track aesthetic.
        Track is a rounded capsule in surface-high; fill is accent green;
        thumb is white. JUCE 8.0 signature.
    */
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                          float sliderPos, float minSliderPos, float maxSliderPos,
                          juce::Slider::SliderStyle style,
                          juce::Slider& slider) override;

    /** Custom scrollbar: transparent track, dim thumb (matches mockup
        ::-webkit-scrollbar). JUCE 8.0 signature.
    */
    void drawScrollbar(juce::Graphics& g, juce::ScrollBar& scrollbar,
                       int x, int y, int width, int height,
                       bool isScrollbarVertical,
                       int thumbStartPosition, int thumbSize,
                       bool isMouseOver, bool isMouseDown) override;

    /** Intercept all default-font requests and substitute the embedded
        JetBrains Mono typeface. Bold-style requests get Bold; everything
        else gets Regular. For Medium (500) / SemiBold (600), use the
        static factory methods below, which bind directly to the typeface.
    */
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& font) override;

    /** Return a JetBrains Mono font for text buttons. */
    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;

    // ========================================================================
    // Static design tokens — the single source of truth
    // Stored as juce::uint32 (ARGB) for constexpr compatibility in JUCE 8.0.
    // Use juce::Colour(HathorLookAndFeel::Colours::xxx) at call sites.
    // ========================================================================

    struct Colours
    {
        // --- Background levels (Material-3 dark) ---
        static constexpr juce::uint32 background       = 0xff0e0e0eu; ///< surface-container-lowest (deepest)
        static constexpr juce::uint32 surface          = 0xff131313u; ///< surface (main workspace / editor)
        static constexpr juce::uint32 surfaceLow       = 0xff1c1b1bu; ///< surface-container-low (hover, inactive tabs)
        static constexpr juce::uint32 surfaceContainer = 0xff201f1fu; ///< surface-container (panel bgs, chat bubbles)
        static constexpr juce::uint32 surfaceHigh      = 0xff2a2a2au; ///< surface-container-high (inputs, slider tracks)
        static constexpr juce::uint32 surfaceHighest   = 0xff353534u; ///< surface-container-highest (borders, hover, thumb)
        static constexpr juce::uint32 surfaceBright    = 0xff3a3939u;

        // --- Text ---
        static constexpr juce::uint32 textPrimary     = 0xffe5e2e1u; ///< on-surface (primary text, code, active tab labels)
        static constexpr juce::uint32 textSecondary   = 0xffb9ccb2u; ///< on-surface-variant (muted labels, inactive tabs, comments)
        static constexpr juce::uint32 textMuted       = 0xff858585u; ///< line numbers, disabled text
        static constexpr juce::uint32 textDisabled    = 0xff666666u;

        // --- Accent ---
        static constexpr juce::uint32 accent          = 0xff00ff41u; ///< primary-container (tactical neon green)
        static constexpr juce::uint32 accentDim       = 0xff00e639u; ///< surface-tint (darker green for subtle highlights)
        static constexpr juce::uint32 accentOn        = 0xff003907u; ///< on-primary-container (text on green)

        // --- Semantic ---
        static constexpr juce::uint32 error           = 0xffff5f56u; ///< status dot (red — traffic light style)
        static constexpr juce::uint32 warning         = 0xffe0a020u; ///< amber (unsaved dot, warnings)

        // --- Code syntax (VS Code Dark+ colors used in mockup code example) ---
        static constexpr juce::uint32 codeText        = 0xffd4d4d4u;
        static constexpr juce::uint32 codeComment     = 0xff6a9955u;
        static constexpr juce::uint32 codeKeyword     = 0xff569cd6u;
        static constexpr juce::uint32 codeType        = 0xff4ec9b0u;
        static constexpr juce::uint32 codeString      = 0xffce9178u;
        static constexpr juce::uint32 codeFunction    = 0xffdcdcaau;
        static constexpr juce::uint32 codeMacro       = 0xffc586c0u;
        static constexpr juce::uint32 codeBracket     = 0xffffd700u;
        static constexpr juce::uint32 codeLineNum     = 0xff858585u;
    };

    struct Typography
    {
        // JetBrains Mono — font heights in points (≈ px at 72 DPI).
        // Source: Stitch mockup tailwind config.
        static constexpr float labelMd     = 11.0f; ///< labels, status bar — weight 500 (Medium)
        static constexpr float bodySm      = 12.0f; ///< body text — weight 400 (Regular)
        static constexpr float codeDefault = 13.0f; ///< code editor — weight 400 (Regular)
        static constexpr float bodyLg      = 14.0f; ///< chat messages, body text — weight 400 (Regular)
        static constexpr float headlineMd  = 18.0f; ///< section headers — weight 600 (SemiBold)
        static constexpr float headlineLg  = 24.0f; ///< window titles — weight 700 (Bold)
    };

    struct Spacing
    {
        static constexpr int unit   = 4;
        static constexpr int xs     = 4;
        static constexpr int sm     = 8;
        static constexpr int md     = 12;
        static constexpr int lg     = 16;
        static constexpr int xl     = 24;
        static constexpr int gutter = 1;
    };

    struct Radius
    {
        static constexpr float small   = 4.0f;  ///< buttons, inputs, tabs
        static constexpr float large   = 8.0f;  ///< cards, larger panels
        static constexpr float full    = 9999.0f; ///< pills, status dots
    };

    // ========================================================================
    // Static font factory methods — use these instead of raw juce::Font
    // for Medium (500) / SemiBold (600) weights, which JUCE's Font style
    // flags cannot express.
    // ========================================================================

    /// JetBrains Mono Regular, height in points.
    static juce::Font fontRegular(float height) noexcept;
    /// JetBrains Mono Medium (500), height in points.
    static juce::Font fontMedium(float height) noexcept;
    /// JetBrains Mono SemiBold (600), height in points.
    static juce::Font fontSemiBold(float height) noexcept;
    /// JetBrains Mono Bold (700), height in points.
    static juce::Font fontBold(float height) noexcept;

private:
    // ========================================================================
    // Embedded typeface accessors (function-local statics — lazily loaded
    // from BinaryData on first call, thread-safe in C++11+).
    // ========================================================================
    static juce::Typeface::Ptr regularTypeface();
    static juce::Typeface::Ptr mediumTypeface();
    static juce::Typeface::Ptr semiBoldTypeface();
    static juce::Typeface::Ptr boldTypeface();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorLookAndFeel)
};
