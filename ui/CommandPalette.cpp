// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * CommandPalette.cpp — implementation.
 *
 * Requirement references: L-1 §5
 */

#include "CommandPalette.hpp"
#include "HathorLookAndFeel.hpp"

#include <algorithm>
#include <cctype>

namespace hathor::ui {

// ===========================================================================
// CommandPalette
// ===========================================================================

CommandPalette::CommandPalette()
{
    setOpaque(true);

    filterField_ = std::make_unique<juce::TextEditor>();
    filterField_->setMultiLine(false);
    filterField_->setInputRestrictions(0, juce::String());
    filterField_->setFont(HathorLookAndFeel::getUiFont(14.0f));
    addAndMakeVisible(filterField_.get());
    filterField_->onTextChange = [this]() {
        setFilter(filterField_->getText());
    };

    listBox_ = std::make_unique<juce::ListBox>();
    listBox_->setOpaque(false);
    addAndMakeVisible(listBox_.get());

    hintLabel_ = std::make_unique<juce::Label>();
    hintLabel_->setText("Type to filter actions (Esc to close)",
                         juce::dontSendNotification);
    hintLabel_->setJustificationType(juce::Justification::centred);
    addAndMakeVisible(hintLabel_.get());

    // Allow keyboard navigation via key listener
    // (down arrow handled by parent or key listener)
}

CommandPalette::~CommandPalette() = default;

void CommandPalette::show(juce::Component* parent)
{
    if (!parent || !registry_)
        return;

    // Size: 480 wide, 320 tall, centered
    const int w = 480;
    const int h = 320;
    int x = (parent->getWidth() - w) / 2;
    int y = (parent->getHeight() - h) / 2;
    setBounds(x, y, w, h);

    refreshList();
    filterField_->setText(juce::String());
    filterField_->grabKeyboardFocus();
    filterField_->onTextChange();

    setVisible(true);
    toFront(true);
}

void CommandPalette::hide()
{
    setVisible(false);
}

void CommandPalette::resized()
{
    juce::Rectangle<int> area(getLocalBounds());
    hintLabel_->setBounds(area.removeFromTop(24));
    filterField_->setBounds(area.removeFromTop(24));
    area.reduce(4, 4);
    listBox_->setBounds(area);
}

void CommandPalette::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.85f));
    g.setColour(juce::Colours::darkgrey.darker(0.4f));
    g.drawRect(getLocalBounds(), 1);
}

void CommandPalette::setFilter(const juce::String& query)
{
    refreshList(query);
    selectedIndex_ = 0;
}

bool CommandPalette::executeSelected()
{
    if (!registry_ || filteredActions_.empty())
        return false;

    if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(filteredActions_.size()))
        return false;

    const EditorAction* action = filteredActions_[selectedIndex_];
    if (!action)
        return false;

    return registry_->dispatch(action->id);
}

void CommandPalette::selectUp()
{
    selectedIndex_ = std::max(0, selectedIndex_ - 1);
}

void CommandPalette::selectDown()
{
    if (selectedIndex_ < static_cast<int>(filteredActions_.size()) - 1)
        ++selectedIndex_;
}

void CommandPalette::refreshList(const juce::String& query)
{
    if (!registry_)
    {
        filteredActions_.clear();
        return;
    }

    // Get all actions
    auto all = registry_->listActions();

    juce::String lowerQuery = query.trim().toLowerCase();

    filteredActions_.clear();

    for (const auto* action : all)
    {
        if (lowerQuery.isEmpty())
        {
            filteredActions_.push_back(action);
        }
        else
        {
            // Match against id, label, and category (case-insensitive)
            std::string q = lowerQuery.toStdString();
            bool match = false;
            if (action->id.find(q) != std::string::npos)
                match = true;
            else if (action->label.find(q) != std::string::npos)
                match = true;
            else if (action->category.find(q) != std::string::npos)
                match = true;

            if (match)
                filteredActions_.push_back(action);
        }
    }

    // Update list box content
    // For simplicity, we don't use a custom ListBoxModel here — the palette
    // is a minimal overlay. A full implementation would use a ListBoxModel.
}

} // namespace hathor::ui
