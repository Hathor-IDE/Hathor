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

#include <chrono>
#include <filesystem>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ExplorerPanel::ExplorerPanel()
    // Agent 0.1: never default to the process CWD as workspace root — the
    // owner (MainWindow) always sets/restores the real root before display.
    : directory_(juce::File::getSpecialLocation(juce::File::userHomeDirectory))
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Header label — label-md: 11px, Medium 500, letter-spacing 0.05em (mockup)
    headerLabel_.setText("EXPLORER", juce::dontSendNotification);
    headerLabel_.setFont(HathorLookAndFeel::uiFontMedium(11.0f));
    headerLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    headerLabel_.setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(headerLabel_);

    // Tree view — uses HathorLookAndFeel TreeView colour IDs (already set).
    treeView_.setColour(juce::TreeView::backgroundColourId, palette.background);
    treeView_.setColour(juce::TreeView::linesColourId,      palette.surfaceHighest);
    treeView_.setColour(juce::TreeView::selectedItemBackgroundColourId, palette.surfaceLow);
    treeView_.setRootItemVisible(true);
    treeView_.setIndentSize(16);
    addAndMakeVisible(treeView_);

    // B8-K5 §9: Set up a polling timer so the managed view stays in sync
    // with filesystem changes (new instruments, re-bakes, deletions,
    // renames).  juce::DirectoryWatcher is unavailable in this JUCE version,
    // so we poll every 2 seconds, comparing file_write_times.
    fsPollTimer_ = std::make_unique<FsPollTimer>(*this);
    fsPollTimer_->watch(directory_);

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

    // B8-K5 §9: Update the polling watcher so the managed view stays
    // in sync with filesystem changes.
    if (fsPollTimer_)
        fsPollTimer_->watch(dir);

    // Persist the new directory for next launch.
    saveLastDirectory();

    refresh();
}

void ExplorerPanel::restoreLastDirectoryAndRefresh()
{
    const juce::File restored = restoreLastDirectory();
    if (restored.isDirectory())
    {
        directory_ = restored;
        // B8-K5 §9: point the polling timer at the restored directory.
        if (fsPollTimer_)
            fsPollTimer_->watch(directory_);
    }
    // If no persisted directory, keep the current one (cwd fallback).

    refresh();
}

void ExplorerPanel::handleFilesystemChange()
{
    // Called by the DirectoryWatcher callback on a background thread.
    // Marshal to the message thread to rebuild the tree safely.
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
        juce::MessageManager::callAsync([this]() { refresh(); });
    else
        refresh(); // no message manager (e.g. in tests) — refresh directly
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
    treeView_.repaint();
}

void ExplorerPanel::buildRootItem()
{
    if (!rootData_)
        return;

    // Create the callback that fires when a song file is clicked.
    // This is shared for both ordinary song files and managed instrument
    // .ck source files (B8-K5) — both open in the EditorArea.
    auto callback = [this](const juce::File& file)
    {
        if (onFileClicked)
            onFileClicked(file);
    };

    rootItem_ = std::make_unique<FolderTreeItem>(*rootData_, callback, callback);
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
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Background
    g.fillAll(palette.background);

    // Header background — slightly lighter than panel bg (surface-container-low)
    g.setColour(palette.surfaceLow);
    g.fillRect(0, 0, getWidth(), kHeaderHeight);
}

// ---------------------------------------------------------------------------
// B8-K5 §9: FsPollTimer — filesystem change polling
// ---------------------------------------------------------------------------

void ExplorerPanel::FsPollTimer::timerCallback()
{
    std::map<std::string, std::uint64_t> current;
    bool changed = false;

    if (!watchedDir_.isDirectory())
        return;

    const auto rootPath = std::filesystem::path(watchedDir_.getFullPathName().toStdString());
    std::error_code ec;

    // Walk the tree collecting (path, write_time) pairs.
    std::filesystem::recursive_directory_iterator it(rootPath, ec);
    if (ec)
        return;

    std::filesystem::recursive_directory_iterator end;

    while (it != end)
    {
        std::error_code ec2;
        const auto& p = it->path();
        const auto ftime = std::filesystem::last_write_time(p, ec2);
        if (!ec2)
        {
            const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                ftime.time_since_epoch()).count();

            const std::string key = p.string();
            current[key] = static_cast<std::uint64_t>(epoch);
        }
        ++it;
    }

    // Compare against the snapshot.
    if (current.size() != snapshot_.size())
    {
        changed = true;
    }
    else
    {
        for (const auto& [k, v] : current)
        {
            auto found = snapshot_.find(k);
            if (found == snapshot_.end() || found->second != v)
            {
                changed = true;
                break;
            }
        }
    }

    if (changed)
    {
        rebuildSnapshot();
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
            juce::MessageManager::callAsync([this]() { owner_.refresh(); });
    }
}

void ExplorerPanel::FsPollTimer::watch(const juce::File& dir) noexcept
{
    watchedDir_ = dir;
    rebuildSnapshot();

    if (!isTimerRunning())
        startTimer(2000);  // poll every 2 seconds
}

void ExplorerPanel::FsPollTimer::reset() noexcept
{
    snapshot_.clear();
    watchedDir_ = juce::File();
    if (isTimerRunning())
        stopTimer();
}

void ExplorerPanel::FsPollTimer::rebuildSnapshot() noexcept
{
    snapshot_.clear();

    if (!watchedDir_.isDirectory())
        return;

    const auto rootPath = std::filesystem::path(watchedDir_.getFullPathName().toStdString());
    std::error_code ec;

    std::filesystem::recursive_directory_iterator it(rootPath, ec);
    if (ec)
        return;

    std::filesystem::recursive_directory_iterator end;

    while (it != end)
    {
        std::error_code ec2;
        const auto& p = it->path();
        const auto ftime = std::filesystem::last_write_time(p, ec2);
        if (!ec2)
        {
            const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                ftime.time_since_epoch()).count();
            snapshot_[p.string()] = static_cast<std::uint64_t>(epoch);
        }
        ++it;
    }
}

} // namespace hathor::ui
