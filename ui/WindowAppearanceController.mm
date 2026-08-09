// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WindowAppearanceController.mm — platform-isolated implementation for
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

// Concrete impl typedef for this platform
#if JUCE_MAC
class MacPlatformImpl;
#elif JUCE_WINDOWS
class WindowsPlatformImpl;
#elif JUCE_LINUX
class LinuxPlatformImpl;
#else
class FallbackPlatformImpl;
#endif

// ===========================================================================
// PlatformImpl implementations (platform-specific)
// ===========================================================================

#if JUCE_MAC
// ---------------------------------------------------------------------------
// macOS: full-window alpha + NSVisualEffectView blur
// ---------------------------------------------------------------------------

class MacPlatformImpl : public WindowAppearanceController::PlatformImplBase
{
public:
    explicit MacPlatformImpl(juce::TopLevelWindow& w) : window_(w) {}

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
    juce::TopLevelWindow& window_;

    /**
     * Install an NSVisualEffectView as the window's content view's background
     * to produce a true background blur (blurs desktop/pixels behind the
     * window, NOT a filter over Hathor's own UI).
     *
     * The blur radius maps to NSVisualEffectView's material + properties:
     *   radius 0   → remove the effect view (solid background)
     *   radius >0  → NSVisualEffectMaterialHUDWindow provides backdrop blur
     *                 with the material chosen reflecting blur strength
     */
    static void applyMacOSVisualEffect(juce::TopLevelWindow& window, int radius)
    {
        NSView* contentView = static_cast<NSView*>(window.getWindowHandle());
        if (contentView == nil)
            return;

        if (radius <= 0)
        {
            removeVisualEffectViews(contentView);
            return;
        }

        removeVisualEffectViews(contentView);

        NSVisualEffectView* effectView =
            [[NSVisualEffectView alloc] initWithFrame:[contentView bounds]];
        effectView.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        effectView.state = NSVisualEffectStateActive;
        effectView.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        // Map radius (0-100) to material variants with different blur strengths:
        //   - HUDWindow: moderate blur (HUD panels)
        //   - Menu: lighter blur (menus)
        //   - Titlebar: subtle blur (title bars)
        // Higher radius → stronger material → more pronounced blur.
        if (radius >= 70)
            effectView.material = NSVisualEffectMaterialHUDWindow;
        else if (radius >= 40)
            effectView.material = NSVisualEffectMaterialMenu;
        else
            effectView.material = NSVisualEffectMaterialTitlebar;

        [contentView addSubview:effectView positioned:NSWindowBelow relativeTo:nil];
        [effectView release];
    }

    static void removeVisualEffectViews(NSView* contentView)
    {
        if (contentView == nil)
            return;

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

class WindowsPlatformImpl : public WindowAppearanceController::PlatformImplBase
{
public:
    explicit WindowsPlatformImpl(juce::TopLevelWindow& w) : window_(w) {}

    PlatformCapabilities detectCaps() const override
    {
        PlatformCapabilities caps;
        caps.transparencySupported = true;   // WS_EX_LAYERED is reliable on Windows
        caps.blurSupported          = true;   // DWM Acrylic available on Win10+
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
    juce::TopLevelWindow& window_;

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

        struct AccentPolicy
        {
            int AccentState;
            int AccentFlags;
            int GradientColor;
            int AnimationId;
        };

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
                policy.GradientColor = 0;
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

class LinuxPlatformImpl : public WindowAppearanceController::PlatformImplBase
{
public:
    explicit LinuxPlatformImpl(juce::TopLevelWindow& w)
        : window_(w), lastCaps_{}
    {}

    PlatformCapabilities detectCaps() const override
    {
        PlatformCapabilities caps;
        caps.transparencySupported = detectCompositor();
        caps.blurSupported          = caps.transparencySupported && supportsBlurProtocol();
        lastCaps_ = caps;
        return caps;
    }

    void setWindowOpacity(float alpha) override
    {
        if (lastCaps_.transparencySupported)
        {
            window_.setAlpha(alpha);
            window_.setOpaque(false);
        }
        else
        {
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

    void setWindowBlur(int /*radius*/) override {}

    void setAcrylicEnabled(bool /*enabled*/) override {}

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
    juce::TopLevelWindow& window_;
    mutable PlatformCapabilities lastCaps_{};

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

    static bool detectCompositor()
    {
        const char* sessionType = std::getenv("XDG_SESSION_TYPE");
        if (sessionType != nullptr &&
            juce::String(sessionType).containsIgnoreCase("wayland"))
            return true;

        if (isX11CompositorRunning())
            return true;

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

    static bool supportsBlurProtocol()
    {
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

#if !JUCE_MAC && !JUCE_WINDOWS && !JUCE_LINUX
class FallbackPlatformImpl : public WindowAppearanceController::PlatformImplBase
{
public:
    explicit FallbackPlatformImpl(juce::TopLevelWindow& w) : window_(w) {}

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

private:
    juce::TopLevelWindow& window_;
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
        restoreOpaque();
        return;
    }

    const float alpha = juce::jlimit(0.01f, 1.0f, state.opacityPercent / 100.0f);
    if (impl_)
        impl_->setWindowOpacity(alpha);

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
