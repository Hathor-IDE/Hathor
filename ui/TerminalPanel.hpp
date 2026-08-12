// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * TerminalPanel.hpp — L-4: native JUCE integrated terminal panel.
 *
 * A dockable panel (bottom-docked in EditorArea, like ProblemsPanel) that
 * provides a simple developer terminal:
 *
 *   - Read-only monospace output display (juce::TextEditor)
 *   - Single-line command input (juce::TextEditor) with history (Up/Down)
 *   - Task quick-launch buttons (build, test, check, etc.)
 *   - Process state indicator (Running / Done / Error)
 *   - Cancel button for running processes
 *   - Streaming stdout/stderr via a polling timer
 *
 * Threading boundary:
 *   - TerminalProcess runs a worker thread for all I/O (never the message
 *     or audio thread).
 *   - This component polls TerminalProcess::drainOutput() at 30 Hz via a
 *     juce::Timer on the JUCE message thread, appending bytes to the
 *     output editor.
 *   - The audio thread is never touched by this component.
 *
 * The panel is intentionally simple — an advanced/developer escape hatch,
 * not a full PTY terminal emulator. It does not interpret ANSI escape
 * sequences beyond basic line buffering.
 *
 * Requirement references: L-4 §Architecture, L-4 Acceptance
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "HathorLookAndFeel.hpp"
#include "TerminalProcess.hpp"
#include "TaskRunner.hpp"

namespace hathor::ui {

class TerminalPanel : public juce::Component,
                      private juce::Timer,
                      private juce::TextEditor::Listener
{
public:
    static constexpr int kPanelHeight = 240;

    explicit TerminalPanel(const std::string& projectDir = {});
    ~TerminalPanel() override;

    TerminalPanel(const TerminalPanel&) = delete;
    TerminalPanel& operator=(const TerminalPanel&) = delete;

    // -----------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;

    // -----------------------------------------------------------------------
    // Visibility
    // -----------------------------------------------------------------------
    void setVisible(bool visible) override;

    // -----------------------------------------------------------------------
    // Panel operations
    // -----------------------------------------------------------------------

    /**
     * Open a new shell or command in the terminal.
     * Currently launches the user's default shell ($SHELL or /bin/sh).
     */
    void openShell();

    /**
     * Run an arbitrary command line.
     * The command is parsed into an argv vector and launched via
     * TerminalProcess. Any currently-running process is killed first.
     *
     * @param commandLine  Full command string (e.g. "echo hello").
     */
    void runCommand(const std::string& commandLine);

    /**
     * Run a named task (e.g. "build", "test", "check").
     * Looks up the task in the TaskRunner, expands placeholders, and
     * launches it. Any currently-running process is killed first.
     *
     * @param taskId  The task ID (e.g. "build").
     * @return true if the task was found and launched, false otherwise.
     */
    bool runTask(const std::string& taskId);

    /**
     * Cancel the currently-running process (if any).
     */
    void cancelProcess();

    /** True if a process is currently running. */
    bool isRunning() const noexcept;

    /** Get the task runner for populating quick-launch buttons. */
    TaskRunner& taskRunner() noexcept { return taskRunner_; }

    // -----------------------------------------------------------------------
    // Callbacks — installed by EditorArea / MainWindow
    // -----------------------------------------------------------------------

    /** Fired when the panel is closed by the user. */
    std::function<void()> onClosePanel;

    /** Fired when a task completes, with exit code. */
    std::function<void(std::string taskId, int exitCode)> onTaskCompleted;

    /**
     * Prompt the user for a command (e.g. via the command palette).
     * Opens the panel if hidden and focuses the input field.
     */
    void focusInput();

    // -----------------------------------------------------------------------
    // juce::TextEditor::Listener
    // -----------------------------------------------------------------------
    void editorTextChanged(juce::TextEditor& editor) override;
    void editorFocusLost(juce::TextEditor& editor) override;
    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;
    void textEditorEscapePressed(juce::TextEditor& editor) override;

private:
    // -----------------------------------------------------------------------
    // juce::Timer — polls TerminalProcess for output (message thread)
    // -----------------------------------------------------------------------
    static constexpr int kPollIntervalHz = 30;
    void timerCallback() override;

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /** Parse a command line string into an argv vector. Handles basic
        quoting (single and double quotes). */
    std::vector<std::string> parseCommandLine(const std::string& line) const;

    /** Append text to the output editor (message thread). */
    void appendOutput(const std::string& text);

    /** Append a system status line to the output editor. */
    void appendStatus(const std::string& text);

    /** Update the process-state label. */
    void updateStateLabel();

    /** Update the cancel button visibility. */
    void updateCancelButton();

    /** Clear output (e.g. for a new session). */
    void clearOutput();

    /** Called when the process exits. */
    void onProcessExited();

    /** Run the user's shell. */
    std::string resolveShell() const;

    // -----------------------------------------------------------------------
    // Child components
    // -----------------------------------------------------------------------
    std::unique_ptr<juce::TextEditor> outputEditor_;
    std::unique_ptr<juce::TextEditor> inputField_;
    std::unique_ptr<juce::TextButton>  cancelBtn_;
    std::unique_ptr<juce::TextButton>  clearBtn_;
    std::unique_ptr<juce::Label>       stateLabel_;
    std::unique_ptr<juce::ComboBox>    taskCombo_;
    std::unique_ptr<juce::TextButton>  runTaskBtn_;

    // -----------------------------------------------------------------------
    // Logic
    // -----------------------------------------------------------------------
    TerminalProcess process_;
    TaskRunner      taskRunner_;

    bool running_ = false;
    int  lastExitCode_ = 0;

    // Input history (for Up/Down arrow recall).
    std::vector<std::string> inputHistory_;
    int historyIndex_ = -1;

    // Current command being typed (for history navigation).
    std::string pendingInput_;

    // Layout constants
    static constexpr int kInputHeight    = 26;
    static constexpr int kHeaderHeight   = 30;
    static constexpr int kMargin         = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TerminalPanel)
};

} // namespace hathor::ui
