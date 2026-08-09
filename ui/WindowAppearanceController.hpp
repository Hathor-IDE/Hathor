// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * WindowAppearanceController — platform-isolated abstraction for window
 * opacity, background blur, and Acrylic effects.
 *
 * This controller is the single entry point for all platform-specific window
 * appearance logic. It isolates #ifdef JUCE_MAC / JUCE_WINDOWS / JUCE_LINUX
 * behind a stable interface, so the Settings UI and MainWindow never need to
 * branch on platform macros.
 *
 * Requirements: B5 (Window Opacity + Background Blur)
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <string>

namespace hathor::ui {

/**
 * PlatformCapabilities — result of runtime feature detection.
 *
 * Fields indicate what the *current* platform/environment actually supports,
 * not what the OS "should" support. For example, Linux may report
 * transparencySupported=false if no compositing manager is running.
 */
struct PlatformCapabilities
{
    bool transparencySupported = false;  ///< setAlpha() works reliably
    bool blurSupported          = false;  ///< native background blur is available
};

/**
 * WindowAppearanceState — the current applied appearance configuration.
 *
 * This is the state that lives in the window (not the edit buffer).
 * It is restored from persistence on startup and updated by Apply.
 */
struct WindowAppearanceState
{
    float opacityPercent     = 70.0f;   ///< 1.0–100.0; 100 = opaque
    int   macosBlurRadius    = 30;      ///< 0–100 (macOS only)
    bool  windowsAcrylic     = false;    ///< Acrylic effect on/off (Windows only)
};

/**
 * WindowAppearanceController
 *
 * Owns the platform-specific implementation for applying window opacity,
 * background blur, and Acrylic effects to the MainWindow. All platform
 * branching is confined to the .mm/.cpp file, so the Settings UI and
 * MainWindow never need to branch on platform macros.
 *
 * The controller is constructed once (in MainWindow) and lives for the
 * lifetime of the application. SettingsComponent queries its capabilities
 * and calls apply() / applyOpacity() / applyBlur() to change window
 * appearance.
 */
class WindowAppearanceController
{
public:
    explicit WindowAppearanceController(juce::TopLevelWindow& window);
    ~WindowAppearanceController();

    WindowAppearanceController(WindowAppearanceController&&) = delete;
    WindowAppearanceController& operator=(WindowAppearanceController&&) = delete;

    /**
     * Detect what the current platform environment actually supports.
     * Called at construction; re-callable for testing.
     */
    PlatformCapabilities detectCapabilities() const;

    /**
     * Apply a full appearance state to the live window.
     * This commits opacity + blur/Acrylic immediately (no restart needed).
     */
    void apply(const WindowAppearanceState& state);

    /**
     * Apply only opacity to the live window (used for live preview while editing).
     * Blur/Acrylic state is updated separately via applyBlur.
     */
    void applyOpacity(float opacityPercent);

    /**
     * Apply only the background blur/Acrylic effect (used for live preview).
     * Has no visible effect when opacity is 100%.
     */
    void applyBlur(const WindowAppearanceState& state);

    /**
     * Restore the window to a fully opaque, no-blur state.
     * Used as a fallback path for graceful degradation.
     */
    void restoreOpaque();

    /** Returns true if the current platform supports window transparency. */
    bool transparencySupported() const noexcept { return cachedCaps_.transparencySupported; }

    /** Returns true if the current platform supports native background blur. */
    bool blurSupported() const noexcept { return cachedCaps_.blurSupported; }

    /**
     * Returns the platform-appropriate default opacity.
     * macOS/Windows: 70.0f
     * Linux: 100.0f
     */
    static float defaultOpacity();

    /**
     * Returns a human-readable message when transparency is unsupported.
     * Used to display a warning in the Settings UI on Linux.
     */
    juce::String unsupportedReason() const;

    /** Access the window being controlled (used by SettingsComponent for live preview). */
    juce::TopLevelWindow& getWindow() const noexcept { return window_; }

    // -----------------------------------------------------------------------
    // Platform implementation interface (public so .mm can inherit)
    // -----------------------------------------------------------------------

    class PlatformImplBase
    {
    public:
        virtual ~PlatformImplBase() = default;
        virtual PlatformCapabilities detectCaps() const = 0;
        virtual void setWindowOpacity(float alpha) = 0;
        virtual void setWindowOpaque(bool opaque) = 0;
        virtual void setWindowBlur(int radius) = 0;
        virtual void setAcrylicEnabled(bool enabled) = 0;
        virtual juce::String getUnsupportedReason() const = 0;
    };

private:
    juce::TopLevelWindow& window_;
    std::unique_ptr<PlatformImplBase> impl_;
    mutable PlatformCapabilities cachedCaps_;
};

} // namespace hathor::ui
