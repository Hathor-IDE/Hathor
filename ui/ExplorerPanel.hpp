// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ExplorerPanel.hpp — recursive folder-tree file browser panel.
 *
 * The panel is toggled open/closed by the Explorer button in ActivityRibbon.
 * When open it displays a hierarchical folder/file tree of the project directory:
 *   - Folders (albums) are expandable/collapsible branch nodes.
 *   - Supported song files (.hathor, and .ck once A5 is in place) are leaf nodes.
 *
 * Clicking a song calls onFileClicked(juce::File), which the owner (MainWindow /
 * EditorArea) uses to open the file in a new tab or focus an existing tab.
 *
 * The last-used root directory is persisted via a juce::ApplicationProperties
 * object installed by the owner (MainWindow), and restored on startup.
 *
 * Requirements: 21.3, 21.4, 24.1, A4
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <atomic>
#include <functional>
#include <map>
#include <cstdint>

#include "HathorLookAndFeel.hpp"
#include "ExplorerTreeItems.hpp"
#include "TreeBuilder.hpp"

namespace hathor::ui {

/**
 * ExplorerPanel
 *
 * A juce::Component that hosts a juce::TreeView showing a recursive
 * project directory tree.
 *
 * Owners should:
 *   1. Call setApplicationProperties() after constructing MainWindow's
 *      ApplicationProperties, so the last root directory can be persisted.
 *   2. Install onFileClicked to respond to song file selections.
 *   3. Call setDirectory(juce::File) whenever the project directory changes.
 */
class ExplorerPanel : public juce::Component
{
public:
    explicit ExplorerPanel();
    ~ExplorerPanel() override = default;

    //==========================================================================
    // Callback — installed by MainWindow / EditorArea.
    // Called on the JUCE message thread when the user clicks a song file.
    std::function<void(juce::File)> onFileClicked;

    //==========================================================================
    // Application properties — for persisting the last root directory.
    // Called once after MainWindow creates its ApplicationProperties.
    void setApplicationProperties(juce::ApplicationProperties* props) noexcept
    {
        appProperties_ = props;
    }

    //==========================================================================
    // Directory management

    /// Set the root directory to walk recursively.
    /// Triggers an immediate rebuild of the tree.
    void setDirectory(const juce::File& dir);

    juce::File directory() const noexcept { return directory_; }

    /// Re-build the tree from the current directory.
    /// The walk runs on a worker thread; results install on the message
    /// thread (stale generations dropped). Synchronous only when no
    /// message manager exists (tests).
    void refresh();

    /// Restore the last-saved root directory from ApplicationProperties.
    /// Called by MainWindow after setApplicationProperties(). If no
    /// persisted directory exists, the current directory (cwd fallback)
    /// is used. Rebuilds the tree in-place.
    void restoreLastDirectoryAndRefresh();

    /// Notify the panel that the project directory's filesystem contents
    /// may have changed externally (e.g. a new .ck instrument was baked).
    /// This is wired to a background polling timer so the managed view
    /// stays in sync with the real filesystem (B8-K5 §9).
    void handleFilesystemChange();

    //==========================================================================
    // Wave 4.1 — file management (X1). Called from the tree context menu.
    void showContextMenu(const juce::File& target, bool isDirectory);
    void createFileIn(const juce::File& dir);
    void createFolderIn(const juce::File& dir);
    void renameTarget(const juce::File& target);
    void duplicateTarget(const juce::File& target);
    void deleteTarget(const juce::File& target);

    //==========================================================================
    // juce::Component overrides
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    // Layout
    static constexpr int kHeaderHeight = 28;

    //==========================================================================
    // Persistence helpers
    void saveLastDirectory() const;
    juce::File restoreLastDirectory() const;

    //==========================================================================
    // Tree root item — rebuilt on each refresh.
    void buildRootItem();

    /// Install a worker-built tree on the message thread.
    void installBuiltTree(FolderNode root);

    //==========================================================================
    // Filesystem refresh (B8-K5 §9, Wave 4.1 S7)
    // ---------------------------------------------------------------------------
    /// Polling timer for filesystem changes. A lightweight juce::Timer walks
    /// the tree and compares file_write_times to detect additions /
    /// deletions / modifications, skipping ignored trees (.git,
    /// node_modules, build*, DerivedData). When a change is detected the
    /// tree is rebuilt.
    class FsPollTimer : public juce::Timer
    {
    public:
        explicit FsPollTimer(ExplorerPanel& owner) : owner_(owner) {}
        void timerCallback() override;
        void watch(const juce::File& dir) noexcept;
        void reset() noexcept;
    private:
        /// Largest tracked tree (entries). Beyond this the watcher holds
        /// its last snapshot instead of churning on untrackable trees.
        static constexpr size_t kMaxSnapshotEntries = 50000;
        ExplorerPanel& owner_;
        juce::File watchedDir_;
        std::map<std::string, std::uint64_t> snapshot_;  // path -> write_time
        void rebuildSnapshot() noexcept;
        /// Shared walk for the poll + snapshot paths (same ignores, same
        /// error handling). False = capped/unreadable, keep old data.
        bool collectInto(std::map<std::string, std::uint64_t>& out) noexcept;
    };

    //==========================================================================
    // Data
    juce::File                    directory_;
    // True once a real workspace root has been set. Replaces the old
    // home-directory sentinel so a workspace rooted at ~ works normally.
    bool                          hasWorkspace_{ false };
    // Async refresh generation: stale worker builds drop themselves.
    std::atomic<uint64_t>         refreshGeneration_{ 0 };
    TreeBuilder                   treeBuilder_;
    std::unique_ptr<FolderTreeItem> rootItem_;
    juce::TreeView                treeView_;
    juce::Label                   headerLabel_;

    /// B8-K5 §9: filesystem polling timer so the managed view reflects
    /// external changes (new instruments, re-bakes, deletions, renames)
    /// without requiring a manual refresh.
    std::unique_ptr<FsPollTimer>  fsPollTimer_;

    // Non-owning — set by MainWindow after ApplicationProperties is created.
    juce::ApplicationProperties*  appProperties_{ nullptr };

    // Storage for the root FolderNode so FolderTreeItem has a stable owner.
    std::unique_ptr<FolderNode>   rootData_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExplorerPanel)
};

} // namespace hathor::ui
