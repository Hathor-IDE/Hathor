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

     // -----------------------------------------------------------------------
     // C1: Register the playback highlight overlay as a child component.
     // It sits on top of editor_ and paints only the transient highlight.
     // -----------------------------------------------------------------------
     addAndMakeVisible(highlightOverlay_);
     highlightOverlay_.setInterceptsMouseClicks(false, false);

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
    // Style as a flat icon button — no background fill, just the icon.
    slotPlayButton_.setColour(juce::TextButton::buttonColourId,
                              juce::Colours::transparentBlack);
    slotPlayButton_.onClick = [this]() { slotPlayButtonClicked(); };
    addAndMakeVisible(slotPlayButton_);

    // Initialize the button visual state to stopped (Play icon).
    // The authoritative state is the engine's SlotState::running atomic,
    // which UITimer syncs at 60 Hz via syncSlotButtonStates().
    setSlotRunningVisual(false);
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

void HathorTab::setFrontMatter(const FrontMatter& fm)
{
    frontMatter_ = fm;
    // Sync display label from front matter if present.
    if (fm.label && !fm.label->empty())
        displayLabel_ = *fm.label;
    else
        displayLabel_.reset();
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
        // Stop icon (two squares).
        slotPlayButton_.setButtonText(juce::String::charToString(0x25A0) + " " +
                                      juce::String::charToString(0x25A0));
        slotPlayButton_.setColour(juce::TextButton::buttonColourId,
                                  juce::Colours::transparentBlack);
        slotPlayButton_.setColour(juce::TextButton::textColourOnId,
                                  palette.error);
        slotPlayButton_.setColour(juce::TextButton::textColourOffId,
                                  palette.error);
    }
    else
    {
        // Play icon (triangle).
        slotPlayButton_.setButtonText(juce::String::charToString(0x25B6));
        slotPlayButton_.setColour(juce::TextButton::buttonColourId,
                                  juce::Colours::transparentBlack);
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

// ---------------------------------------------------------------------------
// B4-K7: Per-tab .ck eval state
// ---------------------------------------------------------------------------

void HathorTab::setCkEvalState(CkevalState s) noexcept
{
    if (ckEvalState_ == s)
        return;

    ckEvalState_ = s;

    // For .ck tabs, sync the Play/Stop button visual to the eval state.
    // Running = Stop icon (red), Compiling = amber, Error = red (X or stop),
    // Idle = Play icon (default).
    if (useChuckTokeniser_)
    {
        setSlotRunningVisual(s == CkevalState::Running || s == CkevalState::Compiling);
    }

    // Repaint to update the button icon.
    slotPlayButton_.repaint();
}

// ---------------------------------------------------------------------------
// C1: Playback highlight overlay — HighlightOverlay implementation
// ---------------------------------------------------------------------------

void HathorTab::HighlightOverlay::paint(juce::Graphics& g)
{
    if (!active_ || highlightBounds_.isEmpty())
        return;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Draw a semi-transparent accent overlay with a thin outline.
    // Uses palette.accent with high alpha so it reads as "now playing"
    // without obscuring the syntax highlighting beneath.
    g.setColour(palette.accent.withAlpha(0.25f));
    g.fillRect(highlightBounds_);

    g.setColour(palette.accent.withAlpha(0.8f));
    g.drawRect(highlightBounds_, 1);
}

void HathorTab::HighlightOverlay::setHighlight(
    const juce::Rectangle<int>& bounds) noexcept
{
    // Repaint the old highlight region before changing it.
    if (active_)
    {
        const auto oldRect = highlightBounds_.toFloat().expanded(2);
        repaint(oldRect.toNearestInt());
    }

    highlightBounds_ = bounds;
    active_ = true;

    // Repaint the new region.
    const auto newRect = highlightBounds_.toFloat().expanded(2);
    repaint(newRect.toNearestInt());
}

void HathorTab::HighlightOverlay::clearHighlight() noexcept
{
    if (!active_)
        return;
    active_ = false;
    repaint();
}

// ---------------------------------------------------------------------------
// C1: setNowPlayingHighlight / clearNowPlayingHighlight
// ---------------------------------------------------------------------------

void HathorTab::setNowPlayingHighlight(std::size_t sourceOffset,
                                       const juce::Rectangle<int>& glyphBounds) noexcept
{
    highlightBoundsPrev_ = highlightBounds_;
    highlightOffset_     = sourceOffset;
    highlightBounds_     = glyphBounds;
    highlightActive_     = true;

    // The overlay is a child of HathorTab, positioned to cover the editor.
    // glyphBounds is in editor-local coordinates, so translate to the
    // overlay's coordinate space (which matches the editor's since they
    // share the same parent layout).
    highlightOverlay_.setHighlight(glyphBounds);
}

void HathorTab::clearNowPlayingHighlight() noexcept
{
    if (!highlightActive_)
        return;

    highlightActive_     = false;
    highlightBoundsPrev_ = highlightBounds_;
    highlightBounds_     = {};
    highlightOverlay_.clearHighlight();
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

    // Highlight overlay covers the same area as the editor so that glyph-box
    // coordinates (editor-local) map directly to overlay-local coordinates.
    highlightOverlay_.setBounds(0, editorTop, getWidth(), getHeight() - editorTop);
}

void HathorTab::lookAndFeelChanged()
{
    // Rebuild palette-derived colours + syntax colour scheme when the theme
    // switches (B3). The LookAndFeel has already been updated by the time this
    // is called (setPalette() → sendLookAndFeelChange()). JUCE's
    // CodeEditorComponent::lookAndFeelChanged() does not refresh its syntax
    // colour scheme, so the active tokeniser's scheme is re-applied here, all
    // sourced from the current HathorLookAndFeel palette.
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

    // Re-apply the active tokeniser's palette-derived colour scheme so syntax
    // highlighting tracks the new theme.
    if (useChuckTokeniser_)
        editor_.setColourScheme(chuckTokeniser_.getDefaultColourScheme());
    else
        editor_.setColourScheme(miniTokeniser_.getDefaultColourScheme());

    // Re-apply button visual state to sync colours with the new palette.
    setSlotRunningVisual(slotRunning_);

    // C1: repaint the highlight overlay so it picks up the new palette colours.
    highlightOverlay_.repaint();
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
