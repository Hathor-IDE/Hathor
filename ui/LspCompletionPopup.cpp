// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * LspCompletionPopup.cpp — implementation of LspCompletionPopup.
 *
 * Requirement references: AI-4
 */

#include "LspCompletionPopup.hpp"
#include "HathorLookAndFeel.hpp"

#include <algorithm>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

LspCompletionPopup::LspCompletionPopup(SelectCallback onSelect,
                                       DismissCallback onDismiss)
    : onSelect_(std::move(onSelect))
    , onDismiss_(std::move(onDismiss))
    , listBox_()
{
    listBox_.setModel(this);
    listBox_.setRowHeight(kRowHeight);
    listBox_.setMultipleSelectionEnabled(false);
    addAndMakeVisible(listBox_);

    setSize(kPopupWidth, kMaxVisibleRows * kRowHeight);
    setInterceptsMouseClicks(true, true);
}

// ---------------------------------------------------------------------------
// juce::Component
// ---------------------------------------------------------------------------

void LspCompletionPopup::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    g.fillAll(palette.surface);
    g.setColour(palette.accent.withAlpha(0.5f));
    g.drawRect(getLocalBounds(), 1);
}

void LspCompletionPopup::resized()
{
    listBox_.setBounds(getLocalBounds());
}

bool LspCompletionPopup::keyPressed(const juce::KeyPress& key)
{
    if (displayedItems_.empty())
        return false;

    if (key == juce::KeyPress::upKey)
    {
        selectPrevious();
        return true;
    }
    if (key == juce::KeyPress::downKey)
    {
        selectNext();
        return true;
    }
    if (key == juce::KeyPress::returnKey || key == juce::KeyPress::tabKey)
    {
        confirmSelection();
        return true;
    }
    if (key == juce::KeyPress::escapeKey)
    {
        if (onDismiss_)
            onDismiss_();
        return true;
    }

    return false;
}

void LspCompletionPopup::mouseDown(const juce::MouseEvent& e)
{
    if (!listBox_.getBounds().contains(juce::Point<int>(static_cast<int>(e.position.x),
                                                        static_cast<int>(e.position.y))))
    {
        if (onDismiss_)
            onDismiss_();
    }
}

void LspCompletionPopup::mouseMove(const juce::MouseEvent& /*e*/)
{
}

void LspCompletionPopup::mouseUp(const juce::MouseEvent& /*e*/)
{
}

// ---------------------------------------------------------------------------
// juce::ListBoxModel
// ---------------------------------------------------------------------------

int LspCompletionPopup::getNumRows()
{
    return static_cast<int>(displayedItems_.size());
}

void LspCompletionPopup::paintListBoxItem(int row, juce::Graphics& g,
                                           int width, int height, bool rowIsSelected)
{
    if (row < 0 || static_cast<std::size_t>(row) >= displayedItems_.size())
        return;

    const auto& item = displayedItems_[row];
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    if (rowIsSelected)
        g.fillAll(palette.accent.withAlpha(0.2f));

    int x = 4;
    int y = (height - 12) / 2;

    juce::String kindIcon;
    juce::Colour kindColour = palette.codeText;

    switch (item.kind)
    {
        case lsp::CompletionItemKind::Function:
            kindIcon = "ƒ";
            kindColour = palette.accent;
            break;
        case lsp::CompletionItemKind::Value:
            kindIcon = "•";
            kindColour = palette.textSecondary;
            break;
        case lsp::CompletionItemKind::Keyword:
            kindIcon = "¶";
            kindColour = palette.textSecondary;
            break;
        case lsp::CompletionItemKind::Enum:
            kindIcon = "◆";
            kindColour = palette.warning;
            break;
        case lsp::CompletionItemKind::Class:
            kindIcon = "⚬";
            kindColour = palette.error;
            break;
        default:
            kindIcon = "";
            break;
    }

    // Inter UI font for the completion popup (proportional), not JetBrains Mono.
    juce::FontOptions iconFontOpts;
    iconFontOpts = iconFontOpts.withTypeface(HathorLookAndFeel::interRegularTypeface());
    iconFontOpts = iconFontOpts.withHeight(11.0f);
    juce::Font iconFont(iconFontOpts);
    g.setFont(iconFont);

    if (!kindIcon.isEmpty())
    {
        g.setColour(kindColour);
        g.drawText(kindIcon, x, y, 16, height, juce::Justification::centredLeft);
        x += 18;
    }

    juce::Colour labelColour = rowIsSelected ? palette.textPrimary : palette.codeText;
    g.setColour(labelColour);

    juce::FontOptions labelFontOpts;
    labelFontOpts = labelFontOpts.withTypeface(HathorLookAndFeel::interRegularTypeface());
    labelFontOpts = labelFontOpts.withHeight(12.0f);
    g.setFont(juce::Font(labelFontOpts));

    juce::String labelStr(item.label);
    g.drawText(labelStr, x, y, width - x - 4, height, juce::Justification::centredLeft);

    if (!item.detail.empty())
    {
        g.setColour(palette.textSecondary);

        juce::FontOptions detailFontOpts;
        detailFontOpts = detailFontOpts.withTypeface(HathorLookAndFeel::interRegularTypeface());
        detailFontOpts = detailFontOpts.withHeight(10.0f);
        juce::Font detailFont(detailFontOpts);
        g.setFont(detailFont);

        juce::String detailStr(item.detail);
        juce::AttributedString detailAttr;
        detailAttr.append(detailStr, detailFont, palette.textSecondary);
        juce::TextLayout detailLayout;
        detailLayout.createLayout(detailAttr, kPopupWidth);
        int detailWidth = static_cast<int>(detailLayout.getWidth());
        g.drawText(detailStr, width - detailWidth - 4, y, detailWidth, height,
                   juce::Justification::centredRight);
    }
}

// ---------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

void LspCompletionPopup::setCandidates(const std::vector<lsp::CompletionCandidate>& candidates)
{
    allItems_ = candidates;
    prefix_.clear();
    rebuildDisplay();
    selectedIndex_ = 0;
    updateListBoxSize();
    repaint();
}

void LspCompletionPopup::addCandidates(const std::vector<lsp::CompletionCandidate>& candidates)
{
    for (const auto& c : candidates)
    {
        bool found = false;
        for (const auto& existing : allItems_)
        {
            if (existing.label == c.label)
            {
                found = true;
                break;
            }
        }
        if (!found)
            allItems_.push_back(c);
    }
    rebuildDisplay();
    selectedIndex_ = 0;
    updateListBoxSize();
    repaint();
}

void LspCompletionPopup::filterPrefix(std::string_view prefix)
{
    prefix_ = std::string(prefix);
    rebuildDisplay();
    selectedIndex_ = 0;

    if (displayedItems_.empty() && !allItems_.empty())
    {
        if (onDismiss_)
            onDismiss_();
        return;
    }

    updateListBoxSize();
    repaint();
}

void LspCompletionPopup::selectFirst() noexcept
{
    selectedIndex_ = 0;
    listBox_.selectRow(0);
}

void LspCompletionPopup::selectLast() noexcept
{
    selectedIndex_ = static_cast<int>(displayedItems_.size()) - 1;
    listBox_.selectRow(selectedIndex_);
}

void LspCompletionPopup::selectPrevious() noexcept
{
    if (displayedItems_.empty())
        return;
    selectedIndex_ = (selectedIndex_ - 1 + static_cast<int>(displayedItems_.size()))
                     % static_cast<int>(displayedItems_.size());
    listBox_.selectRow(selectedIndex_);
}

void LspCompletionPopup::selectNext() noexcept
{
    if (displayedItems_.empty())
        return;
    selectedIndex_ = (selectedIndex_ + 1) % static_cast<int>(displayedItems_.size());
    listBox_.selectRow(selectedIndex_);
}

void LspCompletionPopup::confirmSelection()
{
    if (selectedIndex_ >= 0 && static_cast<std::size_t>(selectedIndex_) < displayedItems_.size())
    {
        if (onSelect_)
            onSelect_(displayedItems_[selectedIndex_]);
    }
    dismiss();
}

void LspCompletionPopup::dismiss()
{
    if (onDismiss_)
        onDismiss_();
    setVisible(false);
}

const lsp::CompletionCandidate* LspCompletionPopup::selectedCandidate() const noexcept
{
    if (selectedIndex_ >= 0 && static_cast<std::size_t>(selectedIndex_) < displayedItems_.size())
        return &displayedItems_[selectedIndex_];
    return nullptr;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void LspCompletionPopup::rebuildDisplay()
{
    displayedItems_.clear();

    for (const auto& item : allItems_)
    {
        if (prefix_.empty() || matchesPrefix(item.label, prefix_))
            displayedItems_.push_back(item);
    }

    listBox_.updateContent();
}

bool LspCompletionPopup::matchesPrefix(std::string_view label, std::string_view prefix) noexcept
{
    if (prefix.empty())
        return true;
    if (label.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(label[i]))
            != std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

void LspCompletionPopup::updateListBoxSize()
{
    int rows = std::min(static_cast<int>(displayedItems_.size()), kMaxVisibleRows);
    int height = rows * kRowHeight;
    listBox_.setBounds(0, 0, kPopupWidth, height);
    setSize(kPopupWidth, height);
}

} // namespace hathor::ui
