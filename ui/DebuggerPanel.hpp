// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * DebuggerPanel.hpp — L-6: native C++ debugger UI panel.
 *
 * A dockable panel (bottom-docked within EditorArea, like TerminalPanel)
 * that drives the platform's native debugger through DebugSession:
 *
 *   - Launch/stop a debug session (LLDB on macOS/Linux, GDB on Linux)
 *   - Breakpoints: set at file:line, list, delete
 *   - Continue / Pause (interrupt) / Step Over / Step Into / Step Out
 *   - Call-stack view
 *   - Locals / data inspection view
 *   - Watches (expression evaluation)
 *   - Raw debugger output view (last N lines)
 *
 * If the platform has no supported native debugger (e.g. Windows), the panel
 * states this explicitly and disables the controls — it never pretends to be
 * a debugger.
 *
 * Threading / audio-safety:
 *   - All DebugSession calls happen on the JUCE message thread and are
 *     non-blocking (one line written to the debugger's stdin).
 *   - pollResults() runs from a 30 Hz timer on the message thread.
 *   - The audio thread never touches this panel or DebugSession.
 *
 * AI restriction (L-6 §AI RESTRICTION): the traditional debugger remains a
 * deterministic IDE tool — no AI repair affordance.
 *
 * Requirement references: L-6 §Native/C++ Debugging
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "HathorLookAndFeel.hpp"
#include "DebugSession.hpp"

namespace hathor::ui {

/**
 * DebuggerPanel — native JUCE debugger UI.
 */
class DebuggerPanel : public juce::Component,
                      private juce::Timer
{
public:
    static constexpr int kPanelHeight = 300;

    DebuggerPanel();
    ~DebuggerPanel() override;

    // -----------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;
    void setVisible(bool visible) override;

    // -----------------------------------------------------------------------
    // Callbacks — installed by DebugPanel / EditorArea
    // -----------------------------------------------------------------------
    std::function<void()> onClosePanel;

    /// True if a debug session is currently running.
    bool isSessionRunning() const noexcept { return session_.isRunning(); }

private:
    // -----------------------------------------------------------------------
    // Actions (button handlers)
    // -----------------------------------------------------------------------
    void launchSession();
    void stopSession();
    void addBreakpoint();
    void deleteBreakpoint();
    void addWatch();
    void refreshViews();

    // -----------------------------------------------------------------------
    // DebugSession callbacks
    // -----------------------------------------------------------------------
    void installSessionCallbacks();

    // -----------------------------------------------------------------------
    // juce::Timer — poll debugger output on the message thread
    // -----------------------------------------------------------------------
    static constexpr int kPollIntervalHz = 30;
    void timerCallback() override;

    void setStatus(const juce::String& status, bool isError = false);
    void appendOutput(const std::string& line);
    void updateControlStates();

    // -----------------------------------------------------------------------
    // Debug session
    // -----------------------------------------------------------------------
    DebugSession session_;

    // Cached async results (message thread only)
    std::vector<DebugSession::StackFrame>  stackFrames_;
    std::vector<DebugSession::WatchValue>  locals_;
    std::vector<DebugSession::WatchValue>  watches_;
    std::vector<DebugSession::Breakpoint>  breakpoints_;
    std::deque<std::string>                outputLines_;

    // -----------------------------------------------------------------------
    // Child components
    // -----------------------------------------------------------------------
    std::unique_ptr<juce::Label>       statusLabel_;
    std::unique_ptr<juce::Label>       stackTitle_;
    std::unique_ptr<juce::Label>       localsTitle_;
    std::unique_ptr<juce::Label>       watchesTitle_;
    std::unique_ptr<juce::TextEditor>  exeField_;
    std::unique_ptr<juce::TextEditor>  argsField_;
    std::unique_ptr<juce::TextEditor>  fileField_;
    std::unique_ptr<juce::TextEditor>  lineField_;
    std::unique_ptr<juce::TextEditor>  watchField_;
    std::unique_ptr<juce::TextEditor>  stackView_;
    std::unique_ptr<juce::TextEditor>  localsView_;
    std::unique_ptr<juce::TextEditor>  watchesView_;
    std::unique_ptr<juce::TextEditor>  outputView_;
    std::unique_ptr<juce::TextButton>  launchBtn_;
    std::unique_ptr<juce::TextButton>  stopBtn_;
    std::unique_ptr<juce::TextButton>  addBpBtn_;
    std::unique_ptr<juce::TextButton>  delBpBtn_;
    std::unique_ptr<juce::TextButton>  continueBtn_;
    std::unique_ptr<juce::TextButton>  pauseBtn_;
    std::unique_ptr<juce::TextButton>  stepOverBtn_;
    std::unique_ptr<juce::TextButton>  stepIntoBtn_;
    std::unique_ptr<juce::TextButton>  stepOutBtn_;
    std::unique_ptr<juce::TextButton>  watchBtn_;
    std::unique_ptr<juce::TextButton>  closeBtn_;

    juce::String statusText_;
    bool statusIsError_ = false;
    bool viewsDirty_ = false;   ///< stack/locals/watches changed since last render
    bool outputDirty_ = false;  ///< raw output changed since last render

    // Layout constants
    static constexpr int kHeaderHeight  = 26;
    static constexpr int kFieldHeight   = 24;
    static constexpr int kMargin        = 6;
    static constexpr int kMaxOutputLines = 200;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DebuggerPanel)
};

} // namespace hathor::ui
