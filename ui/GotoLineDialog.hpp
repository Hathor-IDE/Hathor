// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GotoLineDialog.hpp — modal "Go to Line" input dialog.
 *
 * A small modal DialogWindow content component (mirrors the BakeTargetDialog
 * convention) that lets the user enter a 1-based line number. Input is
 * validated against the document bounds; the callback is only invoked with a
 * valid line number.
 *
 * Requirement references: L-1 §3 (Go to Line action).
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>

namespace hathor::ui {

/**
 * GotoLineDialog
 *
 * Contents:
 *   - A numeric TextEditor, pre-filled with the current cursor line.
 *   - An inline error label (shown only on invalid input).
 *   - OK / Cancel buttons.
 */
class GotoLineDialog : public juce::Component,
                       public juce::TextEditor::Listener,
                       public juce::Button::Listener
{
public:
    /// Invoked on a valid, confirmed line number (1-based).
    using LineCallback = std::function<void(int lineNumber)>;

    /** Construct the dialog content.
        @param numLines     Total lines in the document (1-based max).
        @param currentLine  1-based line number to pre-fill (clamped to [1, numLines]).
        @param cb           Called with the chosen 1-based line when the user
                            confirms with valid input.
    */
    GotoLineDialog(int numLines, int currentLine, LineCallback cb);

    ~GotoLineDialog() override = default;

    static constexpr int kWidth  = 360;
    static constexpr int kHeight = 150;

    // juce::Component
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    // TextEditor::Listener
    void textEditorReturnKeyPressed(juce::TextEditor&) override;
    void textEditorEscapeKeyPressed(juce::TextEditor&) override;
    void textEditorFocusLost(juce::TextEditor&) override {}

    // Button::Listener
    void buttonClicked(juce::Button*) override;

    /// Validate the field and, if valid, fire callback_ then dismiss.
    void attemptConfirm();

    /// Show an inline validation error and keep the dialog open.
    void setError(const juce::String& msg);

    /// Clear the inline error.
    void clearError();

    std::unique_ptr<juce::TextEditor> lineField_;
    std::unique_ptr<juce::TextButton> okBtn_;
    std::unique_ptr<juce::TextButton> cancelBtn_;
    std::unique_ptr<juce::Label>      promptLabel_;
    std::unique_ptr<juce::Label>      errorLabel_;

    int         numLines_ = 0;
    LineCallback callback_;
    bool        fired_    = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GotoLineDialog)
};

/**
 * Launch the Go-to-Line dialog modally, centred on @p parent.
 * @return false if the platform does not permit modal loops.
 */
bool showGotoLineDialog(juce::Component* parent,
                        int numLines,
                        int currentLine,
                        GotoLineDialog::LineCallback cb);

} // namespace hathor::ui
