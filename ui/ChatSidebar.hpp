// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ChatSidebar.hpp — AI chat sidebar with thread tabs and sliders.
 *
 * Per decision #3 (PROGRAM.md §2) and B6: one AcpAgentSession per chat
 * thread tab. Per C2: each thread tracks its own connection state
 * (Connected / Disconnected / Reconnecting) and has its own reconnect
 * action. A reconnect on one thread does NOT affect other threads.
 *
 * Component hierarchy (320 px wide, full window height):
 *
 *   ┌─────────────────────────────────┐
 *   │  [Tab bar: Thread 1 | Thread 2] │  ← one tab button per thread
 *   ├─────────────────────────────────┤
 *   │                                 │
 *   │  Active ChatThread:             │
 *   │  ├─ [Status label]             │  ← error/info
 *   │  ├─ [Reconnect banner]          │  ← per-thread, C2
 *   │  ├─ [Permission prompt]         │  ← per-thread
 *   │  ├─ MessageHistoryView          │  ← per-thread bubbles
 *   │  ├─ ChatInputField              │  ← per-thread
 *   │  └─ ASCII art header            │
 *   ├─────────────────────────────────┤
 *   │  SliderPanel (BPM + Gain)       │  ← shared, always visible
 *   └─────────────────────────────────┘
 *
 * Thread model:
 *   - All public methods run on the JUCE message thread.
 *   - AcpAgentSession callbacks are marshalled to the message thread
 *     before reaching ChatThread — no additional synchronisation needed.
 *
 * Audio isolation (Req 32.9):
 *   - No AudioEngine members; SliderPanel holds the only CI reference.
 *   - AcpAgentSession teardown does not touch AudioEngine state.
 *
 * Requirements: 25.1, 25.2, 25.3, 25.5, 25.6, 26.1, 32.1, 32.3, 32.5,
 *               32.6, 32.8, 32.9, B6, C2
 */

// This guard is checked in MainWindow.cpp to suppress the stub ChatSidebar.
#define HATHOR_CHAT_SIDEBAR_DEFINED

#include <memory>
#include <string>

#include <juce_gui_basics/juce_gui_basics.h>

// App / control
#include "../app/AudioEngine.hpp"
#include "../control/ControlInterface.hpp"

// Sibling UI components
#include "AcpAgentSession.hpp"
#include "SliderPanel.hpp"
#include "ChatThread.hpp"

namespace hathor::ui {

/**
 * ChatSidebar
 *
 * The AI chat sidebar: 320 px wide right-hand panel.
 *
 * Manages multiple chat threads (B6). Each ChatThread owns its own
 * AcpAgentSession (one subprocess per tab, decision #3) and its own
 * thread-scoped connection state (C2 §7).
 *
 * Lifecycle:
 *   1. Construct (MainWindow constructor)
 *   2. Call addThread() to create a new chat thread tab
 *   3. Call setActiveThread() to switch between threads
 *
 * Disconnect / reconnect (C2):
 *   - Each ChatThread independently tracks its own connection state.
 *   - A reconnect on Thread A calls Thread A's session_->restart() only.
 *   - Thread B remains unaffected.
 *
 * Audio independence (Req 32.9):
 *   - No AudioEngine members; SliderPanel holds the only CI reference.
 *   - AcpAgentSession teardown does not touch AudioEngine state.
 */
class ChatSidebar : public juce::Component
{
public:
    /**
     * @param audio  AudioEngine reference (used only to pass to SliderPanel).
     * @param ci     ControlInterface for slider dispatches.
     */
    ChatSidebar(AudioEngine& audio,
                hathor::control::ControlInterface& ci);

    ~ChatSidebar() override;

    // -----------------------------------------------------------------------
    // Thread management (B6)
    // -----------------------------------------------------------------------

    /**
     * Create a new chat thread with its own AcpAgentSession.
     *
     * @param agentExePath  Path to the agent executable.
     * @param projectDir    Project directory (cwd for session/new).
     * @param mcpPath       Path to hathor-mcp.
     * @return The index of the new thread.
     */
    int addThread(const std::string& agentExePath,
                  const std::string& projectDir,
                  const std::string& mcpPath);

    /**
     * Switch to a different chat thread (tab).
     * Background threads retain their connection state (C2 §8).
     */
    void setActiveThread(int threadIndex);

    /** Returns the number of chat threads. */
    int threadCount() const noexcept { return static_cast<int>(threads_.size()); }

    /** Returns the index of the currently active thread, or -1 if none. */
    int activeThreadIndex() const noexcept { return activeThreadIndex_; }

    /**
     * Returns the active ChatThread, or nullptr if there are no threads.
     */
    ChatThread* activeThread() const noexcept;

    /**
     * Access a thread by index. Returns nullptr if out of range.
     */
    ChatThread* threadAt(int index) const noexcept
    {
        if (index < 0 || index >= static_cast<int>(threads_.size()))
            return nullptr;
        return threads_[index];
    }

    /**
     * Install the MCP command handler on all existing and future sessions.
     * Called by MainWindow to wire hathor-mcp socket commands to ControlInterface.
     *
     * @param handler  Invoked on the socket accept-loop worker thread for each
     *                 MCP command line, with a respond callback.
     */
    void setMcpCommandHandler(AcpAgentSession::McpCommandHandlerFn handler);

    /**
     * Restart all chat threads with a new agent executable path (A2).
     * Stops all sessions, then starts a new session for each thread with
     * the updated path. If agentExePath is empty, sessions are stopped
     * without restarting.
     */
    void restartAllThreads(const std::string& agentExePath,
                           const std::string& projectDir,
                           const std::string& hathorMcpPath);

    // -----------------------------------------------------------------------
    // SliderPanel access (for UITimer bidirectional sync)
    // -----------------------------------------------------------------------
    SliderPanel& getSliderPanel() noexcept { return *sliderPanel_; }

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    // -----------------------------------------------------------------------
    // Layout constants
    // -----------------------------------------------------------------------
    static constexpr int kTabAreaH    = 32;  ///< Tab bar height
    static constexpr int kSliderH     = 80;  ///< SliderPanel height

    // -----------------------------------------------------------------------
    // Tab bar UI helpers
    // -----------------------------------------------------------------------
    void buildTabButtons();
    void updateTabButtons();
    void onTabClicked(int index);
    void closeTab(int index);

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    /** All chat threads — each owns its own AcpAgentSession (decision #3, B6). */
    juce::OwnedArray<ChatThread> threads_;

    /** Each thread's AcpAgentSession is owned by the ChatSidebar (one per tab). */
    juce::OwnedArray<AcpAgentSession> sessions_;

    /** Index of the currently visible/active thread. */
    int activeThreadIndex_ = -1;

    /** Tab bar buttons — one per thread. */
    juce::OwnedArray<juce::TextButton> tabButtons_;

    /** Container component for the tab bar. */
    juce::Component tabBarArea_;

    /** Scroll buttons for tab bar when there are too many tabs. */
    juce::TextButton scrollLeftBtn_;
    juce::TextButton scrollRightBtn_;
    int tabScrollOffset_ = 0;

    /** Flag to suppress tab button callbacks during programmatic updates. */
    bool updatingTabs_ = false;

    /** MCP command handler installed on each session (H0). */
    AcpAgentSession::McpCommandHandlerFn mcpCommandHandler_;

    // SliderPanel (shared across all threads — BPM/gain are global)
    std::unique_ptr<SliderPanel> sliderPanel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChatSidebar)
};

} // namespace hathor::ui
