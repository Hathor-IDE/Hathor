// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WindowAppearanceController.cpp — platform-isolated implementation for
 * window opacity, background blur, and Acrylic effects.
 *
 * Requirements: B5 (Window Opacity + Background Blur)
 *
 * Platform isolation strategy:
 *   - macOS: uses TopLevelWindow::setAlpha() for full-window alpha.
 *            Background blur via NSVisualEffectView (Objective-C++ bridge).
 *   - Windows: uses TopLevelWindow::setAlpha() (JUCE adds WS_EX_LAYERED).
 *              Acrylic via SetWindowCompositionAttribute.
 *   - Linux: feature-detects compositor; degrades to opaque if unavailable.
 */

#include "WindowAppearanceController.hpp"

// ---------------------------------------------------------------------------
// Platform-specific headers
// ---------------------------------------------------------------------------

#if JUCE_MAC
#  import <Cocoa/Cocoa.h>
#elif JUCE_WINDOWS
#  include <Windows.h>
#  include <shellapi.h>
#endif

namespace hathor::ui {

// ===========================================================================
// PlatformImpl implementations (platform-specific)
// ===========================================================================

#if JUCE_MAC
// ---------------------------------------------------------------------------
// macOS: full-window alpha + NSVisualEffectView blur
// ---------------------------------------------------------------------------

/**
 * macOS implementation: full-window alpha via TopLevelWindow::setAlpha(),
 * NSVisualEffectView for background blur radius.
 */
class WindowAppearanceController::MacPlatformImpl : public PlatformImpl
{
public:
    explicit MacPlatformImpl(juce::TopLevelWindow& window) : PlatformImpl(window) {}

    PlatformCapabilities detectCaps() const override
    {
        PlatformCapabilities caps;
        caps.transparencySupported = true;  // macOS reliably supports full-window alpha
        caps.blurSupported          = true;  // NSVisualEffectView provides backdrop blur
        return caps;
    }

    void setWindowOpacity(float alpha) override
    {
        window_.setAlpha(alpha);
        window_.setOpaque(false);
    }

    void setWindowOpaque(bool opaque) override
    {
        if (opaque)
        {
            window_.setAlpha(1.0f);
            window_.setOpaque(true);
        }
        else
        {
            window_.setOpaque(false);
        }
    }

    void setWindowBlur(int radius) override
    {
        applyMacOSVisualEffect(window_, radius);
    }

    void setAcrylicEnabled(bool /*enabled*/) override
    {
        // Acrylic is a Windows concept; no-op on macOS.
    }

    juce::String getUnsupportedReason() const override
    {
        return {};  // never unsupported on macOS
    }

private:
    /**
     * Install an NSVisualEffectView as the window's content view's background
     * to produce a true background blur (blurs desktop/pixels behind the window,
     * NOT a filter over Hathor's own UI).
     *
     * The blur radius maps to NSVisualEffectView's material + blending properties:
     *   radius 0   → remove the effect view (solid background)
     *   radius >0  → NSVisualEffectMaterialHUDWindow with blendingOpacity
     *                 proportional to the radius
     */
    static void applyMacOSVisualEffect(juce::TopLevelWindow& window, int radius)
    {
        // On macOS, getWindowHandle() returns the NSView* for the window
        NSView* contentView = static_cast<NSView*>(window.getWindowHandle());
        if (contentView == nil)
            return;

        if (radius <= 0)
        {
            // Remove any existing NSVisualEffectView subviews
            removeVisualEffectViews(contentView);
            return;
        }

        // Remove any existing effect views first
        removeVisualEffectViews(contentView);

        // Create NSVisualEffectView for background blur
        NSVisualEffectView* effectView =
            [[NSVisualEffectView alloc] initWithFrame:[contentView bounds]];
        effectView.material = NSVisualEffectMaterialHUDWindow;
        effectView.blendingMode = NSVisualEffectBlendingModeBehindWindow;

        // Map 1-100 radius to 0.1-1.0 blending opacity
        CGFloat blendOpacity =
            static_cast<CGFloat>(juce::jlimit(1, 100, radius)) / 100.0f;
        effectView.blendingOpacity = blendOpacity;
        effectView.state = NSVisualEffectStateActive;
        effectView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        // Insert at the bottom of the view hierarchy so it's behind all content
        [contentView addSubview:effectView positioned:NSWindowBelow relativeTo:nil];
        [effectView release];
    }

    static void removeVisualEffectViews(NSView* contentView)
    {
        if (contentView == nil)
            return;

        // Remove any existing NSVisualEffectView subviews
        for (NSUInteger i = [contentView subviews].count; i > 0; --i)
        {
            NSView* subview = [contentView subviews][i - 1];
            if ([subview isKindOfClass:[NSVisualEffectView class]])
            {
                [subview removeFromSuperview];
            }
        }
    }
};
#endif  // JUCE_MAC

#if JUCE_WINDOWS
// ---------------------------------------------------------------------------
// Windows: layered window transparency + Acrylic via DWM
// ---------------------------------------------------------------------------

/**
 * Windows implementation: full-window alpha via WS_EX_LAYERED (JUCE sets this
 * when setAlpha() is called on a TopLevelWindow). Acrylic via
 * SetWindowCompositionAttribute.
 */
class WindowAppearanceController::WindowsPlatformImpl : public PlatformImpl
{
public:
    explicit WindowsPlatformImpl(juce::TopLevelWindow& window) : PlatformImpl(window) {}

    PlatformCapabilities detectCaps() const override
    {
        PlatformCapabilities caps;
        caps.transparencySupported = true;   // WS_EX_LAYERED is reliable on Windows
        caps.blurSupported          = true;   // DWM Acrylic available on Win10+
        return caps;
    }

    void setWindowOpacity(float alpha) override
    {
        // JUCE's TopLevelWindow::setAlpha() internally uses UpdateLayeredWindow
        // (which requires WS_EX_LAYERED). This is the reliable path on Windows.
        window_.setAlpha(alpha);
        window_.setOpaque(false);
    }

    void setWindowOpaque(bool opaque) override
    {
        if (opaque)
        {
            window_.setAlpha(1.0f);
            window_.setOpaque(true);
        }
        else
        {
            window_.setOpaque(false);
        }
    }

    void setWindowBlur(int /*radius*/) override
    {
        // Blur radius slider is not exposed on Windows — Acrylic is binary.
    }

    void setAcrylicEnabled(bool enabled) override
    {
        applyWindowsAcrylic(window_, enabled);
    }

    juce::String getUnsupportedReason() const override
    {
        return {};  // Acrylic available on Win10+
    }

private:
    /**
     * Enable/disable the Acrylic blur effect on the window.
     *
     * Uses SetWindowCompositionAttribute with ACCENT_ENABLE_ACRYLIC_BLUR
     * to enable Acrylic, which blurs the desktop background visible
     * through the translucent window. When disabled, falls back to
     * normal layered-window transparency.
     *
     * This does NOT blur Hathor's own UI — it's the Windows compositor
     * blurring the pixels behind the window.
     */
    static void applyWindowsAcrylic(juce::TopLevelWindow& window, bool enabled)
    {
        void* native = window.getWindowHandle();
        if (native == nullptr)
            return;

        HWND hwnd = static_cast<HWND>(native);

        // ACCENT_POLICY struct — matches the Windows SDK definition
        struct AccentPolicy
        {
            int AccentState;   // ACCENT_ENABLE_*
            int AccentFlags;
            int GradientColor; // 0xAABBGGRR (0 = transparent)
            int AnimationId;
        };

        // SetWindowCompositionAttribute is not in the default SDK headers
        // but is available on Windows 10+. Load it dynamically.
        using SetWindowCompositionAttributeFunc =
            BOOL (WINAPI*)(HWND, AccentPolicy*, SIZE_T);

        HMODULE user32 = LoadLibraryA("user32.dll");
        if (user32 == nullptr)
            return;

        auto pSetWindowCompositionAttribute =
            reinterpret_cast<SetWindowCompositionAttributeFunc>(
                GetProcAddress(user32, "SetWindowCompositionAttribute"));

        if (pSetWindowCompositionAttribute != nullptr)
        {
            AccentPolicy policy;
            if (enabled)
            {
                policy.AccentState   = 3;     // ACCENT_ENABLE_ACRYLIC_BLUR
                policy.AccentFlags   = 0;
                policy.GradientColor = 0;     // fully transparent
                policy.AnimationId    = 0;
            }
            else
            {
                policy.AccentState   = 1;     // ACCENT_ENABLED (normal transparency)
                policy.AccentFlags   = 0;
                policy.GradientColor = 0;
                policy.AnimationId    = 0;
            }

            SIZE_T attrSize = sizeof(AccentPolicy);
            pSetWindowCompositionAttribute(hwnd, &policy, attrSize);
        }

        FreeLibrary(user32);
    }
};
#endif  // JUCE_WINDOWS

#if JUCE_LINUX
// ---------------------------------------------------------------------------
// Linux: feature-detect compositor; degrade to opaque if unavailable
// ---------------------------------------------------------------------------

/**
 * Linux implementation: feature-detects compositor support.
 * If no compositor is running, degrades to opaque (100%).
 */
class WindowAppearanceController::LinuxPlatformImpl : public PlatformImpl
{
public:
    explicit LinuxPlatformImpl(juce::TopLevelWindow& window)
        : PlatformImpl(window), lastCaps_{}
    {}

    PlatformCapabilities detectCaps() const override
    {
        PlatformCapabilities caps;
        caps.transparencySupported = detectCompositor();
        caps.blurSupported          = caps.transparencySupported && supportsBlurProtocol();
        lastCaps_ = caps;  // cache for use by setWindowOpacity/setWindowOpaque
        return caps;
    }

    void setWindowOpacity(float alpha) override
    {
        if (lastCaps_.transparencySupported)
        {
            // setAlpha() on X11 with a compositor applies via _NET_WM_WINDOW_OPACITY
            window_.setAlpha(alpha);
            window_.setOpaque(false);
        }
        else
        {
            // Fall back to fully opaque
            window_.setAlpha(1.0f);
            window_.setOpaque(true);
        }
    }

    void setWindowOpaque(bool opaque) override
    {
        if (opaque || !lastCaps_.transparencySupported)
        {
            window_.setAlpha(1.0f);
            window_.setOpaque(true);
        }
        else
        {
            window_.setOpaque(false);
        }
    }

    void setWindowBlur(int /*radius*/) override
    {
        // Linux blur is feature-detected. If unsupported, no-op.
    }

    void setAcrylicEnabled(bool /*enabled*/) override
    {
        // Acrylic is a Windows concept; no-op on Linux.
    }

    juce::String getUnsupportedReason() const override
    {
        if (!lastCaps_.transparencySupported)
        {
            return "Window transparency requires a running compositor "
                   "(e.g. picom, compton, or a Wayland compositor). "
                   "No compositor was detected.";
        }
        if (!lastCaps_.blurSupported)
        {
            return "Background blur requires a compositor that supports "
                   "_NET_WM_WINDOW_BLUR_BEHIND (e.g. KWin with blur). "
                   "Not available on this compositor.";
        }
        return {};
    }

private:
    mutable PlatformCapabilities lastCaps_{};

    /**
     * Detect whether a compositing manager is running.
     *
     * Checks:
     *   1. XDG_SESSION_TYPE=wayland → always composited
     *   2. X11: check _NET_WM_CM_S0 selection via xprop
     *   3. Fall back to checking for common compositor processes
     */
    static bool detectCompositor()
    {
        // 1. Wayland always has a compositor
        const char* sessionType = std::getenv("XDG_SESSION_TYPE");
        if (sessionType != nullptr &&
            juce::String(sessionType).containsIgnoreCase("wayland"))
            return true;

        // 2. Check _NET_WM_CM_S0 selection (standard X11 compositor detection)
        if (isX11CompositorRunning())
            return true;

        // 3. Fall back: check for common compositor processes
        const char* compositors[] = {
            "picom", "compton", "xcompmgr", "mutter", "kwin_x11", "kwin_wayland",
            nullptr
        };

        for (int i = 0; compositors[i] != nullptr; ++i)
        {
            if (checkProcessRunning(compositors[i]))
                return true;
        }

        return false;
    }

    static bool checkProcessRunning(const char* name)
    {
        juce::String cmd = "pgrep -x " + juce::String(name);
        FILE* pipe = popen(cmd.toStdString().c_str(), "r");
        if (pipe == nullptr)
            return false;

        char buffer[16];
        bool found = (fgets(buffer, sizeof(buffer), pipe) != nullptr);
        pclose(pipe);
        return found;
    }

    static bool isX11CompositorRunning()
    {
        FILE* pipe = popen("xprop -root _NET_WM_CM_S0 2>/dev/null", "r");
        if (pipe == nullptr)
            return false;

        char buffer[256];
        bool found = false;
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr)
        {
            juce::String result(buffer);
            found = result.contains("_NET_WM_CM_S0");
        }
        pclose(pipe);
        return found;
    }

    static bool supportsBlurProtocol()
    {
        // Check for compositors known to support blur-back
        const char* blurCompositors[] = {
            "picom", "kwin_x11", "kwin_wayland",
            nullptr
        };

        for (int i = 0; blurCompositors[i] != nullptr; ++i)
        {
            if (checkProcessRunning(blurCompositors[i]))
                return true;
        }

        return false;
    }
};
#endif  // JUCE_LINUX

// ===========================================================================
// Fallback implementation (unknown platform)
// ===========================================================================

#if !JUCE_MAC && !JUCE_WINDOWS && !JUCE_LINUX
class WindowAppearanceController::FallbackPlatformImpl : public PlatformImpl
{
public:
    explicit FallbackPlatformImpl(juce::TopLevelWindow& window) : PlatformImpl(window) {}

    PlatformCapabilities detectCaps() const override
    {
        PlatformCapabilities caps{};
        caps.transparencySupported = false;
        caps.blurSupported          = false;
        return caps;
    }

    void setWindowOpacity(float /*alpha*/) override {}

    void setWindowOpaque(bool opaque) override
    {
        if (opaque)
            window_.setOpaque(true);
    }

    void setWindowBlur(int /*radius*/) override {}
    void setAcrylicEnabled(bool /*enabled*/) override {}

    juce::String getUnsupportedReason() const override
    {
        return "Window transparency is not supported on this platform.";
    }
};
#endif

// ===========================================================================
// Public API
// ===========================================================================

WindowAppearanceController::WindowAppearanceController(juce::TopLevelWindow& window)
    : window_(window)
{
#if JUCE_MAC
    impl_ = std::make_unique<MacPlatformImpl>(window);
#elif JUCE_WINDOWS
    impl_ = std::make_unique<WindowsPlatformImpl>(window);
#elif JUCE_LINUX
    impl_ = std::make_unique<LinuxPlatformImpl>(window);
#else
    impl_ = std::make_unique<FallbackPlatformImpl>(window);
#endif

    cachedCaps_ = detectCapabilities();
}

WindowAppearanceController::~WindowAppearanceController() = default;

PlatformCapabilities WindowAppearanceController::detectCapabilities() const
{
    if (impl_ != nullptr)
    {
        auto caps = impl_->detectCaps();
        cachedCaps_ = caps;
        return caps;
    }

    PlatformCapabilities caps{};
    caps.transparencySupported = false;
    caps.blurSupported          = false;
    return caps;
}

void WindowAppearanceController::apply(const WindowAppearanceState& state)
{
    if (!cachedCaps_.transparencySupported)
    {
        // No transparency support: force opaque
        restoreOpaque();
        return;
    }

    // Apply opacity
    const float alpha = juce::jlimit(0.01f, 1.0f, state.opacityPercent / 100.0f);
    if (impl_)
        impl_->setWindowOpacity(alpha);

    // Apply blur/Acrylic — only meaningful when opacity < 100%
    if (state.opacityPercent < 100.0f && impl_ != nullptr)
    {
#if JUCE_MAC
        impl_->setWindowBlur(state.macosBlurRadius);
#elif JUCE_WINDOWS
        impl_->setAcrylicEnabled(state.windowsAcrylic);
#endif
    }
}

void WindowAppearanceController::applyOpacity(float opacityPercent)
{
    if (!cachedCaps_.transparencySupported)
    {
        if (impl_)
            impl_->setWindowOpaque(true);
        return;
    }

    const float alpha = juce::jlimit(0.01f, 1.0f, opacityPercent / 100.0f);
    if (impl_)
        impl_->setWindowOpacity(alpha);
}

void WindowAppearanceController::applyBlur(const WindowAppearanceState& state)
{
    if (impl_ == nullptr || state.opacityPercent >= 100.0f)
        return;

#if JUCE_MAC
    impl_->setWindowBlur(state.macosBlurRadius);
#elif JUCE_WINDOWS
    impl_->setAcrylicEnabled(state.windowsAcrylic);
#endif
}

void WindowAppearanceController::restoreOpaque()
{
    if (impl_)
        impl_->setWindowOpaque(true);
}

float WindowAppearanceController::defaultOpacity()
{
#if JUCE_LINUX
    return 100.0f;  // Linux: opaque by default (B5 decision #10)
#else
    return 70.0f;   // macOS/Windows: 70% default
#endif
}

juce::String WindowAppearanceController::unsupportedReason() const
{
    if (impl_)
        return impl_->getUnsupportedReason();
    return {};
}

} // namespace hathor::ui
