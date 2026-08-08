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
#include <functional>

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
    void refresh();

    /// Restore the last-saved root directory from ApplicationProperties.
    /// Called by MainWindow after setApplicationProperties(). If no
    /// persisted directory exists, the current directory (cwd fallback)
    /// is used. Rebuilds the tree in-place.
    void restoreLastDirectoryAndRefresh();

    //==========================================================================
    // juce::Component overrides
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    // Colours — sourced from HathorLookAndFeel design tokens (single source
    // of truth).
    //==========================================================================
    static constexpr juce::uint32 kBgColour        = HathorLookAndFeel::Colours::background;
    static constexpr juce::uint32 kHeaderBgColour  = HathorLookAndFeel::Colours::surfaceLow;
    static constexpr juce::uint32 kHeaderTextColour = HathorLookAndFeel::Colours::textSecondary;

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

    //==========================================================================
    // Data
    juce::File                    directory_;
    TreeBuilder                   treeBuilder_;
    std::unique_ptr<FolderTreeItem> rootItem_;
    juce::TreeView                treeView_;
    juce::Label                   headerLabel_;

    // Non-owning — set by MainWindow after ApplicationProperties is created.
    juce::ApplicationProperties*  appProperties_{ nullptr };

    // Storage for the root FolderNode so FolderTreeItem has a stable owner.
    std::unique_ptr<FolderNode>   rootData_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExplorerPanel)
};

} // namespace hathor::ui
