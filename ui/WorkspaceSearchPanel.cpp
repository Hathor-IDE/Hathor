// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WorkspaceSearchPanel.cpp — implementation of the workspace search panel.
 *
 * JUCE-dependent. Uses the JUCE-free WorkspaceSearchModel for the actual
 * search logic; this class only handles UI rendering and user interaction.
 *
 * Requirement references: L-2 §1
 */

#include "WorkspaceSearchPanel.hpp"

#include <algorithm>
#include <cctype>

namespace hathor::ui {

class WorkspaceSearchDoubleClickHandler : public juce::MouseListener
{
public:
    explicit WorkspaceSearchDoubleClickHandler(WorkspaceSearchPanel* parent) : parent_(parent) {}
    void mouseDoubleClick(const juce::MouseEvent&) override {
        int idx = parent_->selectedIndex_;
        if (idx >= 0 && idx < static_cast<int>(parent_->displayItems_.size()))
        {
            const auto& item = parent_->displayItems_[idx];
            if (parent_->onNavigateToMatch)
                parent_->onNavigateToMatch(item.filePath, item.match->line, item.match->column);
        }
    }
private:
    WorkspaceSearchPanel* parent_;
};

WorkspaceSearchPanel::WorkspaceSearchPanel(std::filesystem::path workspaceRoot,
                                             WorkspaceSearchModel* model)
    : workspaceRoot_(std::move(workspaceRoot))
    , model_(model)
{
    searchField_ = std::make_unique<juce::TextEditor>();
    searchField_->addListener(this);
    searchField_->setFont(juce::FontOptions{15.0f});
    searchField_->setColour(juce::TextEditor::backgroundColourId,
                            juce::Colours::black.withAlpha(0.7f));
    searchField_->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    searchField_->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
    searchField_->setInputRestrictions(0, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:/\\ \"'()");
    addAndMakeVisible(searchField_.get());

    replaceField_ = std::make_unique<juce::TextEditor>();
    replaceField_->setFont(juce::FontOptions{15.0f});
    replaceField_->setColour(juce::TextEditor::backgroundColourId,
                             juce::Colours::black.withAlpha(0.7f));
    replaceField_->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    replaceField_->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
    addAndMakeVisible(replaceField_.get());

    searchBtn_ = std::make_unique<juce::TextButton>("Search");
    searchBtn_->onClick = [this]() {
        startSearch(searchField_->getText(), currentFlags());
    };
    addAndMakeVisible(searchBtn_.get());

    replaceAllBtn_ = std::make_unique<juce::TextButton>("Replace All");
    replaceAllBtn_->onClick = [this]() {
        auto query = searchField_->getText();
        auto replacement = replaceField_->getText();
        if (query.isNotEmpty() && model_)
        {
            for (const auto& fileResult : model_->results())
            {
                model_->replaceInFile(fileResult.filePath,
                                      query.toStdString(),
                                      replacement.toStdString(),
                                      currentFlags());
            }
        }
    };
    addAndMakeVisible(replaceAllBtn_.get());

    closeBtn_ = std::make_unique<juce::TextButton>("x");
    closeBtn_->setBounds(0, 0, 20, 20);
    closeBtn_->onClick = [this]() {
        setVisible(false);
        if (onClosePanel)
            onClosePanel();
    };
    addAndMakeVisible(closeBtn_.get());

    regexCheckbox_ = std::make_unique<juce::ToggleButton>("Regex");
    addAndMakeVisible(regexCheckbox_.get());

    caseSensitiveCheckbox_ = std::make_unique<juce::ToggleButton>("Aa");
    caseSensitiveCheckbox_->setTooltip("Case sensitive");
    addAndMakeVisible(caseSensitiveCheckbox_.get());

    wholeWordCheckbox_ = std::make_unique<juce::ToggleButton>("Whole Word");
    addAndMakeVisible(wholeWordCheckbox_.get());

    hintLabel_ = std::make_unique<juce::Label>();
    hintLabel_->setText("Workspace search (Esc to close)", juce::dontSendNotification);
    hintLabel_->setFont(juce::FontOptions{14.0f});
    hintLabel_->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.7f));
    addAndMakeVisible(hintLabel_.get());

    listBox_ = std::make_unique<juce::ListBox>();
    listBox_->setModel(this);
    listBox_->setColour(juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha(0.9f));
    listBox_->setColour(juce::ListBox::outlineColourId, juce::Colours::white.withAlpha(0.1f));
    listBox_->setRowSelectedOnMouseDown(true);
    listBox_->addMouseListener(new WorkspaceSearchDoubleClickHandler(this), true);
    addAndMakeVisible(listBox_.get());
}

WorkspaceSearchPanel::~WorkspaceSearchPanel()
{
    listBox_->setModel(nullptr);
}

WorkspaceSearchFlags WorkspaceSearchPanel::currentFlags() const
{
    WorkspaceSearchFlags flags;
    flags.caseSensitive = caseSensitiveCheckbox_ ? caseSensitiveCheckbox_->getToggleState() : false;
    flags.useRegex = regexCheckbox_ ? regexCheckbox_->getToggleState() : false;
    flags.wholeWord = wholeWordCheckbox_ ? wholeWordCheckbox_->getToggleState() : false;
    return flags;
}

void WorkspaceSearchPanel::startSearch(const juce::String& query,
                                        const WorkspaceSearchFlags& flags)
{
    if (!model_ || query.isEmpty())
        return;

    model_->search(query.toStdString(), flags);

    displayItems_.clear();
    for (const auto& fileResult : model_->results())
    {
        auto relative = std::filesystem::relative(fileResult.filePath, workspaceRoot_);
        std::string relativeStr = relative.string();

        for (const auto& match : fileResult.matches)
        {
            DisplayItem item;
            item.filePath = fileResult.filePath;
            item.relativePath = relativeStr;
            item.match = &match;
            item.fileResult = &fileResult;
            displayItems_.push_back(std::move(item));
        }
    }

    selectedIndex_ = 0;
    listBox_->updateContent();
    listBox_->selectRow(0);
}

void WorkspaceSearchPanel::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
    if (visible)
        searchField_->grabKeyboardFocus();
}

void WorkspaceSearchPanel::resized()
{
    const int margin = 8;
    const int fieldHeight = 28;
    const int btnWidth = 90;
    const int btnHeight = 28;
    const int checkboxWidth = 70;
    const int hintHeight = 20;
    const int topRowY = margin;
    const int searchRowY = topRowY + hintHeight + 4;
    const int listTop = searchRowY + fieldHeight + 8;

    hintLabel_->setBounds(margin, topRowY, getWidth() - 2 * margin, hintHeight);
    searchField_->setBounds(margin, searchRowY, getWidth() - 4 * margin - btnWidth, fieldHeight);
    searchBtn_->setBounds(getWidth() - 3 * margin - btnWidth * 2, searchRowY, btnWidth, btnHeight);
    replaceAllBtn_->setBounds(getWidth() - 2 * margin - btnWidth, searchRowY, btnWidth, btnHeight);
    closeBtn_->setBounds(getWidth() - margin - 20, topRowY, 20, 20);

    int checkboxX = margin;
    if (regexCheckbox_)
    {
        regexCheckbox_->setBounds(checkboxX, searchRowY, checkboxWidth, fieldHeight);
        checkboxX += checkboxWidth + 4;
    }
    if (caseSensitiveCheckbox_)
    {
        caseSensitiveCheckbox_->setBounds(checkboxX, searchRowY, checkboxWidth, fieldHeight);
        checkboxX += checkboxWidth + 4;
    }
    if (wholeWordCheckbox_)
    {
        wholeWordCheckbox_->setBounds(checkboxX, searchRowY, checkboxWidth + 20, fieldHeight);
        checkboxX += checkboxWidth + 24;
    }

    listBox_->setBounds(margin, listTop, getWidth() - 2 * margin, getHeight() - listTop - margin);
}

void WorkspaceSearchPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.92f));
}

// ---------------------------------------------------------------------------
// TextEditor::Listener
// ---------------------------------------------------------------------------

void WorkspaceSearchPanel::textEditorTextChanged(juce::TextEditor& /*editor*/)
{
    // Could auto-search on change, but for now require explicit search button
}

void WorkspaceSearchPanel::textEditorEscapeKeyPressed(juce::TextEditor& /*editor*/)
{
    setVisible(false);
    if (onClosePanel)
        onClosePanel();
}

void WorkspaceSearchPanel::textEditorReturnKeyPressed(juce::TextEditor& /*editor*/)
{
    startSearch(searchField_->getText(), currentFlags());
}

// ---------------------------------------------------------------------------
// ListBoxModel
// ---------------------------------------------------------------------------

int WorkspaceSearchPanel::getNumRows()
{
    return static_cast<int>(displayItems_.size());
}

void WorkspaceSearchPanel::paintListBoxItem(int row, juce::Graphics& g,
                                             int width, int height, bool isSelected)
{
    juce::Rectangle<int> bounds(0, 0, width, height);

    if (isSelected)
        g.fillAll(juce::Colours::white.withAlpha(0.15f));
    else
        g.fillAll(juce::Colours::black.withAlpha(0.85f));

    if (row >= 0 && row < static_cast<int>(displayItems_.size()))
    {
        const auto& item = displayItems_[row];

        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.setFont(juce::FontOptions{13.0f});
        g.drawText(item.relativePath, bounds.removeFromLeft(width / 3).reduced(4, 2),
                   juce::Justification::centredLeft, false);

        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions{14.0f});
        juce::String displayText = juce::String(item.match->line) + ": " + juce::String(item.match->lineText.c_str());
        g.drawText(displayText, bounds.reduced(4, 2), juce::Justification::centredLeft, false);

        g.setColour(juce::Colours::yellow.withAlpha(0.8f));
        g.setFont(juce::FontOptions{13.0f});
        juce::String matchText = juce::String(item.match->matchText.c_str());
        g.drawText(matchText, bounds.removeFromRight(width / 4).reduced(4, 2),
                   juce::Justification::centredLeft, false);
    }
}

void WorkspaceSearchPanel::selectedRowsChanged(int lastSelectedRow)
{
    selectedIndex_ = lastSelectedRow;
}

} // namespace hathor::ui
