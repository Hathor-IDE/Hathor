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
 *   - AcpAgentSession teardown does not touch AudioEngine state.
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

ChatSidebar::ChatSidebar(AudioEngine& /*audio*/,
                         hathor::control::ControlInterface& ci)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // -----------------------------------------------------------------------
    // Tab bar area (B6)
    // -----------------------------------------------------------------------
    addAndMakeVisible(tabBarArea_);
    tabBarArea_.setTopLeftPosition(juce::Point<int>());

    // -----------------------------------------------------------------------
    // Tab scroll buttons (shown when tabs overflow horizontally)
    // -----------------------------------------------------------------------
    scrollLeftBtn_.setButtonText("\xe2\x86\x90");   // ←
    scrollRightBtn_.setButtonText("\xe2\x86\x92");  // →
    scrollLeftBtn_.setColour(juce::TextButton::buttonColourId, palette.surfaceHigh);
    scrollLeftBtn_.setColour(juce::TextButton::textColourOffId, palette.textPrimary);
    scrollRightBtn_.setColour(juce::TextButton::buttonColourId, palette.surfaceHigh);
    scrollRightBtn_.setColour(juce::TextButton::textColourOffId, palette.textPrimary);
    scrollLeftBtn_.setVisible(false);
    scrollRightBtn_.setVisible(false);
    scrollLeftBtn_.onClick = [this]()
    {
        tabScrollOffset_ = std::max(0, tabScrollOffset_ - 120);
        updateTabButtons();
        resized();
    };
    scrollRightBtn_.onClick = [this]()
    {
        constexpr int kTabBtnW = 120;
        constexpr int kScrollBtnW = 24;
        const int totalW = static_cast<int>(tabButtons_.size()) * kTabBtnW;
        const int visibleW = getWidth() - kScrollBtnW * 2 - 4;
        tabScrollOffset_ = std::min(tabScrollOffset_ + 120,
                                    std::max(0, totalW - visibleW));
        updateTabButtons();
        resized();
    };
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
    // Stop all sessions before destroying components (C2 §11 — clean teardown).
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
    // Owned by ChatSidebar via OwnedArray.
    auto* session = new AcpAgentSession();

    // Wire MCP handler if one has been installed.
    if (mcpCommandHandler_)
        session->setMcpCommandHandler(mcpCommandHandler_);

    sessions_.add(session);

    // Create a new ChatThread UI component.
    // Each thread has its own message history, input, reconnect banner, and
    // connection state — completely independent of other threads (C2 §7).
    auto* thread = new ChatThread();
    threads_.add(thread);
    addChildComponent(thread);

    // Wire the session to this thread.
    thread->setSession(*session, agentExePath, projectDir, mcpPath);

    // Set a default tab title.
    thread->setTabTitle(juce::String("Thread ") + juce::String(threads_.size()));

    // Start the session.
    session->start(agentExePath, projectDir, mcpPath);

    // Build/update tab buttons.
    buildTabButtons();

    // Switch to the new thread immediately.
    setActiveThread(static_cast<int>(threads_.size()) - 1);

    return static_cast<int>(threads_.size()) - 1;
}

void ChatSidebar::setMcpCommandHandler(AcpAgentSession::McpCommandHandlerFn handler)
{
    // Store for future threads.
    mcpCommandHandler_ = handler;

    // Install on all existing sessions.
    for (auto* session : sessions_)
    {
        if (session)
            session->setMcpCommandHandler(handler);
    }
}

void ChatSidebar::restartAllThreads(const std::string& agentExePath,
                                    const std::string& projectDir,
                                    const std::string& hathorMcpPath)
{
    // Stop all sessions (C2 §11 — clean teardown, no lingering callbacks).
    for (auto* session : sessions_)
    {
        if (session)
            session->stop();
    }

    if (agentExePath.empty())
    {
        // No agent configured — mark all threads as disconnected.
        for (auto* thread : threads_)
        {
            if (thread)
                thread->onDisconnected();
        }
        return;
    }

    // Restart each session with the new path.
    for (int i = 0; i < static_cast<int>(threads_.size()); ++i)
    {
        auto* session = sessions_[i];
        auto* thread  = threads_[i];
        if (session && thread)
        {
            // Wire callbacks on the existing thread.
            thread->setSession(*session, agentExePath, projectDir, hathorMcpPath);
            session->start(agentExePath, projectDir, hathorMcpPath);
        }
    }
}

void ChatSidebar::setActiveThread(int threadIndex)
{
    if (threadIndex < 0 || threadIndex >= static_cast<int>(threads_.size()))
        return;

    // Hide the old active thread.
    if (activeThreadIndex_ >= 0 &&
        activeThreadIndex_ < static_cast<int>(threads_.size()))
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
        newThread->resized();
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
// Tab bar UI helpers (B6)
// ---------------------------------------------------------------------------

void ChatSidebar::buildTabButtons()
{
    // Clear existing tab buttons.
    for (auto* btn : tabButtons_)
        tabBarArea_.removeChildComponent(btn);
    tabButtons_.clear();

    // Create a tab button for each thread.
    for (int i = 0; i < static_cast<int>(threads_.size()); ++i)
    {
        auto* btn = new juce::TextButton();
        const juce::String title = threads_[i]->tabTitle();
        btn->setButtonText(title.isEmpty()
                               ? juce::String("Thread ") + juce::String(i + 1)
                               : title);
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
    // Layout all tab buttons at their natural width.
    constexpr int kTabBtnW = 120;
    int x = tabScrollOffset_;
    int idx = 0;
    for (auto* btn : tabButtons_)
    {
        btn->setBounds(x, 2, kTabBtnW, kTabAreaH - 6);
        btn->setToggleState(activeThreadIndex_ == idx, juce::dontSendNotification);
        x += kTabBtnW;
        ++idx;
    }
}

void ChatSidebar::onTabClicked(int index)
{
    if (updatingTabs_)
        return;
    setActiveThread(index);
}

void ChatSidebar::closeTab(int /*index*/)
{
    // Not implemented for v1 — tabs are not closable yet (C2 §8: switching
    // away does NOT erase state; closing is a separate feature).
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
    // Simple horizontal layout — scroll buttons shown if overflow.
    constexpr int kTabBtnW = 120;
    constexpr int kScrollBtnW = 24;

    auto tabArea = tabBarArea_.getLocalBounds().reduced(2, 2);
    int x = tabScrollOffset_;
    int idx = 0;
    for (auto* btn : tabButtons_)
    {
        btn->setBounds(x, tabArea.getY(), kTabBtnW, tabArea.getHeight() - 2);
        btn->setToggleState(activeThreadIndex_ == idx, juce::dontSendNotification);
        x += kTabBtnW;
        ++idx;
    }

    // Scroll buttons — shown if total tab width exceeds viewport.
    const int totalTabWidth = static_cast<int>(tabButtons_.size()) * kTabBtnW;
    const bool showScroll = totalTabWidth > tabArea.getWidth();
    scrollLeftBtn_.setVisible(showScroll && tabScrollOffset_ > 0);
    scrollRightBtn_.setVisible(showScroll &&
                               (tabScrollOffset_ + tabArea.getWidth() < totalTabWidth));

    if (showScroll)
    {
        scrollLeftBtn_.setBounds(tabArea.getX(), tabArea.getY(),
                                 kScrollBtnW, tabArea.getHeight() - 2);
        scrollRightBtn_.setBounds(tabArea.getRight() - kScrollBtnW,
                                  tabArea.getY(), kScrollBtnW, tabArea.getHeight() - 2);
    }

    // Position all thread components — only the active one is visible.
    // The content area is below the tab bar, above the slider panel.
    auto contentBounds = b;
    contentBounds.removeFromBottom(kSliderH);  // reserve space for slider

    for (int i = 0; i < static_cast<int>(threads_.size()); ++i)
    {
        if (auto* t = threads_[i])
        {
            t->setBounds(contentBounds);
            t->setVisible(i == activeThreadIndex_);
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
    g.setColour(palette.surfaceHighest);
    g.drawHorizontalLine(kTabAreaH, 0.0f, static_cast<float>(getWidth()));

    // Separator above slider panel.
    const int sepY = getHeight() - kSliderH - 1;
    if (sepY > 0)
    {
        g.drawHorizontalLine(sepY, 0.0f, static_cast<float>(getWidth()));
    }
}

} // namespace hathor::ui
