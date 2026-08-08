// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * HathorTab.cpp — implementation of HathorTab.
 *
 * Requirements: 22.1, 22.2, 22.5, 22.7
 */

#include "HathorTab.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

HathorTab::HathorTab(int slotIndex)
    : slotIndex_(slotIndex)
    , editor_(document_, &tokeniser_) // MiniNotationTokeniser wired in task 3.5
{
    // -----------------------------------------------------------------------
    // Editor font: JetBrains Mono, 13 pt (code-default from mockup, Req 22.1)
    // -----------------------------------------------------------------------
    editor_.setFont(HathorLookAndFeel::fontRegular(
        HathorLookAndFeel::Typography::codeDefault));

    // -----------------------------------------------------------------------
    // Dark background colours (Req 22.1, 20.2) — sourced from design tokens.
    // These local overrides ensure correctness even if this component is
    // used outside of MainWindow's look-and-feel scope.
    // -----------------------------------------------------------------------
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    editor_.setColour(juce::CodeEditorComponent::backgroundColourId,
                      palette.surface);
    editor_.setColour(juce::CodeEditorComponent::defaultTextColourId,
                      palette.codeText);
    editor_.setColour(juce::CodeEditorComponent::highlightColourId,
                      palette.accent.withAlpha(0.3f));
    editor_.setColour(juce::CodeEditorComponent::lineNumberBackgroundId,
                      palette.surfaceLow);
    editor_.setColour(juce::CodeEditorComponent::lineNumberTextId,
                      palette.codeLineNum);

    // -----------------------------------------------------------------------
    // Register as a CodeDocument listener so we can detect edits (Req 22.5)
    // -----------------------------------------------------------------------
    document_.addListener(this);

    addAndMakeVisible(editor_);
}

HathorTab::~HathorTab()
{
    document_.removeListener(this);
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

void HathorTab::setFilePath(const juce::File& f)
{
    filePath_ = f;
}

void HathorTab::setDisplayLabel(const std::string& label)
{
    if (label.empty())
        displayLabel_.reset();
    else
        displayLabel_ = label;
}

juce::String HathorTab::tabLabel() const
{
    // 1. Front-matter label (Req 22.2)
    if (displayLabel_.has_value() && !displayLabel_->empty())
        return juce::String(*displayLabel_);

    // 2. Filename stem (Req 22.2)
    if (filePath_.has_value())
        return filePath_->getFileNameWithoutExtension();

    // 3. Generic untitled name
    return "untitled-" + juce::String(slotIndex_);
}

void HathorTab::clearUnsavedDot()
{
    if (unsavedDot_)
    {
        unsavedDot_ = false;
        if (onUnsavedDotChanged)
            onUnsavedDotChanged();
    }
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void HathorTab::resized()
{
    editor_.setBounds(getLocalBounds());
}

// ---------------------------------------------------------------------------
// juce::CodeDocument::Listener
// ---------------------------------------------------------------------------

void HathorTab::codeDocumentTextInserted(const juce::String& /*newText*/,
                                         int /*insertIndex*/)
{
    markUnsaved();
}

void HathorTab::codeDocumentTextDeleted(int /*startIndex*/,
                                        int /*endIndex*/)
{
    markUnsaved();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void HathorTab::markUnsaved()
{
    if (!unsavedDot_)
    {
        unsavedDot_ = true;
        if (onUnsavedDotChanged)
            onUnsavedDotChanged();
    }
}

} // namespace hathor::ui
