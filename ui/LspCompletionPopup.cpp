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
    listBox_.setHeaderHeight(0);
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

    // Background
    g.fillAll(palette.surface);

    // Border
    g.setColour(palette.accent.withAlpha(0.5f));
    g.drawRect(getLocalBounds(), 1);
}

void LspCompletionPopup::resized()
{
    listBox_.setBounds(getLocalBounds());
}

bool LspCompletionPopup::keyDown(const juce::KeyPress& key)
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

    // Let the user type to filter — the parent handler will call filterPrefix
    return false;
}

void LspCompletionPopup::mouseDown(const juce::MouseEvent& e)
{
    // Click on the popup background — keep it open
    // Click selection is handled by the list box
    if (!listBox_.getBounds().contains(e.position))
    {
        // Clicked outside the list box area
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
    {
        g.fillAll(palette.accent.withAlpha(0.2f));
    }

    // Left padding
    int x = 4;
    int y = (height - 12) / 2;

    // Icon / kind indicator
    juce::String kindIcon;
    juce::Colour kindColour = palette.codeText;\n\n";
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

    // Draw kind icon
    if (!kindIcon.isEmpty())
    {
        g.setColour(kindColour);
        g.setFont(juce::Font(juce::Font::getDefaultMonospaceFontName(), 11.0f, juce::Font::plain));
        g.drawText(kindIcon, x, y, 16, height, juce::Justification::centredLeft);
        x += 18;
    }

    // Label
    juce::Colour labelColour = rowIsSelected ? palette.textPrimary : palette.codeText;
    g.setColour(labelColour);
    g.setFont(juce::Font(juce::Font::getDefaultMonospaceFontName(), 12.0f, juce::Font::plain));
    g.drawText(item.label, x, y, width - x - 4, height, juce::Justification::centredLeft);

    // Detail (right-aligned)
    if (!item.detail.empty())
    {
        g.setColour(palette.textSecondary);
        g.setFont(juce::Font(juce::Font::getDefaultMonospaceFontName(), 10.0f, juce::Font::plain));
        juce::String detailStr(item.detail);
        int detailWidth = juce::Font(juce::Font::getDefaultMonospaceFontName(), 10.0f, juce::Font::plain)
                              .getStringWidth(detailStr);
        g.drawText(detailStr, width - detailWidth - 4, y, detailWidth, height,
                   juce::Justification::centredRight);
    }
}

juce::Component* LspCompletionPopup::createSnapshot()
{
    return nullptr; // No per-row components
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
        // Check for duplicates (by label)
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
        // All filtered out — dismiss
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
}

const lsp::CompletionCandidate* LspCompletionPopup::selectedCandidate() const noexcept
{
    if (selectedIndex_ >= 0 && static_cast<std::size_t>(selectedIndex_) < displayedItems_.size())
        return &displayedItems_[selectedIndex_];
    return nullptr;
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

bool LspCompletionPopup::matchesPrefix(std::string_view label, std::string_view prefix) noexcept
{
    if (prefix.empty())
        return true;
    if (label.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        char l = static_cast<char>(std::tolower(static_cast<unsigned char>(label[i])));
        char p = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (l != p)
            return false;
    }
    return true;
}

void LspCompletionPopup::rebuildDisplay()
{
    displayedItems_.clear();
    for (const auto& item : allItems_)
    {
        if (matchesPrefix(item.label, prefix_))
            displayedItems_.push_back(item);
    }
}

void LspCompletionPopup::updateListBoxSize()
{
    int rows = static_cast<int>(displayedItems_.size());
    int visibleRows = std::min(rows, kMaxVisibleRows);
    if (visibleRows == 0)
        visibleRows = 1; // show empty placeholder

    int popupHeight = visibleRows * kRowHeight;
    int popupWidth = kPopupWidth;
    setSize(popupWidth, popupHeight);
    listBox_.setBounds(getLocalBounds());
}

} // namespace hathor::ui
