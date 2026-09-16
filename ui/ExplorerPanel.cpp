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

    // Do NOT start watching the user's home directory here — a recursive
    // walk of ~ on macOS is slow and trips over permission-restricted /
    // non-UTF-8 paths that can terminate the app (the FsPollTimer callbacks
    // are noexcept).  MainWindow sets the real workspace directory (or shows
    // the welcome screen) immediately after construction, which is when
    // the watcher is (re)pointed at a sane directory.
    // -----------------------------------------------------------------------
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
    // B8-K5 §9 / Agent 0.1: On a fresh launch no workspace has been chosen
    // yet, so `directory_` is still the placeholder user-home path.  Building
    // a tree from ~ would recursively walk the user's home folder (slow, and
    // prone to permission/encoding errors that terminate the noexcept
    // TreeBuilder callbacks).  Leave the tree empty until MainWindow calls
    // setDirectory() or restoreLastDirectoryAndRefresh() with a real path.
    if (directory_ == juce::File::getSpecialLocation(juce::File::userHomeDirectory))
        return;

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

    // Wave 4.1 (X1): right-click context menu for file management.
    auto ctxMenu = [this](const juce::File& target, bool isDir)
    {
        showContextMenu(target, isDir);
    };

    rootItem_ = std::make_unique<FolderTreeItem>(*rootData_, callback, callback, ctxMenu);
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

    // Wave 4.1 (S7): ignore version-control, dependency and build trees so
    // node_modules-scale directories don't peg the CPU on every 2 s poll.
    static const char* kIgnored[] = {
        ".git", "node_modules", "build", "DerivedData", ".hathor",
        ".idea", ".vscode", "CMakeFiles", "_deps"
    };
    auto isIgnored = [](const std::filesystem::path& p) {
        const std::string name = p.filename().string();
        for (auto* ig : kIgnored)
            if (name == ig)
                return true;
        // build* prefix (build-debug, build-release, ...)
        if (name.rfind("build", 0) == 0)
            return true;
        return false;
    };

    const auto rootPath = std::filesystem::path(watchedDir_.getFullPathName().toStdString());
    std::error_code ec;

    // Walk the tree collecting (path, write_time) pairs.
    std::filesystem::recursive_directory_iterator it(rootPath, ec);
    if (ec)
        return;

    std::filesystem::recursive_directory_iterator end;

    try
    {
        while (it != end)
        {
            std::error_code ec2;
            const auto& p = it->path();
            if (isIgnored(p))
            {
                it.disable_recursion_pending();
                ++it;
                continue;
            }
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
    }
    catch (const std::exception& ex)
    {
        // A filesystem error (permission denied, symlink loop, non-UTF-8 path,
        // etc.) must never terminate the app — these callbacks run under
        // JUCE's timer thread.  Log and bail out of this poll cycle.
        std::cerr << "[ExplorerPanel] FsPollTimer iteration error: "
                  << ex.what() << std::endl;
        return;
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

    try
    {
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
    catch (const std::exception& ex)
    {
        // Guarded: rebuildSnapshot is noexcept (called from JUCE's timer
        // thread), so an uncaught exception here would terminate the
        // process.  A filesystem error on a restricted/odd path should
        // only invalidate this poll cycle, not crash the IDE.
        std::cerr << "[ExplorerPanel] FsPollTimer rebuildSnapshot error: "
                  << ex.what() << std::endl;
    }
}

void ExplorerPanel::showContextMenu(const juce::File& target, bool isDirectory)
{
    juce::File dir = isDirectory ? target : target.getParentDirectory();
    juce::File file = isDirectory ? juce::File() : target;

    juce::PopupMenu menu;
    menu.addItem(1, "New File…");
    menu.addItem(2, "New Folder…");
    if (file.existsAsFile() || (isDirectory && target.exists()))
    {
        menu.addSeparator();
        menu.addItem(3, "Rename…");
        if (!isDirectory)
            menu.addItem(4, "Duplicate");
        menu.addItem(5, "Delete…");
        menu.addSeparator();
        menu.addItem(6, "Reveal in Finder");
        menu.addItem(7, "Copy Path");
    }

    menu.showMenuAsync(juce::PopupMenu::Options(), [this, dir, file, isDirectory](int result) {
        switch (result)
        {
            case 1: createFileIn(dir); break;
            case 2: createFolderIn(dir); break;
            case 3: renameTarget(isDirectory ? dir : file); break;
            case 4: if (!isDirectory) duplicateTarget(file); break;
            case 5: deleteTarget(isDirectory ? dir : file); break;
            case 6: (isDirectory ? dir : file).revealToUser(); break;
            case 7:
                juce::SystemClipboard::copyTextToClipboard(
                    (isDirectory ? dir : file).getFullPathName());
                break;
            default: break;
        }
    });
}

void ExplorerPanel::createFileIn(const juce::File& dir)
{
    if (!dir.isDirectory())
        return;
    auto* alert = new juce::AlertWindow("New File", "File name (e.g. sketch.hathor):",
                                        juce::AlertWindow::NoIcon);
    alert->addTextEditor("name", "untitled.hathor");
    alert->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    auto* raw = alert;
    auto retain = std::shared_ptr<juce::AlertWindow>(alert);
    raw->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, dir, retain](int result) {
            if (result == 1)
            {
                const juce::String name = retain->getTextEditorContents("name").trim();
                if (name.isNotEmpty())
                {
                    juce::File f = dir.getChildFile(name);
                    if (!f.existsAsFile())
                    {
                        f.create();
                        if (onFileClicked)
                            onFileClicked(f);
                    }
                    refresh();
                }
            }
        }));
}

void ExplorerPanel::createFolderIn(const juce::File& dir)
{
    if (!dir.isDirectory())
        return;
    auto* alert = new juce::AlertWindow("New Folder", "Folder name:",
                                        juce::AlertWindow::NoIcon);
    alert->addTextEditor("name", "untitled");
    alert->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    auto* raw = alert;
    auto retain = std::shared_ptr<juce::AlertWindow>(alert);
    raw->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, dir, retain](int result) {
            if (result == 1)
            {
                const juce::String name = retain->getTextEditorContents("name").trim();
                if (name.isNotEmpty())
                    dir.getChildFile(name).createDirectory();
                refresh();
            }
        }));
}

void ExplorerPanel::renameTarget(const juce::File& target)
{
    if (!target.exists())
        return;
    auto* alert = new juce::AlertWindow("Rename", "New name:",
                                        juce::AlertWindow::NoIcon);
    alert->addTextEditor("name", target.getFileName());
    alert->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
    alert->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    auto* raw = alert;
    auto retain = std::shared_ptr<juce::AlertWindow>(alert);
    raw->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, target, retain](int result) {
            if (result == 1)
            {
                const juce::String name = retain->getTextEditorContents("name").trim();
                if (name.isNotEmpty() && name != target.getFileName())
                    target.moveFileTo(target.getParentDirectory().getChildFile(name));
                refresh();
            }
        }));
}

void ExplorerPanel::duplicateTarget(const juce::File& target)
{
    if (!target.existsAsFile())
        return;
    juce::File parent = target.getParentDirectory();
    juce::String base = target.getFileNameWithoutExtension();
    juce::String ext = target.getFileExtension();
    juce::File copy = parent.getChildFile(base + " copy" + ext);
    int n = 2;
    while (copy.exists())
        copy = parent.getChildFile(base + " copy " + juce::String(n++) + ext);
    target.copyFileTo(copy);
    refresh();
}

void ExplorerPanel::deleteTarget(const juce::File& target)
{
    if (!target.exists())
        return;
    const bool isDir = target.isDirectory();
    juce::AlertWindow::showOkCancelBox(
        juce::AlertWindow::WarningIcon, "Delete",
        "Delete \"" + target.getFileName() + "\"?" + (isDir ? " This removes the folder and its contents." : ""),
        "Delete", "Cancel", nullptr,
        juce::ModalCallbackFunction::create([this, target](int result) {
            if (result == 1)
            {
                if (target.isDirectory())
                    target.deleteRecursively();
                else
                    target.deleteFile();
                refresh();
            }
        }));
}

} // namespace hathor::ui
