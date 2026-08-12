// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * DebuggerPanel.cpp — L-6: native C++ debugger UI implementation.
 *
 * Requirement references: L-6 §Native/C++ Debugging
 */

#include "DebuggerPanel.hpp"

#include <sstream>

namespace hathor::ui {

namespace {

/// Whitespace-split command-line arguments (simple, sufficient for the panel).
std::vector<std::string> splitArgs(const juce::String& text)
{
    std::vector<std::string> out;
    std::istringstream ss(text.toStdString());
    std::string tok;
    while (ss >> tok)
        out.push_back(tok);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

DebuggerPanel::DebuggerPanel()
{
    const auto& pal = HathorLookAndFeel::globalPalette();

    auto makeEditor = [pal](const juce::String& placeholder)
    {
        auto ed = std::make_unique<juce::TextEditor>();
        ed->setFont(HathorLookAndFeel::fontRegular(11.0f));
        ed->setJustification(juce::Justification::centredLeft);
        ed->setTextToShowWhenEmpty(placeholder, pal.textDisabled);
        ed->setColour(juce::TextEditor::outlineColourId, pal.surfaceHighest);
        ed->setColour(juce::TextEditor::focusedOutlineColourId, pal.accentDim);
        return ed;
    };
    auto makeView = [pal](const juce::String& placeholder)
    {
        auto ed = std::make_unique<juce::TextEditor>();
        ed->setFont(HathorLookAndFeel::fontRegular(11.0f));
        ed->setMultiLine(true);
        ed->setReadOnly(true);
        ed->setScrollbarsShown(true);
        ed->setTextToShowWhenEmpty(placeholder, pal.textDisabled);
        ed->setColour(juce::TextEditor::outlineColourId, pal.surfaceHighest);
        return ed;
    };

    statusLabel_ = std::make_unique<juce::Label>();
    statusLabel_->setFont(HathorLookAndFeel::fontRegular(11.0f));
    addAndMakeVisible(*statusLabel_);

    exeField_    = makeEditor("Executable path (e.g. /path/to/hathor-ui)");
    argsField_   = makeEditor("Target args");
    fileField_   = makeEditor("Source file");
    lineField_   = makeEditor("Line");
    watchField_  = makeEditor("Watch expression (e.g. myVar.field)");

    stackTitle_  = std::make_unique<juce::Label>("stack", "Call Stack");
    stackTitle_->setFont(HathorLookAndFeel::fontMedium(10.0f));
    localsTitle_ = std::make_unique<juce::Label>("locals", "Locals");
    localsTitle_->setFont(HathorLookAndFeel::fontMedium(10.0f));
    watchesTitle_ = std::make_unique<juce::Label>("watches", "Watches");
    watchesTitle_->setFont(HathorLookAndFeel::fontMedium(10.0f));

    stackView_   = makeView("Stopped frames appear here");
    localsView_  = makeView("Local variables at the stop point");
    watchesView_ = makeView("Add a watch expression above");
    outputView_  = makeView("Debugger output");

    launchBtn_  = std::make_unique<juce::TextButton>("Launch");
    launchBtn_->onClick = [this]() { launchSession(); };
    stopBtn_    = std::make_unique<juce::TextButton>("Stop");
    stopBtn_->onClick = [this]() { stopSession(); };
    addBpBtn_   = std::make_unique<juce::TextButton>("Add BP");
    addBpBtn_->onClick = [this]() { addBreakpoint(); };
    delBpBtn_   = std::make_unique<juce::TextButton>("Del BP");
    delBpBtn_->onClick = [this]() { deleteBreakpoint(); };
    continueBtn_ = std::make_unique<juce::TextButton>("Continue");
    continueBtn_->onClick = [this]() { session_.continue_(); setStatus("Continuing\u2026"); };
    pauseBtn_   = std::make_unique<juce::TextButton>("Pause");
    pauseBtn_->onClick = [this]() { session_.interrupt(); setStatus("Interrupting\u2026"); };
    stepOverBtn_ = std::make_unique<juce::TextButton>("Step Over");
    stepOverBtn_->onClick = [this]() { session_.stepOver(); setStatus("Stepping over\u2026"); };
    stepIntoBtn_ = std::make_unique<juce::TextButton>("Step Into");
    stepIntoBtn_->onClick = [this]() { session_.stepInto(); setStatus("Stepping into\u2026"); };
    stepOutBtn_ = std::make_unique<juce::TextButton>("Step Out");
    stepOutBtn_->onClick = [this]() { session_.stepOut(); setStatus("Stepping out\u2026"); };
    watchBtn_   = std::make_unique<juce::TextButton>("Watch");
    watchBtn_->onClick = [this]() { addWatch(); };
    closeBtn_   = std::make_unique<juce::TextButton>("\u00D7");
    closeBtn_->onClick = [this]() {
        if (onClosePanel)
            onClosePanel();
    };

    addAndMakeVisible(launchBtn_.get());
    addAndMakeVisible(stopBtn_.get());
    addAndMakeVisible(addBpBtn_.get());
    addAndMakeVisible(delBpBtn_.get());
    addAndMakeVisible(continueBtn_.get());
    addAndMakeVisible(pauseBtn_.get());
    addAndMakeVisible(stepOverBtn_.get());
    addAndMakeVisible(stepIntoBtn_.get());
    addAndMakeVisible(stepOutBtn_.get());
    addAndMakeVisible(watchBtn_.get());
    addAndMakeVisible(closeBtn_.get());
    addAndMakeVisible(exeField_.get());
    addAndMakeVisible(argsField_.get());
    addAndMakeVisible(fileField_.get());
    addAndMakeVisible(lineField_.get());
    addAndMakeVisible(watchField_.get());
    addAndMakeVisible(stackTitle_.get());
    addAndMakeVisible(localsTitle_.get());
    addAndMakeVisible(watchesTitle_.get());
    addAndMakeVisible(stackView_.get());
    addAndMakeVisible(localsView_.get());
    addAndMakeVisible(watchesView_.get());
    addAndMakeVisible(outputView_.get());

    installSessionCallbacks();

    // Explicit platform-support status (never a silent fake debugger).
    switch (DebugSession::detectDebugger())
    {
        case DebugSession::DebuggerType::Lldb:
            setStatus("Native debugger: LLDB (macOS/Linux). Launch a target to begin.");
            break;
        case DebugSession::DebuggerType::Gdb:
            setStatus("Native debugger: GDB (Linux). Launch a target to begin.");
            break;
        case DebugSession::DebuggerType::None:
            setStatus("Native debugging is not supported on this platform "
                      "(no LLDB/GDB found).", true);
            launchBtn_->setEnabled(false);
            launchBtn_->setTooltip("No supported native debugger is available on this platform.");
            break;
    }

    updateControlStates();
}

DebuggerPanel::~DebuggerPanel()
{
    stopTimer();
    session_.shutdown();
}

// ---------------------------------------------------------------------------
// juce::Component
// ---------------------------------------------------------------------------

void DebuggerPanel::resized()
{
    auto b = getLocalBounds();

    // Header: status + close
    auto header = b.removeFromTop(kHeaderHeight);
    closeBtn_->setBounds(header.removeFromRight(24));
    statusLabel_->setBounds(header.reduced(kMargin, 4));

    // Launch row
    auto launch = b.removeFromTop(kFieldHeight);
    stopBtn_->setBounds(launch.removeFromRight(52).reduced(1, 3));
    launchBtn_->setBounds(launch.removeFromRight(64).reduced(1, 3));
    argsField_->setBounds(launch.removeFromRight(launch.getWidth() / 3).reduced(0, 2));
    exeField_->setBounds(launch.reduced(0, 2));

    // Breakpoint row
    auto bp = b.removeFromTop(kFieldHeight);
    delBpBtn_->setBounds(bp.removeFromRight(56).reduced(1, 3));
    addBpBtn_->setBounds(bp.removeFromRight(56).reduced(1, 3));
    lineField_->setBounds(bp.removeFromRight(52).reduced(0, 2));
    fileField_->setBounds(bp.reduced(0, 2));

    // Control row: Continue | Pause | Step Over | Step Into | Step Out
    auto ctrl = b.removeFromTop(kFieldHeight);
    const int btnW = ctrl.getWidth() / 5;
    continueBtn_->setBounds(ctrl.removeFromLeft(btnW).reduced(1, 3));
    pauseBtn_->setBounds(ctrl.removeFromLeft(btnW).reduced(1, 3));
    stepOverBtn_->setBounds(ctrl.removeFromLeft(btnW).reduced(1, 3));
    stepIntoBtn_->setBounds(ctrl.removeFromLeft(btnW).reduced(1, 3));
    stepOutBtn_->setBounds(ctrl.reduced(1, 3));

    // Watch row
    auto watch = b.removeFromTop(kFieldHeight);
    watchBtn_->setBounds(watch.removeFromRight(64).reduced(1, 3));
    watchField_->setBounds(watch.reduced(0, 2));

    // Output strip at the bottom
    auto outputArea = b.removeFromBottom(64);
    outputView_->setBounds(outputArea);

    // Three views side by side
    const int colW = b.getWidth() / 3;
    auto col1 = b.removeFromLeft(colW);
    auto col2 = b.removeFromLeft(colW);
    auto col3 = b;

    stackTitle_->setBounds(col1.removeFromTop(14));
    stackView_->setBounds(col1);
    localsTitle_->setBounds(col2.removeFromTop(14));
    localsView_->setBounds(col2);
    watchesTitle_->setBounds(col3.removeFromTop(14));
    watchesView_->setBounds(col3);
}

void DebuggerPanel::paint(juce::Graphics& g)
{
    g.fillAll(HathorLookAndFeel::fromComponent(*this).getPalette().surface);
}

void DebuggerPanel::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
    if (visible)
        startTimerHz(kPollIntervalHz);
    else
        stopTimer();
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void DebuggerPanel::launchSession()
{
    if (session_.isRunning())
        return;

    DebugSession::Config cfg;
    cfg.executable = exeField_->getText().trim().toStdString();
    if (cfg.executable.empty())
    {
        setStatus("Enter an executable path to launch.", true);
        return;
    }
    cfg.args = splitArgs(argsField_->getText());
    cfg.sourceDir.clear();

    const std::string err = session_.launch(cfg);
    if (!err.empty())
    {
        setStatus(err, true);
        appendOutput("launch error: " + err);
        return;
    }

    setStatus("Session started. Set a breakpoint, then Continue.");
    updateControlStates();
}

void DebuggerPanel::stopSession()
{
    session_.shutdown();
    setStatus("Session stopped.");
    updateControlStates();
}

void DebuggerPanel::addBreakpoint()
{
    const std::string file = fileField_->getText().trim().toStdString();
    const int line = lineField_->getText().getIntValue();
    const int id = session_.setBreakpoint(file, line);
    if (id < 0)
        setStatus(session_.lastError(), true);
    else
        setStatus("Breakpoint " + juce::String(id) + " requested at " + file + ":" + juce::String(line));
}

void DebuggerPanel::deleteBreakpoint()
{
    if (breakpoints_.empty())
    {
        setStatus("No breakpoints to delete.", true);
        return;
    }
    const int id = breakpoints_.back().id;
    if (session_.deleteBreakpoint(id))
        setStatus("Breakpoint " + juce::String(id) + " deleted.");
}

void DebuggerPanel::addWatch()
{
    const std::string expr = watchField_->getText().trim().toStdString();
    if (expr.empty())
        return;
    session_.evaluateWatch(expr, expr);
    watchField_->clear();
}

// ---------------------------------------------------------------------------
// Session callbacks
// ---------------------------------------------------------------------------

void DebuggerPanel::installSessionCallbacks()
{
    session_.onStopped = [this](std::vector<DebugSession::StackFrame> frames) {
        stackFrames_ = std::move(frames);
        juce::String loc;
        if (!stackFrames_.empty())
            loc = stackFrames_.front().function;
        setStatus("Stopped" + (loc.isEmpty() ? juce::String() : juce::String(" \u2014 ") + loc));
        // Fresh locals for the stop point.
        session_.requestLocals();
        viewsDirty_ = true;
    };

    session_.onLocals = [this](std::vector<DebugSession::WatchValue> values) {
        locals_ = std::move(values);
        viewsDirty_ = true;
    };

    session_.onWatchValue = [this](DebugSession::WatchValue v) {
        // Upsert by name.
        for (auto& w : watches_)
        {
            if (w.name == v.name)
            {
                w = std::move(v);
                viewsDirty_ = true;
                return;
            }
        }
        watches_.push_back(std::move(v));
        viewsDirty_ = true;
    };

    session_.onBreakpoints = [this](std::vector<DebugSession::Breakpoint> bps) {
        breakpoints_ = std::move(bps);
    };

    session_.onOutput = [this](std::string line) {
        appendOutput(line);
    };

    session_.onError = [this](std::string err) {
        appendOutput("error: " + err);
    };

    session_.onExited = [this]() {
        setStatus("Debugger process exited.");
        updateControlStates();
    };
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

void DebuggerPanel::refreshViews()
{
    juce::String stackText;
    for (const auto& f : stackFrames_)
    {
        stackText += juce::String(f.function) + "()\n";
        if (!f.file.empty())
            stackText += "    " + juce::String(f.file) + ":" + juce::String(f.line);
        if (f.column > 0)
            stackText += ":" + juce::String(f.column);
        stackText += "\n";
    }
    stackView_->setText(stackText, false);
    stackView_->setCaretPosition(0);

    juce::String localsText;
    for (const auto& v : locals_)
    {
        if (!v.type.empty())
            localsText += juce::String(v.type) + " ";
        localsText += juce::String(v.name) + " = " + juce::String(v.value) + "\n";
    }
    localsView_->setText(localsText, false);
    localsView_->setCaretPosition(0);

    juce::String watchesText;
    for (const auto& v : watches_)
    {
        if (!v.type.empty())
            watchesText += juce::String(v.type) + " ";
        watchesText += juce::String(v.name) + " = " + juce::String(v.value) + "\n";
    }
    watchesView_->setText(watchesText, false);
    watchesView_->setCaretPosition(0);
}

void DebuggerPanel::appendOutput(const std::string& line)
{
    outputLines_.push_back(line);
    while (outputLines_.size() > kMaxOutputLines)
        outputLines_.pop_front();
    outputDirty_ = true;
}

void DebuggerPanel::updateControlStates()
{
    const bool running = session_.isRunning();
    const bool supported = (DebugSession::detectDebugger() != DebugSession::DebuggerType::None);

    launchBtn_->setEnabled(!running && supported);
    stopBtn_->setEnabled(running);
    addBpBtn_->setEnabled(running);
    delBpBtn_->setEnabled(running && !breakpoints_.empty());
    continueBtn_->setEnabled(running);
    pauseBtn_->setEnabled(running);
    stepOverBtn_->setEnabled(running);
    stepIntoBtn_->setEnabled(running);
    stepOutBtn_->setEnabled(running);
    watchBtn_->setEnabled(running);
}

void DebuggerPanel::setStatus(const juce::String& status, bool isError)
{
    statusText_ = status;
    statusIsError_ = isError;
    if (statusLabel_)
    {
        const auto& pal = HathorLookAndFeel::fromComponent(*this).getPalette();
        statusLabel_->setColour(juce::Label::textColourId,
                                isError ? pal.error : pal.textSecondary);
        statusLabel_->setText(status, juce::dontSendNotification);
    }
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

void DebuggerPanel::timerCallback()
{
    if (!session_.isRunning())
        return;

    session_.pollResults();

    if (viewsDirty_)
    {
        viewsDirty_ = false;
        refreshViews();
    }
    if (outputDirty_)
    {
        outputDirty_ = false;
        juce::String out;
        for (const auto& line : outputLines_)
            out += juce::String(line) + "\n";
        outputView_->setText(out, false);
        outputView_->setCaretPosition(0);
    }
}

} // namespace hathor::ui
