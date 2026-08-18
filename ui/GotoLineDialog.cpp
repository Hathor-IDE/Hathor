// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GotoLineDialog.cpp — implementation.
 *
 * Requirement references: L-1 §3 (Go to Line action).
 */

#include "GotoLineDialog.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GotoLineDialog::GotoLineDialog(const int numLines,
                               const int currentLine,
                               LineCallback cb)
    : callback_(std::move(cb))
{
    numLines_ = juce::jmax(1, numLines);

    // Prompt
    promptLabel_ = std::make_unique<juce::Label>();
    promptLabel_->setText("Go to line (1\u2013" + juce::String(numLines_) + "):",
                          juce::dontSendNotification);
    promptLabel_->setFont(HathorLookAndFeel::fontRegular(14.0f));
    promptLabel_->setColour(juce::Label::textColourId,
                            HathorLookAndFeel::defaultPalette().textPrimary);
    promptLabel_->setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(promptLabel_.get());

    // Error label (hidden until needed)
    errorLabel_ = std::make_unique<juce::Label>();
    errorLabel_->setFont(HathorLookAndFeel::fontRegular(12.0f));
    errorLabel_->setColour(juce::Label::textColourId,
                           HathorLookAndFeel::defaultPalette().error);
    errorLabel_->setJustificationType(juce::Justification::centredLeft);
    errorLabel_->setVisible(false);
    addAndMakeVisible(errorLabel_.get());

    // Line number input — digits only.
    lineField_ = std::make_unique<juce::TextEditor>();
    lineField_->addListener(this);
    lineField_->setFont(HathorLookAndFeel::fontRegular(14.0f));
    lineField_->setColour(juce::TextEditor::backgroundColourId,
                          HathorLookAndFeel::defaultPalette().surfaceContainer);
    lineField_->setColour(juce::TextEditor::textColourId,
                          HathorLookAndFeel::defaultPalette().textPrimary);
    lineField_->setColour(juce::TextEditor::outlineColourId,
                          HathorLookAndFeel::defaultPalette().surfaceHighest);
    lineField_->setColour(juce::CaretComponent::caretColourId,
                          HathorLookAndFeel::defaultPalette().accent);
    lineField_->setInputRestrictions(0, "0123456789");
    lineField_->setMultiLine(false);
    addAndMakeVisible(lineField_.get());

    // Pre-fill with the current cursor line (1-based), clamped to valid range.
    int seed = juce::jlimit(1, numLines_, currentLine);
    lineField_->setText(juce::String(seed), juce::dontSendNotification);
    lineField_->selectAll();

    // Buttons
    okBtn_ = std::make_unique<juce::TextButton>("OK");
    okBtn_->setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    okBtn_->addListener(this);
    addAndMakeVisible(okBtn_.get());

    cancelBtn_ = std::make_unique<juce::TextButton>("Cancel");
    cancelBtn_->setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    cancelBtn_->addListener(this);
    addAndMakeVisible(cancelBtn_.get());

    
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void GotoLineDialog::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    g.fillAll(palette.surfaceContainer);
}

void GotoLineDialog::resized()
{
    auto area = getLocalBounds().reduced(16);

    promptLabel_->setBounds(area.removeFromTop(22));
    area.removeFromTop(6);
    errorLabel_->setBounds(area.removeFromTop(16));
    area.removeFromTop(6);
    lineField_->setBounds(area.removeFromTop(34));
    area.removeFromTop(12);

    const int btnW = 100;
    auto buttonRow = area.removeFromBottom(30);
    cancelBtn_->setBounds(buttonRow.removeFromLeft(btnW));
    buttonRow = buttonRow.reduced(8, 0);
    okBtn_->setBounds(buttonRow.removeFromRight(btnW));
}

// ---------------------------------------------------------------------------
// TextEditor::Listener
// ---------------------------------------------------------------------------

void GotoLineDialog::textEditorReturnKeyPressed(juce::TextEditor&)
{
    attemptConfirm();
}

void GotoLineDialog::textEditorEscapeKeyPressed(juce::TextEditor&)
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState(0);
}

// ---------------------------------------------------------------------------
// Button::Listener
// ---------------------------------------------------------------------------

void GotoLineDialog::buttonClicked(juce::Button* button)
{
    if (button == okBtn_.get())
    {
        attemptConfirm();
    }
    else if (button == cancelBtn_.get())
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

void GotoLineDialog::setError(const juce::String& msg)
{
    errorLabel_->setText(msg, juce::dontSendNotification);
    errorLabel_->setVisible(true);
    lineField_->grabKeyboardFocus();
    lineField_->selectAll();
}

void GotoLineDialog::clearError()
{
    errorLabel_->setVisible(false);
}

void GotoLineDialog::attemptConfirm()
{
    if (fired_)
        return;

    const juce::String text = lineField_->getText().trim();

    // Empty input
    if (text.isEmpty())
    {
        setError("Enter a line number.");
        return;
    }

    // Strict: every character must be a digit (defensive — input restriction
    // already limits entry, but the field can be populated programmatically).
    for (int i = 0; i < text.length(); ++i)
    {
        if (!juce::CharacterFunctions::isDigit(text[i]))
        {
            setError("Enter a valid line number.");
            return;
        }
    }

    // Parse as a 64-bit value to tolerate very large inputs without overflow.
    const juce::int64 value = text.getLargeIntValue();

    if (value < 1)
    {
        setError("Line must be at least 1.");
        return;
    }

    if (value > std::numeric_limits<int>::max())
    {
        setError("Line number is too large.");
        return;
    }

    const int lineNumber = static_cast<int>(value);

    if (lineNumber > numLines_)
    {
        setError("Line must be at most " + juce::String(numLines_) + ".");
        return;
    }

    clearError();
    fired_ = true;

    if (callback_)
        callback_(lineNumber);

    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState(1);
}

// ---------------------------------------------------------------------------
// Modal launcher
// ---------------------------------------------------------------------------

bool showGotoLineDialog(juce::Component* parent,
                        const int numLines,
                        const int currentLine,
                        GotoLineDialog::LineCallback cb)
{
    if (!parent)
        return false;

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle            = "Go to Line";
    opts.dialogBackgroundColour = HathorLookAndFeel::defaultPalette().surfaceContainer;
    opts.content.setOwned(new GotoLineDialog(numLines, currentLine, std::move(cb)));
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar    = false;
    opts.resizable            = false;
    opts.componentToCentreAround = parent;

#if JUCE_MODAL_LOOPS_PERMITTED
    return opts.launchAsync() != nullptr;
#else
    juce::ignoreUnused(parent, numLines, currentLine, cb);
    return false;
#endif
}

} // namespace hathor::ui
