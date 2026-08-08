// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ExplorerTreeItems.hpp — JUCE TreeViewItem subclasses for the Explorer tree.
 *
 * Bridges the JUCE-free FolderNode/SongNode data model (TreeBuilder.hpp)
 * to juce::TreeViewItem for hierarchical rendering with expand/collapse.
 *
 * - SongTreeItem: a leaf node that displays a song filename and fires
 *   onSongClicked(juce::File) when activated.
 * - FolderTreeItem: a branch node that owns a copy of its FolderNode data
 *   and recursively contains child SongTreeItem / FolderTreeItem nodes.
 *   Only folders with children render as expandable.
 *
 * Icons are drawn inline using simple geometry (no external icon framework):
 *   - Folder: a small folder shape
 *   - Song (.hathor): a small rectangle with an accent dot (pattern icon)
 *   - Song (.ck): a small rectangle with brackets (ChucK icon)
 *
 * Requirements: A4
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <string>

#include "ExplorerFileTypes.hpp"
#include "HathorLookAndFeel.hpp"
#include "TreeBuilder.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Callbacks
// ---------------------------------------------------------------------------

using SongClickedCallback = std::function<void(const juce::File&)>;

// ---------------------------------------------------------------------------
// SongTreeItem — a leaf node for a song file
// ---------------------------------------------------------------------------

/**
 * SongTreeItem
 *
 * A leaf TreeViewItem representing a supported song file (.hathor or .ck).
 * Clicking it fires onSongClicked(juce::File), which the owner (ExplorerPanel)
 * forwards to EditorArea::openFile.
 */
class SongTreeItem : public juce::TreeViewItem
{
public:
    SongTreeItem(SongNode node, SongClickedCallback onClicked);
    ~SongTreeItem() override = default;

    // juce::TreeViewItem overrides
    void paintItem(juce::Graphics& g, int width, int height) override;
    void itemOpennessChanged(bool isOpen) override;
    bool mightContainSubItems() override { return false; }
    void itemClicked(const juce::MouseEvent& e) override;
    void itemDoubleClicked(const juce::MouseEvent& e) override;

    /// The file represented by this tree item.
    juce::File file() const noexcept { return juce::File(juce::String(node_.path.string())); }

private:
    SongNode          node_;         ///< owned copy of the song data
    SongClickedCallback onSongClicked_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SongTreeItem)
};

// ---------------------------------------------------------------------------
// FolderTreeItem — a branch node for a directory
// ---------------------------------------------------------------------------

/**
 * FolderTreeItem
 *
 * A branch TreeViewItem representing a directory. Owns a copy of its
 * FolderNode data and lazily builds child tree items when first expanded.
 *
 * The root folder is expanded by default. Child folders start collapsed.
 */
class FolderTreeItem : public juce::TreeViewItem
{
public:
    FolderTreeItem(FolderNode node, SongClickedCallback onClicked);
    ~FolderTreeItem() override = default;

    // juce::TreeViewItem overrides
    void paintItem(juce::Graphics& g, int width, int height) override;
    void itemOpennessChanged(bool isOpen) override;
    bool mightContainSubItems() override;
    void itemClicked(const juce::MouseEvent& e) override;
    void itemDoubleClicked(const juce::MouseEvent& e) override;

private:
    FolderNode        node_;          ///< owned copy of the folder data
    SongClickedCallback onSongClicked_; ///< propagated to child items
    bool              childrenBuilt_{ false }; ///< true once children are added

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FolderTreeItem)
};

} // namespace hathor::ui
