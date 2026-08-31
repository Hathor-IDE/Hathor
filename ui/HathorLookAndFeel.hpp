// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
  * HathorLookAndFeel.hpp — central juce::LookAndFeel_V4 subclass that is the
  * single source of truth for Hathor's visual design tokens.
  *
  * This file is the single source of truth for:
  *   - All JUCE colour-ID defaults (setColour calls)
  *   - JetBrains Mono font loading (4 weights, embedded as BinaryData) — code
  *     editor, terminal, log/output views only (Audit §1.2 I6).
  *   - Inter font loading (4 weights, embedded as BinaryData, OFL) — all
  *     other UI chrome (buttons, menus, labels, chat prose, breadcrumbs).
  *   - Custom button / slider / scrollbar drawing
  *
  * Font-family policy (Audit §1.2 I6):
  *   - getTypefaceForFont() is NOT overridden; JUCE resolves default
  *     sans-serif/monospace fallback naturally.  Components must call
  *     getCodeFont() (JetBrains Mono) or getUiFont() (Inter) explicitly.
  *
  * Design system derived from the Stitch HTML/CSS mockup
  * (stitch_hathor_ide_dj_workspace/code.html + DESIGN.md).
  *
  * Key token categories:
  *   - Colours:  Material-3-inspired dark palette with a tactical neon-green
  *               accent (#00FF41).
  *   - Typography: JetBrains Mono for code; Inter for UI chrome.
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
  *   Inter is a SIL Open Font License (OFL) font.  Four weights are
  *   embedded the same way for proportional UI chrome.
  *
  * Requirements: 20.2, 20.4, 5
  */

#include <juce_gui_basics/juce_gui_basics.h>

// ---------------------------------------------------------------------------
// Palette — runtime theme value-type (A1 design-token engine)
// ---------------------------------------------------------------------------

/**
  * Palette
  *
  * A flat value-type containing the complete set of colour tokens required by
  * the themed UI.  Each theme (dark, light, purple/neon, capuchin, sand) is
  * a fully-specified Palette instance with identical shape.  The LookAndFeel
  * holds one currentPalette_ and swaps it at runtime via setPalette().
  *
  * All themed components read colours through getPalette() rather than
  * referencing compile-time constants or maintaining their own copies.
  *
  * §4.1 (PROGRAM.md): "Each theme defines its complete token set explicitly
  * (current + Purple/Neon, Capuchin, Sand, Light). Light is not derived from
  * dark assumptions."
  */
struct Palette
{
    // --- Background levels (Material-3 dark) ---
    juce::Colour background;
    juce::Colour surface;
    juce::Colour surfaceLow;
    juce::Colour surfaceContainer;
    juce::Colour surfaceHigh;
    juce::Colour surfaceHighest;
    juce::Colour surfaceBright;

    // --- Text ---
    juce::Colour textPrimary;
    juce::Colour textSecondary;
    juce::Colour textMuted;
    juce::Colour textDisabled;

    // --- Accent ---
    juce::Colour accent;
    juce::Colour accentDim;
    juce::Colour accentOn;

    // --- Semantic ---
    juce::Colour error;
    juce::Colour warning;

    // --- Code syntax (VS Code Dark+ colors used in mockup code example) ---
    juce::Colour codeText;
    juce::Colour codeComment;
    juce::Colour codeKeyword;
    juce::Colour codeType;
    juce::Colour codeString;
    juce::Colour codeFunction;
    juce::Colour codeMacro;
    juce::Colour codeBracket;
    juce::Colour codeLineNum;

    /// Build the default (current) dark theme palette.
    static Palette defaultPalette() noexcept;
};

// ---------------------------------------------------------------------------
// Theme registry — the five themes defined by A1/B3 (§4.1, PROGRAM.md)
// ---------------------------------------------------------------------------

/// Identifies one of the five selectable themes.
enum class ThemeId
{
    Dark,          ///< current default (Material-3 dark)
    PurpleNeon,    ///< Purple/Neon accent variant
    Capuchin,      ///< Capuchin brown-accent variant
    Sand,          ///< Sand warm variant
    Light,         ///< Light theme
};

/// Return the Palette for a given ThemeId.
Palette paletteForTheme(ThemeId id) noexcept;

/// Return the human-readable name for a theme (for Settings list).
juce::String themeDisplayName(ThemeId id) noexcept;

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

    // ========================================================================
    // Font-family policy (Audit §1.2 I6)
    //
    // getTypefaceForFont() is NOT overridden — JUCE's default sans-serif
    // fallback is allowed to resolve naturally for UI chrome.  Components that
    // need a specific face call getUiFont() or getCodeFont() explicitly.
    // =========================================================================

    /** Return a JetBrains Mono font for the code editor at the given height. */
    static juce::Font getCodeFont(float height) noexcept;
    /** Return a JetBrains Mono font for the code editor with a given height and style.
        Style is a bitwise-or of juce::Font::FontStyleFlags (e.g. juce::Font::bold). */
    static juce::Font getCodeFont(float height, int styleFlags) noexcept;

    /** Return an Inter font for UI chrome at the given height. */
    static juce::Font getUiFont(float height) noexcept;
    /** Return an Inter font for UI chrome with a given height and style. */
    static juce::Font getUiFont(float height, int styleFlags) noexcept;

    /** Return an Inter font for text buttons. */
    juce::Font getTextButtonFont(juce::TextButton& /*button*/, int buttonHeight) override;

    // ========================================================================
    // Runtime palette management (A1 design-token engine)
    // ========================================================================

    /** Returns the current active palette. */
    const Palette& getPalette() const noexcept { return currentPalette_; }

    /**
      * Replace the current palette at runtime and re-apply all JUCE colour
      * IDs so that components using LookAndFeel colour IDs update immediately.
      * Components that read colours directly from the palette (via
      * getPalette()) must repaint after this call.
      *
      * B3 will call this to switch themes without a rebuild.
      */
    void setPalette(const Palette& newPalette);

    /** Returns the default (dark) palette. */
    static Palette defaultPalette() noexcept { return Palette::defaultPalette(); }

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

     // =========================================================================
    // Static font factory methods — use these instead of raw juce::Font
    // for JetBrains Mono Medium/SemiBold weights, which JUCE's Font style
    // flags cannot express.
    //
    // Typography policy (Audit §1.2 I6):
    //   - JetBrains Mono: code editors, terminal, logs/output.
    //   - Inter (embedded, OFL): all other UI chrome (buttons, menus, labels,
    //     chat prose, breadcrumbs, status bar).
    // =========================================================================

    /// JetBrains Mono Regular, height in points.
    static juce::Font fontRegular(float height) noexcept;
    /// JetBrains Mono Medium (500), height in points.
    static juce::Font fontMedium(float height) noexcept;
    /// JetBrains Mono SemiBold (600), height in points.
    static juce::Font fontSemiBold(float height) noexcept;
    /// JetBrains Mono Bold (700), height in points.
    static juce::Font fontBold(float height) noexcept;

    /// Inter Regular, height in points.
    static juce::Font uiFontRegular(float height) noexcept;
    /// Inter Medium (500), height in points.
    static juce::Font uiFontMedium(float height) noexcept;
    /// Inter SemiBold (600), height in points.
    static juce::Font uiFontSemiBold(float height) noexcept;
    /// Inter Bold (700), height in points.
    static juce::Font uiFontBold(float height) noexcept;

    // =========================================================================
    // Embedded typeface accessors — function-local statics, lazily loaded
    // from BinaryData on first call (thread-safe in C++11+).
    // ---------------------------------------------------------------------------
    // Inter typeface accessors are public: external components (e.g. LSP hover
    // and completion popups) construct FontOptions with Inter directly.
    // =========================================================================
    static juce::Typeface::Ptr interRegularTypeface();
    static juce::Typeface::Ptr interBoldTypeface();

    // ========================================================================
    // Convenience: fetch a colour directly from the current palette.
    // Components should prefer this over hard-coded values.
    // ========================================================================

    /// Helper: obtain the current palette when you don't have a Component
    /// reference (e.g. inside a TreeViewItem which is not a Component).
    static const Palette& globalPalette() noexcept
    {
        return *globalPalette_;
    }

    /// Helper: obtain the LookAndFeel as HathorLookAndFeel from any Component.
    static HathorLookAndFeel& fromComponent(juce::Component& c) noexcept
    {
        juce::LookAndFeel& lf = c.getLookAndFeel();
        return *static_cast<HathorLookAndFeel*>(&lf);
    }

    /// Const overload — for use in const Component methods.
    static const HathorLookAndFeel& fromComponent(const juce::Component& c) noexcept
    {
        const juce::LookAndFeel& lf = c.getLookAndFeel();
        return *static_cast<const HathorLookAndFeel*>(&lf);
    }

    /// Install the global palette pointer (used by globalPalette()).
    static void setGlobalPalette(const Palette* p) noexcept
    {
        globalPalette_ = p;
    }

private:
    // =========================================================================
    // Runtime design-token state
    // =========================================================================

    Palette currentPalette_;

    /// Global palette pointer — points to currentPalette_ of the single
    /// HathorLookAndFeel instance. Set in the constructor. Used by
    /// globalPalette() for components that can't reach a Component.
    static const Palette* globalPalette_;

    // ========================================================================
    // Re-apply all JUCE colour IDs from the current palette.
    // Called from setPalette() and the constructor.
    // ========================================================================

    void applyPaletteToColours() noexcept;

    // =========================================================================
    // Embedded typeface accessors (function-local statics — lazily loaded
    // from BinaryData on first call, thread-safe in C++11+).
    //
    // JetBrains Mono — code: editor, terminal, log/output views.
    // Inter accessors above are public for external font construction.
    // =========================================================================
    static juce::Typeface::Ptr regularTypeface();
    static juce::Typeface::Ptr mediumTypeface();
    static juce::Typeface::Ptr semiBoldTypeface();
    static juce::Typeface::Ptr boldTypeface();
    static juce::Typeface::Ptr interMediumTypeface();
    static juce::Typeface::Ptr interSemiBoldTypeface();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorLookAndFeel)
};
