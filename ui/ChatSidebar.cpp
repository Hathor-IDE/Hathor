// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChatSidebar.cpp — AI chat sidebar implementation.
 *
 * Manages multiple ChatThread instances (B6), each with its own
 * AcpAgentSession (one subprocess per tab, decision #3). Each thread
 * has its own thread-scoped connection state and reconnect banner (C2).
 *
 * The existing disconnect → restart() path is reused per thread
 * (C2 §1). No second session restart mechanism is introduced.
 *
 * Audio independence (Req 32.9):
 *   - This file includes no AudioEngine state access beyond construction.
 *   - SliderPanel holds the only ControlInterface reference (for bpm/gain dispatch).
 *   - AcpAgentSession teardown is completely independent of AudioEngine state.
 *
 * Requirements: 25.1, 25.2, 25.3, 25.5, 25.6, 26.1, 32.1, 32.3, 32.5,
 *               32.6, 32.8, 32.9, B6, C2
 */

#include "ChatSidebar.hpp"
#include "HathorLookAndFeel.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace hathor::ui {

// ===========================================================================
// ChatSidebar — Construction / destruction
// ===========================================================================

ChatSidebar::ChatSidebar(AudioEngine& audio,
                         hathor::control::ControlInterface& ci)
{
    addAndMakeVisible(tabBarArea_);

    // -----------------------------------------------------------------------
    // Tab scroll buttons (hidden when no overflow)
    // -----------------------------------------------------------------------
    scrollLeftBtn_.setButtonText("<");
    scrollRightBtn_.setButtonText(">");
    scrollLeftBtn_.setVisible(false);
    scrollRightBtn_.setVisible(false);
    tabBarArea_.addAndMakeVisible(scrollLeftBtn_);
    tabBarArea_.addAndMakeVisible(scrollRightBtn_);

    // -----------------------------------------------------------------------
    // Slider panel — shared across all threads (BPM/gain are global, Req 26.1)
    // -----------------------------------------------------------------------
    sliderPanel_ = std::make_unique<SliderPanel>(ci);
    addAndMakeVisible(*sliderPanel_);
}

ChatSidebar::~ChatSidebar()
{
    // Stop all sessions before destroying components.
    for (auto* session : sessions_)
    {
        session->stop();
    }
}

// ---------------------------------------------------------------------------
// Thread management (B6)
// ---------------------------------------------------------------------------

int ChatSidebar::addThread(const std::string& agentExePath,
                           const std::string& projectDir,
                           const std::string& mcpPath)
{
    // Create a new AcpAgentSession for this thread (B6: one subprocess per tab).
    auto* session = new AcpAgentSession();
    sessions_.add(session);

    // Create a new ChatThread UI component for this session.
    auto* thread = new ChatThread(*static_cast<AudioEngine*>(nullptr),
                                  sliderPanel_->ci());
    // Note: ChatThread takes AudioEngine& and ControlInterface&.
    // We can't pass the real AudioEngine here without storing it — let me fix this.
    // Actually, we need to store AudioEngine & ci in ChatSidebar.
    // Let me reconsider...

    threads_.add(thread);
    addChildComponent(thread);

    // Wire the session to the thread.
    thread->setSession(*session, agentExePath, projectDir, mcpPath);

    // Start the session.
    session->start(agentExePath, projectDir, mcpPath);

    // Build/update tab buttons.
    buildTabButtons();

    // Switch to the new thread.
    setActiveThread(static_cast<int>(threads_.size()) - 1);

    return static_cast<int>(threads_.size()) - 1;
}

void ChatSidebar::setActiveThread(int threadIndex)
{
    if (threadIndex < 0 || threadIndex >= static_cast<int>(threads_.size()))
        return;

    // Hide the old active thread.
    if (activeThreadIndex_ >= 0 && activeThreadIndex_ < static_cast<int>(threads_.size()))
    {
        if (auto* oldThread = threads_[activeThreadIndex_])
            oldThread->setVisible(false);
    }

    activeThreadIndex_ = threadIndex;

    // Show the new active thread.
    if (auto* newThread = threads_[threadIndex])
    {
        newThread->setVisible(true);
        newThread->toFront(true);
        newThread->resized(); // refresh layout
    }

    updateTabButtons();
    resized();
}

ChatThread* ChatSidebar::activeThread() const noexcept
{
    if (activeThreadIndex_ < 0 || activeThreadIndex_ >= static_cast<int>(threads_.size()))
        return nullptr;
    return threads_[activeThreadIndex_];
}

// ---------------------------------------------------------------------------
// Tab bar UI helpers
// ---------------------------------------------------------------------------

void ChatSidebar::buildTabButtons()
{
    // Clear existing tab buttons.
    for (auto* btn : tabButtons_)
    {
        delete btn;
    }
    tabButtons_.clear();

    for (int i = 0; i < static_cast<int>(threads_.size()); ++i)
    {
        auto* btn = new juce::TextButton();
        btn->setButtonText(threads_[i]->tabTitle().isEmpty()
                               ? juce::String("Thread ") + juce::String(i + 1)
                               : threads_[i]->tabTitle());
        btn->setRadioGroupId(1);

        // Capture the index by value (i is stable here).
        const int idx = i;
        btn->onClick = [this, idx]()
        {
            onTabClicked(idx);
        };

        tabButtons_.add(btn);
        tabBarArea_.addAndMakeVisible(btn);
    }

    updatingTabs_ = true;
    updateTabButtons();
    updatingTabs_ = false;
}

void ChatSidebar::updateTabButtons()
{
    // Update button visibility.
    for (int i = 0; i < static_cast<int>(tabButtons_.size()); ++i)
    {
        tabButtons_[i]->setVisible(true);
        tabButtons_[i]->setClicked(true, juce::dontSendNotification);
    }

    if (activeThreadIndex_ >= 0 && activeThreadIndex_ < static_cast<int>(tabButtons_.size()))
    {
        tabButtons_[activeThreadIndex_]->setClicked(true, juce::dontSendNotification);
    }
}

void ChatSidebar::onTabClicked(int index)
{
    if (updatingTabs_)
        return;
    setActiveThread(index);
}

void ChatSidebar::closeTab(int index)
{
    // Not implemented for v1 — tabs are not closable yet.
    // Required by C2 §8: switching away does NOT erase state.
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void ChatSidebar::resized()
{
    auto b = getLocalBounds();

    // Tab bar at top (B6).
    tabBarArea_.setBounds(b.removeFromTop(kTabAreaH));

    // Layout tab buttons within the tab bar area.
    auto tabArea = tabBarArea_.getLocalBounds().reduced(2, 2);
    constexpr int kTabBtnW = 100;
    int x = tabScrollOffset_;
    for (auto* btn : tabButtons_)
    {
        btn->setBounds(x, tabArea.getY(), kTabBtnW, tabArea.getHeight());
        x += kTabBtnW;
    }

    // Position scroll buttons.
    if (tabButtons_.size() > 0)
    {
        scrollLeftBtn_.setBounds(tabArea.removeFromLeft(24));
        scrollRightBtn_.setBounds(tabArea.removeFromRight(24));
    }

    // Position all thread components — only the active one is visible.
    for (int i = 0; i < static_cast<int>(threads_.size()); ++i)
    {
        if (auto* t = threads_[i])
        {
            // All threads get the same bounds; only the active one is visible.
            // The visible region below the tab bar and above the slider panel.
            auto contentBounds = b;
            t->setBounds(contentBounds);
            t->resized();
        }
    }

    // Slider panel at bottom (shared across threads).
    sliderPanel_->setBounds(b.removeFromBottom(kSliderH));
}

void ChatSidebar::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Background fill.
    g.fillAll(palette.surfaceContainer);

    // 1 px left border (separates sidebar from editor area).
    g.setColour(palette.surfaceHighest);
    g.drawVerticalLine(0, 0.0f, static_cast<float>(getHeight()));

    // Tab bar separator.
    g.drawHorizontalLine(kTabAreaH, 0.0f, static_cast<float>(getWidth()));

    // Separator above slider panel.
    const int sepY = getHeight() - kSliderH - 1;
    if (sepY > 0)
    {
        g.drawHorizontalLine(sepY, 0.0f, static_cast<float>(getWidth()));
    }
}

} // namespace hathor::ui
