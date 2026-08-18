// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * TerminalPanel.cpp — implementation of the native JUCE integrated terminal.
 *
 * Requirement references: L-4 §Architecture, L-4 Acceptance
 */

#include "TerminalPanel.hpp"
#include "HathorLookAndFeel.hpp"

#include <juce_gui_extra/juce_gui_extra.h>

#include <chrono>
#include <ctime>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

TerminalPanel::TerminalPanel(const std::string& projectDir)
    : taskRunner_(projectDir)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // -----------------------------------------------------------------------
    // Output editor — read-only monospace text display
    // -----------------------------------------------------------------------
    outputEditor_ = std::make_unique<juce::TextEditor>();
    outputEditor_->setReadOnly(true);
    outputEditor_->setOpaque(false);
    outputEditor_->setCaretVisible(false);
    outputEditor_->setScrollbarsShown(true);
    outputEditor_->setFont(HathorLookAndFeel::fontRegular(12.0f));
    outputEditor_->setColour(juce::TextEditor::backgroundColourId, palette.surfaceContainer);
    outputEditor_->setColour(juce::TextEditor::textColourId, palette.textPrimary);
    outputEditor_->setColour(juce::TextEditor::outlineColourId, palette.surfaceHighest.withAlpha(0.3f));
    outputEditor_->setColour(juce::ScrollBar::thumbColourId, palette.textMuted.withAlpha(0.5f));
    outputEditor_->setColour(juce::ScrollBar::trackColourId, palette.surfaceLow);
    outputEditor_->setMultiLine(true);
    outputEditor_->setReturnKeyStartsNewLine(false);
    addAndMakeVisible(*outputEditor_);

    // -----------------------------------------------------------------------
    // Input field — single-line command input
    // -----------------------------------------------------------------------
    inputField_ = std::make_unique<juce::TextEditor>();
    inputField_->setReadOnly(false);
    inputField_->setOpaque(false);
    inputField_->setCaretVisible(true);
    inputField_->setMultiLine(false);
    inputField_->setScrollbarsShown(false);
    inputField_->setFont(HathorLookAndFeel::fontRegular(12.0f));
    inputField_->setColour(juce::TextEditor::backgroundColourId, palette.surfaceContainer);
    inputField_->setColour(juce::TextEditor::textColourId, palette.textPrimary);
    inputField_->setColour(juce::TextEditor::outlineColourId, palette.accent.withAlpha(0.3f));
    inputField_->addListener(this);
    addAndMakeVisible(*inputField_);

    // -----------------------------------------------------------------------
    // State label — shows process state
    // -----------------------------------------------------------------------
    stateLabel_ = std::make_unique<juce::Label>();
    stateLabel_->setColour(juce::Label::textColourId, palette.textSecondary);
    stateLabel_->setColour(juce::Label::backgroundColourId, palette.surfaceLow);
    stateLabel_->setJustificationType(juce::Justification::centredLeft);
    stateLabel_->setBounds(0, 0, 200, kHeaderHeight);
    addAndMakeVisible(*stateLabel_);

    // -----------------------------------------------------------------------
    // Task combo + Run button
    // -----------------------------------------------------------------------
    taskCombo_ = std::make_unique<juce::ComboBox>();
    taskCombo_->setEditableText(false);
    taskCombo_->setColour(juce::ComboBox::backgroundColourId, palette.surfaceLow);
    taskCombo_->setColour(juce::ComboBox::textColourId, palette.textPrimary);
    taskCombo_->setColour(juce::ComboBox::outlineColourId, palette.surfaceHighest.withAlpha(0.3f));

    int idx = 1;
    for (const auto& [id, label] : taskRunner_.taskList())
    {
        taskCombo_->addItem(juce::String(label) + " (" + juce::String(id) + ")", idx++);
    }
    taskCombo_->setSelectedItemIndex(0);
    taskCombo_->setTooltip("Select a task to run (build, test, check, ...)");
    addAndMakeVisible(*taskCombo_);

    runTaskBtn_ = std::make_unique<juce::TextButton>("Run");
    runTaskBtn_->setTooltip("Run the selected task");
    runTaskBtn_->onClick = [this]() {
        int selectedIdx = taskCombo_->getSelectedItemIndex();
        if (selectedIdx < 0)
            return;

        // Extract task id from the item text (we stored "Label (id)")
        juce::String itemText = taskCombo_->getItemText(selectedIdx);
        int openParen = itemText.lastIndexOfChar('(');
        int closeParen = itemText.lastIndexOfChar(')');
        if (openParen > 0 && closeParen > openParen)
        {
            juce::String taskId = itemText.substring(openParen + 1, closeParen);
            runTask(taskId.toStdString());
        }
    };
    addAndMakeVisible(*runTaskBtn_);

    // -----------------------------------------------------------------------
    // Cancel and Clear buttons
    // -----------------------------------------------------------------------
    cancelBtn_ = std::make_unique<juce::TextButton>("⏹ Stop");
    cancelBtn_->setTooltip("Cancel the running process");
    cancelBtn_->setEnabled(false);
    cancelBtn_->onClick = [this]() { cancelProcess(); };
    addAndMakeVisible(*cancelBtn_);

    clearBtn_ = std::make_unique<juce::TextButton>("Clear");
    clearBtn_->setTooltip("Clear terminal output");
    clearBtn_->onClick = [this]() { clearOutput(); };
    addAndMakeVisible(*clearBtn_);

    // -----------------------------------------------------------------------
    // Wire process exit callback — fires on the worker thread. We disable it
    // in favor of polling the state in the timer (avoids JUCE message-thread
    // marshaling complexity in the process worker).
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Header buttons callback
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Initial state
    // -----------------------------------------------------------------------
    updateStateLabel();
    updateCancelButton();

    // Start polling timer
    startTimerHz(kPollIntervalHz);
}

TerminalPanel::~TerminalPanel()
{
    stopTimer();
    process_.shutdown();
}

// ---------------------------------------------------------------------------
// juce::Component
// ---------------------------------------------------------------------------

void TerminalPanel::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Background
    g.fillAll(palette.surfaceContainer);

    // Top border (subtle separator)
    g.setColour(palette.surfaceHighest.withAlpha(0.15f));
    g.drawRect(juce::Rectangle<int>(0, 0, getWidth(), getHeight()), 1);
}

void TerminalPanel::resized()
{
    auto b = getLocalBounds().reduced(kMargin);

    // Header row: state label + task combo + Run + Stop + Clear
    auto headerArea = b.removeFromTop(kHeaderHeight);
    auto rest = headerArea;

    stateLabel_->setBounds(rest.removeFromLeft(140));
    rest = headerArea.withX(stateLabel_->getRight() + kMargin);

    taskCombo_->setBounds(rest.removeFromLeft(200));
    auto btnArea = rest.withX(taskCombo_->getRight() + kMargin);

    const int btnW = 64;
    runTaskBtn_->setBounds(btnArea.removeFromLeft(btnW));
    btnArea = btnArea.translated(btnW + kMargin, 0);

    cancelBtn_->setBounds(btnArea.removeFromLeft(72));
    btnArea = btnArea.translated(72 + kMargin, 0);

    clearBtn_->setBounds(btnArea.removeFromLeft(64));

    // Output editor fills remaining vertical space (above input)
    outputEditor_->setBounds(b.removeFromTop(b.getHeight() - kInputHeight - kMargin));

    // Input field at the bottom
    inputField_->setBounds(b.removeFromBottom(kInputHeight));
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

void TerminalPanel::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
    if (visible)
        inputField_->grabKeyboardFocus();
}

// ---------------------------------------------------------------------------
// Panel operations
// ---------------------------------------------------------------------------

std::string TerminalPanel::resolveShell() const
{
    const char* shell = std::getenv("SHELL");
    if (shell && shell[0] != '\0')
        return std::string(shell);
    return "/bin/sh";
}

void TerminalPanel::openShell()
{
    if (running_)
    {
        appendStatus("A process is already running. Cancel it first.");
        return;
    }

    clearOutput();

    std::string shellPath = resolveShell();

    // On macOS, use login shell for a more authentic terminal experience.
    std::vector<std::string> argv;
#ifdef __APPLE__
    argv = {shellPath, "-l"};
#else
    argv = {shellPath};
#endif

    if (process_.launch(argv, taskRunner_.projectDir()))
    {
        running_ = true;
        updateStateLabel();
        updateCancelButton();
        appendStatus(std::string("Starting shell: ") + shellPath);
    }
    else
    {
        appendStatus("Error: " + process_.lastError());
    }
}

void TerminalPanel::runCommand(const std::string& commandLine)
{
    if (commandLine.empty())
        return;

    if (running_)
    {
        appendStatus("A process is already running. Cancel it first.");
        return;
    }

    clearOutput();

    // Run commands through the user's shell so that pipes, redirects,
    // environment variables, etc. work naturally. This is NOT implementing
    // a shell ourselves — we delegate to /bin/sh (or $SHELL).
    std::string shell = resolveShell();
    std::vector<std::string> shellArgv = {shell, "-c", commandLine};

    if (process_.launch(shellArgv, taskRunner_.projectDir()))
    {
        running_ = true;
        appendStatus(std::string("$ ") + commandLine);
        updateStateLabel();
        updateCancelButton();
        inputField_->setEnabled(false);
    }
    else
    {
        appendStatus("Error: " + process_.lastError());
    }
}

bool TerminalPanel::runTask(const std::string& taskId)
{
    const TaskDef* task = taskRunner_.findTask(taskId);
    if (!task)
    {
        appendStatus("Error: unknown task '" + taskId + "'");
        return false;
    }

    if (running_)
    {
        appendStatus("A process is already running. Cancel it first.");
        return false;
    }

    clearOutput();

    // Expand placeholders and run via shell.
    std::string expanded = taskRunner_.expandPlaceholders(task->command);
    std::vector<std::string> shellArgv = {resolveShell(), "-c", expanded};

    if (process_.launch(shellArgv, task->cwd.empty() ? taskRunner_.projectDir() : task->cwd))
    {
        running_ = true;
        appendStatus(std::string("Task: ") + task->label);
        appendStatus(std::string("$ ") + expanded);
        updateStateLabel();
        updateCancelButton();
        inputField_->setEnabled(!task->isLongRunning);
        return true;
    }
    else
    {
        appendStatus("Error: " + process_.lastError());
        return false;
    }
}

void TerminalPanel::cancelProcess()
{
    if (!running_)
        return;

    appendStatus("[cancelling...]");
    process_.cancel();
}

void TerminalPanel::focusInput()
{
    inputField_->grabKeyboardFocus();
}

bool TerminalPanel::isRunning() const noexcept
{
    return running_;
}

// ---------------------------------------------------------------------------
// juce::Timer — poll for output
// ---------------------------------------------------------------------------

void TerminalPanel::timerCallback()
{
    if (running_)
    {
        // Drain output from the process's SPSC ring buffer.
        char buf[4096];
        std::size_t n = process_.drainOutput(buf, sizeof(buf));
        if (n > 0)
            appendOutput(std::string(buf, n));

        // Check if the process has exited (state transitioned to Done).
        if (process_.state() == TerminalProcess::State::Done)
            onProcessExited();
    }
}

// ---------------------------------------------------------------------------
// Process exit handler
// ---------------------------------------------------------------------------

void TerminalPanel::onProcessExited()
{
    if (!running_)
        return;

    running_ = false;

    auto status = process_.exitStatus();
    lastExitCode_ = status.exitCode;

    if (status.wasCancelled)
        appendStatus("[process cancelled]");
    else if (status.exitedNormally)
        appendStatus("[process exited with code " + std::to_string(status.exitCode) + "]");
    else if (status.signal > 0)
        appendStatus("[process killed by signal " + std::to_string(status.signal) + "]");
    else
        appendStatus("[process terminated]");

    updateStateLabel();
    updateCancelButton();
    inputField_->setEnabled(true);

    if (onTaskCompleted)
    {
        // Try to infer the task id from the combo box selection.
        juce::String itemText = taskCombo_->getItemText(taskCombo_->getSelectedItemIndex());
        int openParen = itemText.lastIndexOfChar('(');
        int closeParen = itemText.lastIndexOfChar(')');
        std::string taskId = "";
        if (openParen > 0 && closeParen > openParen)
            taskId = itemText.substring(openParen + 1, closeParen).toStdString();

        onTaskCompleted(taskId, status.exitCode);
    }
}

// ---------------------------------------------------------------------------
// juce::TextEditor::Listener
// ---------------------------------------------------------------------------

void TerminalPanel::textEditorTextChanged(juce::TextEditor& /*editor*/)
{
    (void)0; // no-op — we don't need to react to live text changes
}

void TerminalPanel::textEditorFocusLost(juce::TextEditor& /*editor*/)
{
    (void)0; // no-op — focus loss doesn't trigger any terminal action
}

void TerminalPanel::textEditorReturnKeyPressed(juce::TextEditor& /*editor*/)
{
    if (!running_)
    {
        std::string cmd = inputField_->getText().toStdString();
        if (!cmd.empty())
        {
            // Add to history.
            if (inputHistory_.empty() ||
                inputHistory_.back() != cmd)
                inputHistory_.push_back(cmd);
            historyIndex_ = -1;

            runCommand(cmd);
            inputField_->clear();
        }
    }
}

void TerminalPanel::textEditorEscapeKeyPressed(juce::TextEditor& /*editor*/)
{
    if (running_)
    {
        cancelProcess();
    }
    else
    {
        inputField_->clear();
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void TerminalPanel::appendOutput(const std::string& text)
{
    if (text.empty())
        return;

    // Insert text at the end of the document.
    outputEditor_->setCaretPosition(static_cast<int>(outputEditor_->getText().length()));
    outputEditor_->insertTextAtCaret(juce::String(text));
}

void TerminalPanel::appendStatus(const std::string& text)
{
    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto nowT = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_r(&nowT, &tm);

    char timeBuf[16];
    std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &tm);

    std::string line = std::string("[") + timeBuf + "] " + text + "\n";
    appendOutput(line);
}

void TerminalPanel::updateStateLabel()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    if (running_)
    {
        stateLabel_->setText("● Running", juce::dontSendNotification);
        stateLabel_->setColour(juce::Label::textColourId, palette.accent);
    }
    else
    {
        std::string status = "○ Idle";
        if (lastExitCode_ == 0)
            status = "✓ Done";
        else if (lastExitCode_ != 0)
            status = "✗ Failed (" + std::to_string(lastExitCode_) + ")";

        stateLabel_->setText(juce::String(status), juce::dontSendNotification);
        stateLabel_->setColour(juce::Label::textColourId, palette.textSecondary);
    }
}

void TerminalPanel::updateCancelButton()
{
    cancelBtn_->setEnabled(running_);
}

void TerminalPanel::clearOutput()
{
    outputEditor_->clear();
}

} // namespace hathor::ui
