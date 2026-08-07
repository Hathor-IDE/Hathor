// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ExplorerPanel.cpp — implementation of the .hathor file explorer panel.
 *
 * Requirements: 21.3, 21.4, 24.1
 */

#include "ExplorerPanel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ExplorerPanel::ExplorerPanel()
    : listBox_("Explorer", this)
{
    // Header label — label-md: 11px, Medium 500, letter-spacing 0.05em (mockup)
    headerLabel_.setText("EXPLORER", juce::dontSendNotification);
    headerLabel_.setFont(HathorLookAndFeel::fontMedium(11.0f));
    headerLabel_.setColour(juce::Label::textColourId, juce::Colour(kHeaderTextColour));
    headerLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(headerLabel_);

    // List box
    listBox_.setColour(juce::ListBox::backgroundColourId, juce::Colour(kBgColour));
    listBox_.setColour(juce::ListBox::outlineColourId, juce::Colours::transparentBlack); // no border
    listBox_.setRowHeight(kRowHeight);
    listBox_.setOutlineThickness(0);
    addAndMakeVisible(listBox_);

    // Initialise with application launch directory as fallback
    directory_ = juce::File::getCurrentWorkingDirectory();
    refresh();
}

// ---------------------------------------------------------------------------
// Directory management
// ---------------------------------------------------------------------------

void ExplorerPanel::setDirectory(const juce::File& dir)
{
    if (dir == directory_)
        return;

    directory_ = dir;
    refresh();
}

void ExplorerPanel::refresh()
{
    files_.clear();

    if (directory_.isDirectory())
    {
        // Find all .hathor files directly in the directory (non-recursive).
        // Requirement 24.1: Explorer panel SHALL list only .hathor files.
        juce::Array<juce::File> found;
        directory_.findChildFiles(found, juce::File::findFiles, false, "*.hathor");

        // Sort alphabetically for predictable ordering
        found.sort();

        files_.reserve(static_cast<std::size_t>(found.size()));
        for (const auto& f : found)
            files_.push_back(f);
    }

    listBox_.updateContent();
    listBox_.repaint();
}

// ---------------------------------------------------------------------------
// juce::Component — layout
// ---------------------------------------------------------------------------

void ExplorerPanel::resized()
{
    auto bounds = getLocalBounds();

    // Header strip at the top
    headerLabel_.setBounds(bounds.removeFromTop(kHeaderHeight).reduced(8, 0));

    // List box fills the remainder
    listBox_.setBounds(bounds);
}

// ---------------------------------------------------------------------------
// juce::Component — painting
// ---------------------------------------------------------------------------

void ExplorerPanel::paint(juce::Graphics& g)
{
    // Background
    g.fillAll(juce::Colour(kBgColour));

    // Header background — slightly lighter than panel bg (surface-container-low)
    g.setColour(juce::Colour(kHeaderBgColour));
    g.fillRect(0, 0, getWidth(), kHeaderHeight);
}

// ---------------------------------------------------------------------------
// juce::ListBoxModel
// ---------------------------------------------------------------------------

int ExplorerPanel::getNumRows()
{
    return static_cast<int>(files_.size());
}

void ExplorerPanel::paintListBoxItem(int rowNumber, juce::Graphics& g,
                                     int width, int height,
                                     bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int>(files_.size()))
        return;

    // Row background
    if (rowIsSelected)
        g.fillAll(juce::Colour(kSelBgColour));
    else
        g.fillAll(juce::Colour(kBgColour));

    // File name text — show stem + extension so the user sees ".hathor"
    const juce::String name = files_[static_cast<std::size_t>(rowNumber)].getFileName();
    const juce::Colour textCol = rowIsSelected
        ? juce::Colour(kSelTextColour)
        : juce::Colour(kItemTextColour);

    g.setColour(textCol);
    g.setFont(HathorLookAndFeel::fontRegular(13.0f));
    g.drawText(name,
               juce::Rectangle<int>(8, 0, width - 8, height),
               juce::Justification::centredLeft,
               true);
}

void ExplorerPanel::listBoxItemClicked(int row, const juce::MouseEvent& /*e*/)
{
    if (row < 0 || row >= static_cast<int>(files_.size()))
        return;

    if (onFileClicked)
        onFileClicked(files_[static_cast<std::size_t>(row)]);
}

void ExplorerPanel::listBoxItemDoubleClicked(int row, const juce::MouseEvent& e)
{
    // Single-click already handles open; double-click does the same.
    listBoxItemClicked(row, e);
}

} // namespace hathor::ui
