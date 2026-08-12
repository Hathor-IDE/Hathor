// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * DebugPanel.cpp — L-6: combined Debug & Inspect panel implementation.
 *
 * Requirement references: L-6 §Native/C++ Debugging, L-6 §Hathor Runtime
 * Inspection, L-6 §Workspace Integration
 */

#include "DebugPanel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DebugPanel::DebugPanel(AudioEngineFacade& audio,
                       hathor::control::DiagnosticRegistry* registry)
    : runtimePane_(audio, registry)
{
    debuggerPane_.onClosePanel = [this]() {
        if (onClosePanel)
            onClosePanel();
    };
    runtimePane_.onClosePanel = [this]() {
        if (onClosePanel)
            onClosePanel();
    };
    runtimePane_.onOpenProblems = [this]() {
        if (onOpenProblems)
            onOpenProblems();
    };

    addAndMakeVisible(debuggerPane_);
    addAndMakeVisible(runtimePane_);

    setActiveTab(activeTab_);
}

// ---------------------------------------------------------------------------
// juce::Component
// ---------------------------------------------------------------------------

void DebugPanel::resized()
{
    auto b = getLocalBounds();
    layoutTabButtons();

    // Tab bar strip
    b.removeFromTop(kTabBarHeight);

    // Active pane fills the rest
    if (activeTab_ == Tab::Debugger)
    {
        runtimePane_.setVisible(false);
        debuggerPane_.setVisible(true);
        debuggerPane_.setBounds(b);
    }
    else
    {
        debuggerPane_.setVisible(false);
        runtimePane_.setVisible(true);
        runtimePane_.setBounds(b);
    }
}

void DebugPanel::paint(juce::Graphics& g)
{
    const auto& pal = HathorLookAndFeel::fromComponent(*this).getPalette();
    g.fillAll(pal.surface);

    // Tab bar background + bottom rule
    g.setColour(pal.surfaceLow);
    g.fillRect(juce::Rectangle<int>(0, 0, getWidth(), kTabBarHeight));
    g.setColour(pal.surfaceHighest);
    g.fillRect(juce::Rectangle<int>(0, kTabBarHeight - 1, getWidth(), 1));

    for (const auto& btn : tabButtons_)
    {
        const bool active = btn.active;
        g.setColour(active ? pal.accent : pal.textMuted);
        g.fillRect(juce::Rectangle<int>(btn.bounds.getX(), btn.bounds.getBottom() - 2,
                                        btn.bounds.getWidth(), 2));
        g.setColour(active ? pal.textPrimary : pal.textSecondary);
        g.setFont(HathorLookAndFeel::fontMedium(11.0f));
        g.drawText(btn.label, btn.bounds, juce::Justification::centred, false);
    }
}

void DebugPanel::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
    if (visible)
    {
        // Ensure the active pane refreshes immediately when the panel opens.
        runtimePane_.refreshNow();
    }
}

// ---------------------------------------------------------------------------
// Tab selection
// ---------------------------------------------------------------------------

void DebugPanel::showRuntimeTab()
{
    setActiveTab(Tab::Runtime);
    resized();
}

void DebugPanel::showDebuggerTab()
{
    setActiveTab(Tab::Debugger);
    resized();
}

void DebugPanel::setActiveTab(Tab tab)
{
    activeTab_ = tab;
    for (auto& btn : tabButtons_)
        btn.active = (btn.tab == tab);
    repaint();
}

void DebugPanel::layoutTabButtons()
{
    int x = 0;
    for (auto& btn : tabButtons_)
    {
        btn.bounds = { x, 0, kTabWidth, kTabBarHeight };
        x += kTabWidth;
    }
}

} // namespace hathor::ui
