// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * FindReplacePanel.cpp — implementation.
 *
 * Requirement references: L-1 §4
 */

#include "FindReplacePanel.hpp"

#include <algorithm>
#include <cctype>

namespace hathor::ui {

FindReplacePanel::FindReplacePanel()
{
    findField_ = std::make_unique<juce::TextEditor>();
    findField_->setInputRestrictions(0, juce::String());
    findField_->setMultiLine(false);
    addAndMakeVisible(findField_.get());
    findField_->onTextChange = [this]() { syncUIFromSearch(findField_->getText()); };

    replaceField_ = std::make_unique<juce::TextEditor>();
    replaceField_->setInputRestrictions(0, juce::String());
    replaceField_->setMultiLine(false);
    addAndMakeVisible(replaceField_.get());
    replaceField_->onTextChange = [this]() { syncUIFromReplace(replaceField_->getText()); };

    findNextBtn_ = std::make_unique<juce::TextButton>("Next");
    addAndMakeVisible(findNextBtn_.get());
    findNextBtn_->onClick = [this]() {
        if (onFindNext) onFindNext();
    };

    findPrevBtn_ = std::make_unique<juce::TextButton>("Prev");
    addAndMakeVisible(findPrevBtn_.get());
    findPrevBtn_->onClick = [this]() {
        if (onFindPrev) onFindPrev();
    };

    replaceBtn_ = std::make_unique<juce::TextButton>("Replace");
    addAndMakeVisible(replaceBtn_.get());
    replaceBtn_->onClick = [this]() {
        if (onReplace) onReplace();
    };

    replaceAllBtn_ = std::make_unique<juce::TextButton>("Replace All");
    addAndMakeVisible(replaceAllBtn_.get());
    replaceAllBtn_->onClick = [this]() {
        if (onReplaceAll) onReplaceAll();
    };

    closeBtn_ = std::make_unique<juce::TextButton>("x");
    addAndMakeVisible(closeBtn_.get());
    closeBtn_->onClick = [this]() {
        if (onClosePanel) onClosePanel();
    };

    regexCheckbox_ = std::make_unique<juce::ToggleButton>("Regex");
    addAndMakeVisible(regexCheckbox_.get());
    regexCheckbox_->onClick = [this]() { updateModelFlags(); };

    caseSensitiveCheckbox_ = std::make_unique<juce::ToggleButton>("Case Sensitive");
    addAndMakeVisible(caseSensitiveCheckbox_.get());
    caseSensitiveCheckbox_->onClick = [this]() { updateModelFlags(); };

    wholeWordCheckbox_ = std::make_unique<juce::ToggleButton>("Whole Word");
    addAndMakeVisible(wholeWordCheckbox_.get());
    wholeWordCheckbox_->onClick = [this]() { updateModelFlags(); };

    wrapAroundCheckbox_ = std::make_unique<juce::ToggleButton>("Wrap");
    addAndMakeVisible(wrapAroundCheckbox_.get());
    wrapAroundCheckbox_->onClick = [this]() { updateModelFlags(); };

    updateModelFlags();
}

FindReplacePanel::~FindReplacePanel() = default;

void FindReplacePanel::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
    // Parent component should resize when we show/hide
}

void FindReplacePanel::setTargetEditor(juce::CodeEditorComponent* editor,
                                        juce::CodeDocument* document)
{
    editor_ = editor;
    document_ = document;

    // Sync current model state to editor
    if (editor_ && !model_.searchText().empty())
    {
        // Highlight matches in the editor
        std::string docStr = document_->getAllContent().toStdString();
        auto matches = model_.findAll(docStr);
        juce::Array<juce::Range<int>> ranges;
        for (const auto& m : matches)
            ranges.add({static_cast<int>(m.start), static_cast<int>(m.end)});
        editor_->setTemporaryUnderlining(ranges);
    }
}

void FindReplacePanel::resized()
{
    juce::Rectangle<int> area(getLocalBounds());
    const int btnW = 70;
    const int fieldW = 180;
    const int checkBoxW = 90;
    const int spacing = 6;

    // Top row: find field + checkboxes
    auto topRow = area.removeFromTop(20);
    findField_->setBounds(topRow.removeFromLeft(fieldW));
    topRow.removeFromLeft(spacing);

    regexCheckbox_->setBounds(topRow.removeFromLeft(checkBoxW));
    caseSensitiveCheckbox_->setBounds(topRow.removeFromLeft(checkBoxW));
    wholeWordCheckbox_->setBounds(topRow.removeFromLeft(checkBoxW));
    wrapAroundCheckbox_->setBounds(topRow.removeFromLeft(checkBoxW));

    // Bottom row: replace field + buttons + close
    auto bottomRow = area.removeFromTop(20);
    replaceField_->setBounds(bottomRow.removeFromLeft(fieldW));
    bottomRow.removeFromLeft(spacing);

    findPrevBtn_->setBounds(bottomRow.removeFromLeft(btnW));
    bottomRow.removeFromLeft(spacing);
    findNextBtn_->setBounds(bottomRow.removeFromLeft(btnW));
    bottomRow.removeFromLeft(spacing);
    replaceBtn_->setBounds(bottomRow.removeFromLeft(btnW));
    bottomRow.removeFromLeft(spacing);
    replaceAllBtn_->setBounds(bottomRow.removeFromLeft(btnW + 50));
    bottomRow.removeFromLeft(spacing);
    closeBtn_->setBounds(bottomRow.removeFromRight(30));
}

void FindReplacePanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey.darker(0.5f));
}

void FindReplacePanel::updateModelFlags()
{
    FindFlags flags = FindFlags::None;
    if (caseSensitiveCheckbox_->getToggleState())
        flags = flags | FindFlags::CaseSensitive;
    if (wholeWordCheckbox_->getToggleState())
        flags = flags | FindFlags::WholeWord;
    if (regexCheckbox_->getToggleState())
        flags = flags | FindFlags::UseRegex;
    if (wrapAroundCheckbox_->getToggleState())
        flags = flags | FindFlags::WrapAround;

    model_.setFlags(flags);
    model_.compilePattern();
}

void FindReplacePanel::syncUIFromSearch(const juce::String& text)
{
    model_.setSearchText(text.toStdString());
    model_.compilePattern();

    // Highlight matches
    if (editor_ && document_)
    {
        std::string docStr = document_->getAllContent().toStdString();
        auto matches = model_.findAll(docStr);
        juce::Array<juce::Range<int>> ranges;
        for (const auto& m : matches)
            ranges.add({static_cast<int>(m.start), static_cast<int>(m.end)});
        editor_->setTemporaryUnderlining(ranges);
    }
}

void FindReplacePanel::syncUIFromReplace(const juce::String& text)
{
    model_.setReplaceText(text.toStdString());
}

} // namespace hathor::ui
