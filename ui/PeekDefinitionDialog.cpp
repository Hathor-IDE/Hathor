// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * PeekDefinitionDialog.cpp — implementation.
 *
 * Requirement references: L-2 (Peek Definition, AI-4 LSP wiring).
 */

#include "PeekDefinitionDialog.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PeekDefinitionDialog::PeekDefinitionDialog(std::vector<PeekDefinitionEntry> entries,
                                           NavigateCallback onNavigate)
    : entries_(std::move(entries))
    , onNavigate_(std::move(onNavigate))
{
    jassert(!entries_.empty());

    const auto& palette = HathorLookAndFeel::defaultPalette();

    // Title
    titleLabel_ = std::make_unique<juce::Label>();
    titleLabel_->setText("Peek Definition", juce::dontSendNotification);
    titleLabel_->setFont(HathorLookAndFeel::uiFontSemiBold(18.0f));
    titleLabel_->setColour(juce::Label::textColourId, palette.textPrimary);
    titleLabel_->setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(titleLabel_.get());

    // Location label (file : line)
    locationLabel_ = std::make_unique<juce::Label>();
    locationLabel_->setFont(HathorLookAndFeel::uiFontMedium(12.0f));
    locationLabel_->setColour(juce::Label::textColourId, palette.textSecondary);
    locationLabel_->setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(locationLabel_.get());

    // Source context viewer (read-only)
    sourceView_ = std::make_unique<juce::TextEditor>();
    sourceView_->setReadOnly(true);
    sourceView_->setMultiLine(true, false);
    sourceView_->setFont(HathorLookAndFeel::fontRegular(13.0f));
    sourceView_->setColour(juce::TextEditor::backgroundColourId, palette.surface);
    sourceView_->setColour(juce::TextEditor::textColourId, palette.codeText);
    sourceView_->setColour(juce::TextEditor::outlineColourId, palette.surfaceHighest);
    sourceView_->setColour(juce::CaretComponent::caretColourId, palette.accent);
    sourceView_->setColour(juce::TextEditor::highlightColourId, palette.accent.withAlpha(0.25f));
    sourceView_->setScrollToShowCursor(false);
    addAndMakeVisible(sourceView_.get());

    // List box — only when there is more than one definition.
    if (entries_.size() > 1)
    {
        listBox_ = std::make_unique<juce::ListBox>();
        listBox_->setModel(this);
        listBox_->setColour(juce::ListBox::backgroundColourId, palette.surfaceContainer);
        listBox_->setColour(juce::ListBox::outlineColourId, palette.surfaceHighest);
        addAndMakeVisible(listBox_.get());
    }

    // Buttons
    goToBtn_ = std::make_unique<juce::TextButton>("Go to Definition");
    goToBtn_->setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    goToBtn_->addListener(this);
    addAndMakeVisible(goToBtn_.get());

    closeBtn_ = std::make_unique<juce::TextButton>("Close");
    closeBtn_->setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    closeBtn_->addListener(this);
    addAndMakeVisible(closeBtn_.get());

    // Select the first entry and reflect it.
    selectedEntry_ = 0;
    if (listBox_)
    {
        listBox_->selectRow(0);
        listBox_->setVisible(true);
    }
    updateSelection();

    // Focus the source view so the user can read / select immediately.
    sourceView_->grabKeyboardFocus();
}

PeekDefinitionDialog::~PeekDefinitionDialog()
{
    if (listBox_)
        listBox_->setModel(nullptr);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void PeekDefinitionDialog::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    g.fillAll(palette.surfaceContainer);
}

void PeekDefinitionDialog::resized()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    juce::ignoreUnused(palette);

    auto area = getLocalBounds().reduced(12);

    titleLabel_->setBounds(area.removeFromTop(26));
    area.removeFromTop(6);
    locationLabel_->setBounds(area.removeFromTop(18));
    area.removeFromTop(8);

    const int buttonHeight = 30;
    auto contentArea = area.removeFromTop(area.getHeight() - buttonHeight - 6);

    if (listBox_ != nullptr)
    {
        const int listWidth = 260;
        auto listArea = contentArea.removeFromLeft(listWidth);
        listBox_->setBounds(listArea);
        sourceView_->setBounds(contentArea.reduced(4, 0));
    }
    else
    {
        sourceView_->setBounds(contentArea);
    }

    auto buttonRow = area.removeFromBottom(buttonHeight);
    const int btnW = 130;
    closeBtn_->setBounds(buttonRow.removeFromLeft(btnW));
    buttonRow = buttonRow.reduced(8, 0);
    goToBtn_->setBounds(buttonRow.removeFromRight(btnW));
}

// ---------------------------------------------------------------------------
// Key handling
// ---------------------------------------------------------------------------

bool PeekDefinitionDialog::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Button::Listener
// ---------------------------------------------------------------------------

void PeekDefinitionDialog::buttonClicked(juce::Button* button)
{
    if (button == goToBtn_.get())
    {
        if (onNavigate_ && !entries_.empty())
            onNavigate_(entries_[juce::jlimit(0, static_cast<int>(entries_.size()) - 1, selectedEntry_)].location);

        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }
    else if (button == closeBtn_.get())
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState(0);
    }
}

// ---------------------------------------------------------------------------
// ListBoxModel
// ---------------------------------------------------------------------------

int PeekDefinitionDialog::getNumRows()
{
    return static_cast<int>(entries_.size());
}

void PeekDefinitionDialog::paintListBoxItem(int row, juce::Graphics& g,
                                            int width, int height, bool isSelected)
{
    juce::Rectangle<int> bounds(0, 0, width, height);

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    if (isSelected)
        g.fillAll(palette.accent.withAlpha(0.25f));
    else
        g.fillAll(palette.surfaceHigh);

    const auto& e = entries_[juce::jlimit(0, static_cast<int>(entries_.size()) - 1, row)];

    juce::String text = e.fileLabel + " : line " + juce::String(e.line1);

    g.setColour(isSelected ? palette.accent : palette.textPrimary);
    g.setFont(HathorLookAndFeel::uiFontMedium(13.0f));
    g.drawFittedText(text, bounds.reduced(8, 2), juce::Justification::centredLeft, 1);
}

void PeekDefinitionDialog::selectedRowsChanged(int lastSelectedRow)
{
    if (lastSelectedRow >= 0 && lastSelectedRow < static_cast<int>(entries_.size()))
        selectedEntry_ = lastSelectedRow;

    updateSelection();
}

// ---------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------

void PeekDefinitionDialog::updateSelection()
{
    const int clamped = juce::jlimit(0, static_cast<int>(entries_.size()) - 1, selectedEntry_);
    const auto& e = entries_[clamped];

    locationLabel_->setText(e.fileLabel + " : line " + juce::String(e.line1),
                            juce::dontSendNotification);

    juce::String text = e.sourceText;
    if (text.isEmpty())
        text = "<definition source unavailable>";

    sourceView_->setText(text, juce::dontSendNotification);
    sourceView_->setCaretPosition(0);
}

// ---------------------------------------------------------------------------
// Modal launcher
// ---------------------------------------------------------------------------

void showPeekDefinition(juce::Component* parent,
                        std::vector<PeekDefinitionEntry> entries,
                        PeekDefinitionDialog::NavigateCallback onNavigate)
{
    if (!parent || entries.empty())
        return;

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle            = "Peek Definition";
    opts.dialogBackgroundColour = HathorLookAndFeel::defaultPalette().surfaceContainer;
    opts.content.setOwned(new PeekDefinitionDialog(std::move(entries), std::move(onNavigate)));
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar    = false;
    opts.resizable            = false;
    opts.componentToCentreAround = parent;

#if JUCE_MODAL_LOOPS_PERMITTED
    opts.launchAsync();
#else
    juce::ignoreUnused(parent);
#endif
}

} // namespace hathor::ui
