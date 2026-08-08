// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ExplorerPanel.cpp — implementation of the recursive Explorer tree panel.
 *
 * Replaces the previous flat juce::ListBox scan with a recursive
 * juce::TreeView showing folder/album and song/file hierarchy.
 *
 * Requirements: 21.3, 21.4, 24.1, A4
 */

#include "ExplorerPanel.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ExplorerPanel::ExplorerPanel()
    : directory_(juce::File::getCurrentWorkingDirectory())
{
    // Header label — label-md: 11px, Medium 500, letter-spacing 0.05em (mockup)
    headerLabel_.setText("EXPLORER", juce::dontSendNotification);
    headerLabel_.setFont(HathorLookAndFeel::fontMedium(11.0f));
    headerLabel_.setColour(juce::Label::textColourId, juce::Colour(kHeaderTextColour));
    headerLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(headerLabel_);

    // Tree view — uses HathorLookAndFeel TreeView colour IDs (already set).
    treeView_.setColour(juce::TreeView::backgroundColourId, juce::Colour(kBgColour));
    treeView_.setColour(juce::TreeView::linesColourId,
                        juce::Colour(HathorLookAndFeel::Colours::surfaceHighest));
    treeView_.setColour(juce::TreeView::selectedItemBackgroundColourId,
                        juce::Colour(HathorLookAndFeel::Colours::surfaceLow));
    treeView_.setRootItemVisible(true);
    treeView_.setIndentSize(16);
    addAndMakeVisible(treeView_);

    // Build the initial tree. If appProperties_ is set later, restoreLastDirectory
    // will update the directory and rebuild.
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

    // Persist the new directory for next launch.
    saveLastDirectory();

    refresh();
}

void ExplorerPanel::restoreLastDirectoryAndRefresh()
{
    const juce::File restored = restoreLastDirectory();
    if (restored.isDirectory())
        directory_ = restored;
    // If no persisted directory, keep the current one (cwd fallback).

    refresh();
}

void ExplorerPanel::refresh()
{
    // Build the tree data via the recursive walker.
    FolderNode root = treeBuilder_.buildTree(
        std::filesystem::path(directory_.getFullPathName().toStdString()));

    // Store the root data so the tree item has a stable owner.
    rootData_ = std::make_unique<FolderNode>(std::move(root));

    // Build the root tree item.
    buildRootItem();

    treeView_.setRootItem(rootItem_.get());
    treeView_.rebuildItems();
    treeView_.repaint();
}

void ExplorerPanel::buildRootItem()
{
    if (!rootData_)
        return;

    // Create the callback that fires when a song is clicked.
    auto callback = [this](const juce::File& file)
    {
        if (onFileClicked)
            onFileClicked(file);
    };

    rootItem_ = std::make_unique<FolderTreeItem>(*rootData_, callback);
}

// ---------------------------------------------------------------------------
// Persistence — last directory (using the existing ApplicationProperties)
// ---------------------------------------------------------------------------

void ExplorerPanel::saveLastDirectory() const
{
    if (appProperties_ == nullptr)
        return;

    if (auto* settings = appProperties_->getUserSettings())
    {
        settings->setValue("explorerLastDirectory",
                           directory_.getFullPathName());
        settings->saveIfNeeded();
    }
}

juce::File ExplorerPanel::restoreLastDirectory() const
{
    if (appProperties_ == nullptr)
        return juce::File();

    if (const auto* settings = appProperties_->getUserSettings())
    {
        const juce::String path = settings->getValue("explorerLastDirectory");
        if (!path.isEmpty())
            return juce::File(path);
    }

    return juce::File(); // invalid → caller falls back to cwd
}

// ---------------------------------------------------------------------------
// Called by MainWindow after setting ApplicationProperties
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// juce::Component — layout
// ---------------------------------------------------------------------------

void ExplorerPanel::resized()
{
    auto bounds = getLocalBounds();

    // Header strip at the top
    headerLabel_.setBounds(bounds.removeFromTop(kHeaderHeight).reduced(8, 0));

    // Tree view fills the remainder
    treeView_.setBounds(bounds);
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

} // namespace hathor::ui
