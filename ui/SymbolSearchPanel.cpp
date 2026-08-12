// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * SymbolSearchPanel.cpp — implementation of the symbol search panel.
 *
 * JUCE-dependent. Uses the JUCE-free SymbolSearchModel for search logic.
 *
 * Requirement references: L-2 §3
 */

#include "SymbolSearchPanel.hpp"

namespace hathor::ui {

SymbolSearchPanel::SymbolSearchPanel(SymbolSearchModel* model)
    : model_(model)
{
    searchField_ = std::make_unique<juce::TextEditor>();
    searchField_->setListener(this);
    searchField_->setFont(juce::FontOptions{16.0f});
    searchField_->setColour(juce::TextEditor::backgroundColourId,
                            juce::Colours::black.withAlpha(0.7f));
    searchField_->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    searchField_->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
    searchField_->setInputRestrictions(0, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:/\\");
    addAndMakeVisible(searchField_.get());

    hintLabel_ = std::make_unique<juce::Label>();
    hintLabel_->setText("Search symbols (Esc to close)", juce::dontSendNotification);
    hintLabel_->setFont(juce::FontOptions{14.0f});
    hintLabel_->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.7f));
    hintLabel_->setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(hintLabel_.get());

    listBox_ = std::make_unique<juce::ListBox>();
    listBox_->setModel(this);
    listBox_->setFont(juce::FontOptions{15.0f});
    listBox_->setColour(juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha(0.9f));
    listBox_->setColour(juce::ListBox::outlineColourId, juce::Colours::white.withAlpha(0.1f));
    listBox_->setColour(juce::ListBox::selectedRowBackgroundColourId, juce::Colours::white.withAlpha(0.15f));
    listBox_->setRowSelectedOnMouseDown(true);
    listBox_->setRowSelectedOnMouseUp(true);
    listBox_->onDoubleClick = [this]() {
        if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(displayResults_.size()))
        {
            if (onSymbolSelected)
                onSymbolSelected(displayResults_[selectedIndex_]);
            setVisible(false);
        }
    };
    addAndMakeVisible(listBox_.get());

    closeBtn_ = std::make_unique<juce::TextButton>("x");
    closeBtn_->setBounds(0, 0, 20, 20);
    closeBtn_->onClick = [this]() {
        setVisible(false);
        if (onClosePanel)
            onClosePanel();
    };
    addAndMakeVisible(closeBtn_.get());

    reloadResults();
}

SymbolSearchPanel::~SymbolSearchPanel()
{
    listBox_->setModel(nullptr);
}

void SymbolSearchPanel::setQuery(const juce::String& query)
{
    if (model_)
    {
        model_->searchMetadata(query.toStdString());
        reloadResults();
    }

    selectedIndex_ = 0;
    listBox_->updateContent();
    listBox_->selectRow(0);

    if (isVisible() && searchField_)
        searchField_->setText(query, juce::dontSendNotification);
}

void SymbolSearchPanel::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
    if (visible)
    {
        if (searchField_)
            searchField_->setText({}, juce::dontSendNotification);
        if (model_)
        {
            model_->searchMetadata("");
            reloadResults();
        }
        searchField_->grabKeyboardFocus();
    }
}

void SymbolSearchPanel::clear()
{
    displayResults_.clear();
    if (listBox_)
    {
        listBox_->updateContent();
        listBox_->selectedRowsChanged(-1);
    }
}

void SymbolSearchPanel::resized()
{
    const int margin = 8;
    const int fieldHeight = 28;
    const int hintHeight = 20;
    const int closeSize = 20;

    closeBtn_->setBounds(getWidth() - margin - closeSize, margin, closeSize, closeSize);
    hintLabel_->setBounds(margin, margin, getWidth() - 2 * margin - closeSize, hintHeight);
    searchField_->setBounds(margin, margin + hintHeight + 4, getWidth() - 2 * margin, fieldHeight);
    listBox_->setBounds(margin, margin + hintHeight + 4 + fieldHeight + 8,
                        getWidth() - 2 * margin, getHeight() - hintHeight - fieldHeight - 4 * margin - closeSize);
}

void SymbolSearchPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.92f));
}

void SymbolSearchPanel::reloadResults()
{
    displayResults_.clear();
    if (model_)
    {
        const auto& results = model_->results();
        displayResults_ = std::vector<SymbolSearchResult>(results.begin(), results.end());
    }
    selectedIndex_ = 0;
    listBox_->updateContent();
    listBox_->selectRow(0);
}

// ---------------------------------------------------------------------------
// TextEditor::Listener
// ---------------------------------------------------------------------------

void SymbolSearchPanel::textEditorTextChanged(juce::TextEditor& /*editor*/)
{
    if (model_)
    {
        model_->searchMetadata(searchField_->getText().toStdString());
        reloadResults();
    }
}

void SymbolSearchPanel::textEditorEscapeKeyPressed(juce::TextEditor& /*editor*/)
{
    setVisible(false);
    if (onClosePanel)
        onClosePanel();
}

void SymbolSearchPanel::textEditorReturnKeyPressed(juce::TextEditor& /*editor*/)
{
    if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(displayResults_.size()))
    {
        if (onSymbolSelected)
            onSymbolSelected(displayResults_[selectedIndex_]);
        setVisible(false);
    }
}

bool SymbolSearchPanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::upKey)
    {
        if (selectedIndex_ > 0)
        {
            selectedIndex_--;
            if (listBox_)
                listBox_->selectRow(selectedIndex_);
        }
    }
    else if (key == juce::KeyPress::downKey)
    {
        if (selectedIndex_ < static_cast<int>(displayResults_.size()) - 1)
        {
            selectedIndex_++;
            if (listBox_)
                listBox_->selectRow(selectedIndex_);
        }
    }
    return false;
}

void SymbolSearchPanel::paintListBoxItem(int row, juce::Graphics& g,
                                          int width, int height, bool isSelected)
{
    juce::Rectangle<int> bounds(0, 0, width, height);

    if (isSelected)
        g.fillAll(juce::Colours::white.withAlpha(0.15f));
    else
        g.fillAll(juce::Colours::black.withAlpha(0.85f));

    if (row >= 0 && row < static_cast<int>(displayResults_.size()))
    {
        const auto& sym = displayResults_[row];

        // Left: symbol name
        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions{15.0f}.withBold());
        juce::String nameText = juce::String(sym.name.c_str());
        g.drawText(nameText, bounds.removeFromLeft(width / 3).reduced(4, 2),
                   juce::Justification::centredLeft, false);

        // Middle: kind + detail
        juce::String kindText = juce::String(sym.kind.c_str()) + "  " + juce::String(sym.detail.c_str());
        g.setFont(juce::FontOptions{13.0f});
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.drawText(kindText, bounds.removeFromLeft(width / 3).reduced(4, 2),
                   juce::Justification::centredLeft, false);

        // Right: container/path
        juce::String containerText = juce::String(sym.containerName.c_str());
        if (containerText.isEmpty() && !sym.uri.empty() && !sym.isBuiltin)
            containerText = sym.filePath.filename().string().c_str();
        else if (sym.isBuiltin)
            containerText = "builtin";
        g.drawText(containerText, bounds.reduced(4, 2),
                   juce::Justification::centredLeft, false);
    }
}

void SymbolSearchPanel::selectedRowsChanged(int lastSelectedRow)
{
    selectedIndex_ = lastSelectedRow;
}

} // namespace hathor::ui
