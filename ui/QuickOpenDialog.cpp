// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * QuickOpenDialog.cpp — implementation of the quick-open dialog.
 *
 * JUCE-dependent. Uses JUCE-free WorkspaceSearchModel for search logic
 * where applicable, but file collection is JUCE-specific (recursive_directory_iterator).
 *
 * Requirement references: L-2 §1
 */

#include "QuickOpenDialog.hpp"

#include "HathorLookAndFeel.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace hathor::ui {

class QuickOpenDialogDoubleClickHandler : public juce::MouseListener
{
public:
    explicit QuickOpenDialogDoubleClickHandler(QuickOpenDialog* parent) : parent_(parent) {}
    void mouseDoubleClick(const juce::MouseEvent&) override { parent_->confirmSelection(); }
private:
    QuickOpenDialog* parent_;
};

QuickOpenDialog::QuickOpenDialog(const std::filesystem::path& workspaceRoot)
    : workspaceRoot_(workspaceRoot)
{
    filterField_ = std::make_unique<juce::TextEditor>();
    filterField_->addListener(this);
    filterField_->setFont(juce::FontOptions{16.0f});
    filterField_->setColour(juce::TextEditor::backgroundColourId,
                            juce::Colours::black.withAlpha(0.6f));
    filterField_->setColour(juce::TextEditor::textColourId, juce::Colours::white);
    filterField_->setColour(juce::CaretComponent::caretColourId, juce::Colours::white);
    filterField_->setInputRestrictions(0, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:/\\");
    addAndMakeVisible(filterField_.get());

    hintLabel_ = std::make_unique<juce::Label>();
    hintLabel_->setText("Type to filter files (Esc to close)", juce::dontSendNotification);
    hintLabel_->setFont(juce::FontOptions{14.0f});
    hintLabel_->setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.7f));
    hintLabel_->setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(hintLabel_.get());

    listBox_ = std::make_unique<juce::ListBox>();
    listBox_->setModel(this);
    listBox_->setRowSelectedOnMouseDown(true);
    listBox_->setColour(juce::ListBox::backgroundColourId, juce::Colours::black.withAlpha(0.8f));
    listBox_->setColour(juce::ListBox::outlineColourId, juce::Colours::white.withAlpha(0.2f));
    addAndMakeVisible(listBox_.get());

    listBox_->addMouseListener(new QuickOpenDialogDoubleClickHandler(this), true);

    collectFiles(workspaceRoot_);
    filteredFiles_ = allFiles_;
    refreshFiltered();
}

QuickOpenDialog::~QuickOpenDialog()
{
    listBox_->setModel(nullptr);
}

void QuickOpenDialog::collectFiles(const std::filesystem::path& root)
{
    if (!std::filesystem::exists(root))
        return;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;

        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (ext == ".hathor" || ext == ".ck" || ext == ".txt" || ext == ".md")
        {
            // Skip nodes_modules and hidden directories
            auto pathStr = entry.path().string();
            if (pathStr.find("node_modules") != std::string::npos)
                continue;
            if (pathStr.find("/.") != std::string::npos)
                continue;

            allFiles_.push_back(entry.path());
        }
    }

    std::sort(allFiles_.begin(), allFiles_.end());
}

void QuickOpenDialog::showOver(juce::Component* parent)
{
    if (!parent)
        return;

    auto bounds = parent->getBounds();
    setBounds(bounds);
    parent->addAndMakeVisible(this);
    setVisible(true);

    filterField_->setText({});
    filterField_->grabKeyboardFocus();
    selectedIndex_ = 0;
    if (listBox_)
        listBox_->selectRow(0);
}

void QuickOpenDialog::hide()
{
    setVisible(false);
    if (onCancelled)
        onCancelled();
}

void QuickOpenDialog::resized()
{
    const int hintHeight = 24;
    const int fieldHeight = 36;
    const int margin = 8;

    hintLabel_->setBounds(margin, margin, getWidth() - 2 * margin, hintHeight);
    filterField_->setBounds(margin, hintHeight + margin, getWidth() - 2 * margin, fieldHeight);
    listBox_->setBounds(margin, hintHeight + fieldHeight + 2 * margin,
                        getWidth() - 2 * margin, getHeight() - hintHeight - fieldHeight - 3 * margin);
}

void QuickOpenDialog::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.9f));
}

bool QuickOpenDialog::fuzzyMatch(std::string_view query, std::string_view path)
{
    if (query.empty())
        return true;

    // Simple subsequence fuzzy match (case-insensitive)
    auto lowerQuery = std::string(query);
    std::transform(lowerQuery.begin(), lowerQuery.end(), lowerQuery.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto lowerPath = std::string(path);
    std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    size_t qi = 0;
    for (size_t pi = 0; pi < lowerPath.size() && qi < lowerQuery.size(); ++pi)
    {
        if (lowerPath[pi] == lowerQuery[qi])
            qi++;
    }

    return qi == lowerQuery.size();
}

void QuickOpenDialog::refreshFiltered()
{
    auto query = filterField_ ? filterField_->getText() : juce::String();
    filteredFiles_.clear();

    if (query.isEmpty())
    {
        filteredFiles_ = allFiles_;
    }
    else
    {
        std::string queryStr = query.toStdString();
        for (const auto& f : allFiles_)
        {
            if (fuzzyMatch(queryStr, f.filename().string()))
                filteredFiles_.push_back(f);
        }
    }

    selectedIndex_ = 0;
    if (listBox_)
    {
        listBox_->updateContent();
        listBox_->selectRow(0);
    }
}

void QuickOpenDialog::setFilter(const juce::String& query)
{
    if (filterField_)
        filterField_->setText(query, juce::dontSendNotification);
    refreshFiltered();
}

std::filesystem::path QuickOpenDialog::selectedFile() const
{
    if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(filteredFiles_.size()))
        return {};
    return filteredFiles_[selectedIndex_];
}

void QuickOpenDialog::selectUp()
{
    if (selectedIndex_ > 0)
    {
        selectedIndex_--;
        if (listBox_)
            listBox_->selectRow(selectedIndex_);
    }
}

void QuickOpenDialog::selectDown()
{
    if (selectedIndex_ < static_cast<int>(filteredFiles_.size()) - 1)
    {
        selectedIndex_++;
        if (listBox_)
            listBox_->selectRow(selectedIndex_);
    }
}

bool QuickOpenDialog::confirmSelection()
{
    auto file = selectedFile();
    if (file.empty())
        return false;

    setVisible(false);

    if (onFileSelected)
    {
        onFileSelected(file);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// TextEditor::Listener
// ---------------------------------------------------------------------------

void QuickOpenDialog::textEditorTextChanged(juce::TextEditor& /*editor*/)
{
    refreshFiltered();
}

void QuickOpenDialog::textEditorEscapeKeyPressed(juce::TextEditor& /*editor*/)
{
    hide();
}

bool QuickOpenDialog::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::returnKey)
    {
        if (confirmSelection())
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

// ---------------------------------------------------------------------------
// ListBoxModel
// ---------------------------------------------------------------------------

int QuickOpenDialog::getNumRows()
{
    return static_cast<int>(filteredFiles_.size());
}

void QuickOpenDialog::paintListBoxItem(int row, juce::Graphics& g,
                                       int width, int height, bool isSelected)
{
    juce::Rectangle<int> bounds(0, 0, width, height);

    if (isSelected)
        g.fillAll(juce::Colours::white.withAlpha(0.15f));
    else
        g.fillAll(juce::Colours::black.withAlpha(0.8f));

    auto file = filteredFiles_[row];
    juce::String displayText = file.filename().string();

    auto relative = std::filesystem::relative(file, workspaceRoot_);
    if (relative.has_parent_path())
    {
        auto parentStr = relative.parent_path().string();
        displayText = juce::String(parentStr.c_str()) + " / " + displayText;
    }

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions{15.0f});
    g.drawText(displayText, bounds.reduced(4, 2), juce::Justification::centredLeft, false);
}

void QuickOpenDialog::selectedRowsChanged(int lastSelectedRow)
{
    selectedIndex_ = lastSelectedRow;
}

} // namespace hathor::ui
