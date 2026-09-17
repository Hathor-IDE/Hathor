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

// Forwards navigation keys from the filter field (which holds keyboard
// focus) to the palette: TextEditor would otherwise consume Up/Down/Return.
class PaletteKeyForwarder : public juce::KeyListener
{
public:
    explicit PaletteKeyForwarder(CommandPalette& owner) : owner_(owner) {}

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        if (key == juce::KeyPress::upKey
            || key == juce::KeyPress::downKey
            || key == juce::KeyPress::returnKey
            || key == juce::KeyPress::escapeKey)
            return owner_.keyPressed(key);
        return false;
    }

private:
    CommandPalette& owner_;
};

// ===========================================================================
// CommandPalette
// ===========================================================================

CommandPalette::CommandPalette()
{
    setOpaque(true);
    setTitle("Command Palette");
    setDescription("Type to filter actions. Arrow keys move, Enter runs, Escape closes.");

    filterField_ = std::make_unique<juce::TextEditor>();
    filterField_->setMultiLine(false);
    filterField_->setInputRestrictions(0, juce::String());
    filterField_->setFont(HathorLookAndFeel::getUiFont(14.0f));
    addAndMakeVisible(filterField_.get());
    filterField_->onTextChange = [this]() {
        setFilter(filterField_->getText());
    };
    keyForwarder_ = std::make_unique<PaletteKeyForwarder>(*this);
    filterField_->addKeyListener(keyForwarder_.get());

    listBox_ = std::make_unique<juce::ListBox>();
    listBox_->setOpaque(false);
    listBox_->setModel(this);
    listBox_->setRowHeight(26);
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

    // Size: 480 wide, 320 tall, centered and clamped into the parent.
    const int w = std::min(480, std::max(280, parent->getWidth() - 40));
    const int h = std::min(320, std::max(200, parent->getHeight() - 40));
    int x = (parent->getWidth() - w) / 2;
    int y = (parent->getHeight() - h) / 3;
    setBounds(x, y, w, h);

    refreshList();
    filterField_->setText(juce::String());
    filterField_->grabKeyboardFocus();

    setVisible(true);
    toFront(true);
}

void CommandPalette::hide()
{
    setVisible(false);
    filterField_->setText(juce::String(), juce::dontSendNotification);
}

bool CommandPalette::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        hide();
        return true;
    }
    if (key == juce::KeyPress::returnKey)
    {
        if (executeSelected())
            hide();
        return true;
    }
    if (key == juce::KeyPress::upKey)
    {
        selectUp();
        return true;
    }
    if (key == juce::KeyPress::downKey)
    {
        selectDown();
        return true;
    }
    return false;
}

int CommandPalette::getNumRows()
{
    return static_cast<int>(filteredActions_.size());
}

void CommandPalette::paintListBoxItem(int rowNumber, juce::Graphics& g,
                                      int width, int height,
                                      bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int>(filteredActions_.size()))
        return;
    const EditorAction* action = filteredActions_[static_cast<size_t>(rowNumber)];
    if (action == nullptr)
        return;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    if (rowIsSelected)
    {
        g.setColour(palette.accent.withAlpha(0.3f));
        g.fillAll();
    }
    g.setColour(palette.textPrimary);
    g.setFont(HathorLookAndFeel::uiFontRegular(13.0f));
    g.drawText(action->label, 8, 0, width - 16, height,
               juce::Justification::centredLeft, true);
    g.setColour(palette.textSecondary);
    g.setFont(HathorLookAndFeel::uiFontRegular(11.0f));
    g.drawText(action->category, 8, 0, width - 16, height,
               juce::Justification::centredRight, true);
}

void CommandPalette::listBoxItemDoubleClicked(int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < static_cast<int>(filteredActions_.size()))
    {
        selectedIndex_ = row;
        if (executeSelected())
            hide();
    }
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
    if (listBox_)
    {
        listBox_->selectRow(selectedIndex_);
        listBox_->scrollToEnsureRowIsOnscreen(selectedIndex_);
    }
}

void CommandPalette::selectDown()
{
    if (selectedIndex_ < static_cast<int>(filteredActions_.size()) - 1)
        ++selectedIndex_;
    if (listBox_)
    {
        listBox_->selectRow(selectedIndex_);
        listBox_->scrollToEnsureRowIsOnscreen(selectedIndex_);
    }
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
            // Match against id, label, and category (case-insensitive:
            // lowercase both sides before comparing).
            std::string q = lowerQuery.toStdString();
            auto lowerOf = [](const std::string& s) {
                std::string out = s;
                std::transform(out.begin(), out.end(), out.begin(),
                               [](unsigned char c) { return (char) std::tolower(c); });
                return out;
            };
            bool match = false;
            if (lowerOf(action->id).find(q) != std::string::npos)
                match = true;
            else if (lowerOf(action->label).find(q) != std::string::npos)
                match = true;
            else if (lowerOf(action->category).find(q) != std::string::npos)
                match = true;

            if (match)
                filteredActions_.push_back(action);
        }
    }

    selectedIndex_ = 0;
    if (listBox_)
    {
        listBox_->updateContent();
        if (!filteredActions_.empty())
            listBox_->selectRow(0);
    }
}

} // namespace hathor::ui
