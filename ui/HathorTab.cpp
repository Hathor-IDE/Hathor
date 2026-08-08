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

HathorTab::HathorTab(int slotIndex, const juce::File& file)
    : slotIndex_(slotIndex)
    , useChuckTokeniser_(ChuckTokeniser::isChuckFile(file))
    , editor_(document_, useChuckTokeniser_
                                  ? static_cast<juce::CodeTokeniser*>(&chuckTokeniser_)
                                  : static_cast<juce::CodeTokeniser*>(&miniTokeniser_))
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

     // -----------------------------------------------------------------------
     // Per-slot Play/Stop button (B1)
     // -----------------------------------------------------------------------
     // Small icon-only button that toggles the tab's slot play/stop state.
     // It dispatches "slot-play <slot>" / "slot-stop <slot>" via the
     // onPlayStopClicked callback installed by EditorArea.
     slotPlayButton_.setButtonText("");
     slotPlayButton_.setTooltip("Play/Stop this tab's slot");
     slotPlayButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
     slotPlayButton_.onClick = [this]() { slotPlayButtonClicked(); };
     addAndMakeVisible(slotPlayButton_);
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
    setFileTypeFromPath(f);
}

void HathorTab::setFileTypeFromPath(const juce::File& file)
{
    const bool isChuck = ChuckTokeniser::isChuckFile(file);

    if (isChuck != useChuckTokeniser_)
    {
        useChuckTokeniser_ = isChuck;
        // Note: CodeEditorComponent does not allow swapping the tokeniser
        // after construction in JUCE 8. The constructor picks the tokeniser
        // based on file type at tab-creation time. Here we only update the
        // colour scheme to match the active tokeniser's scheme.
        if (isChuck)
            editor_.setColourScheme(chuckTokeniser_.getDefaultColourScheme());
        else
            editor_.setColourScheme(miniTokeniser_.getDefaultColourScheme());
    }
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
// Per-slot Play/Stop (B1)
// ---------------------------------------------------------------------------

void HathorTab::setSlotRunningVisual(bool running) noexcept
{
    if (slotRunning_ == running)
        return;

    slotRunning_ = running;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    if (running)
    {
        // Stop icon (squares) — filled with accent for visual prominence.
        slotPlayButton_.setButtonText(juce::CharPointer_UTF8("\xE2\x96\x90\xC2\xA0\xE2\x96\x90\xC2\xA0\xE2\x96\x90"));
        slotPlayButton_.setColour(juce::TextButton::buttonColourId,
                                  palette.surfaceLow);
        slotPlayButton_.setColour(juce::TextButton::textColourOnId,
                                  palette.error);
        slotPlayButton_.setColour(juce::TextButton::textColourOffId,
                                  palette.error);
    }
    else
    {
        // Play icon (triangle).
        slotPlayButton_.setButtonText(juce::CharPointer_UTF8("\xE2\x96\xB6"));
        slotPlayButton_.setColour(juce::TextButton::buttonColourId,
                                  palette.surfaceLow);
        slotPlayButton_.setColour(juce::TextButton::textColourOnId,
                                  palette.textPrimary);
        slotPlayButton_.setColour(juce::TextButton::textColourOffId,
                                  palette.textPrimary);
    }

    slotPlayButton_.repaint();
}

void HathorTab::slotPlayButtonClicked()
{
    if (onPlayStopClicked)
        onPlayStopClicked();
}

void HathorTab::paintSlotPlayButton(juce::Graphics& /*g*/)
{
    // Button is a juce::TextButton — paint is handled by JUCE.
    // This method exists for potential custom icon drawing in the future.
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void HathorTab::resized()
{
    // Play/Stop button: small icon button at the top-right of the editor area.
    static constexpr int kButtonSize = 24;
    static constexpr int kButtonMargin = 6;

    slotPlayButton_.setBounds(
        getWidth() - kButtonSize - kButtonMargin,
        kButtonMargin,
        kButtonSize,
        kButtonSize);

    // Editor fills the remaining space, below the button.
    const int editorTop = kButtonSize + kButtonMargin * 2;
    editor_.setBounds(0, editorTop, getWidth(), getHeight() - editorTop);
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
