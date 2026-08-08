// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * TreeBuilder.hpp — recursive filesystem tree builder for the Explorer.
 *
 * Produces a lightweight, JUCE-free tree of FolderNode / SongNode values
 * from a recursive walk of a project directory. The tree is JUCE-free so
 * it can be unit-tested without the GUI layer.
 *
 * Tree structure:
 *   - FolderNode: has a name, a path, and an ordered list of children
 *     (which are FolderNode or SongNode).
 *   - SongNode: has a name, a path, and a FileType tag.
 *
 * Both node types support expand/collapse state (expanded_ flag) which is
 * purely UI state — the tree data itself is immutable once built.
 *
 * Empty directories are represented as FolderNodes with no children.
 * Inaccessible directories are skipped gracefully (logged to stderr).
 */

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <variant>

#include "ExplorerFileTypes.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Tree node data (JUCE-free, unit-testable)
// ---------------------------------------------------------------------------

/// A song/file leaf node in the tree.
struct SongNode
{
    std::string       name;       ///< display name (file name + extension)
    std::filesystem::path path;    ///< absolute or relative path
    FileType          fileType;   ///< SongHathor or SongChuck

    SongNode() = default;

    SongNode(std::string n, std::filesystem::path p, FileType ft)
        : name(std::move(n)), path(std::move(p)), fileType(ft) {}
};

/// A folder (album) node in the tree.
struct FolderNode
{
    std::string                      name;       ///< display name (directory basename)
    std::filesystem::path            path;       ///< absolute or relative path
    bool                             expanded;   ///< UI expand/collapse state
    std::vector<FolderNode>          folders;   ///< child folders (sorted)
    std::vector<SongNode>            songs;     ///< child song files (sorted)

    FolderNode() = default;

    FolderNode(std::string n, std::filesystem::path p)
        : name(std::move(n)), path(std::move(p)), expanded(false) {}

    bool isEmpty() const noexcept { return folders.empty() && songs.empty(); }
};

// ---------------------------------------------------------------------------
// TreeBuilder — recursive filesystem walker
// ---------------------------------------------------------------------------

class TreeBuilder
{
public:
    TreeBuilder() = default;
    ~TreeBuilder() = default;

    // Non-copyable but movable (tree can be large; move avoids copies).
    TreeBuilder(const TreeBuilder&)            = delete;
    TreeBuilder& operator=(const TreeBuilder&) = delete;
    TreeBuilder(TreeBuilder&&) noexcept        = default;
    TreeBuilder& operator=(TreeBuilder&&) noexcept = default;

    /// Recursively build a tree rooted at @p rootDir.
    ///
    /// Returns a FolderNode representing the root. Children are sorted:
    ///   1. Folders alphabetically
    ///   2. Songs alphabetically
    ///
    /// - Empty directories produce FolderNodes with no children.
    /// - Inaccessible/unreadable directories are logged to stderr and skipped.
    /// - Only supported song extensions (.hathor, .ck) appear as SongNodes.
    ///   All other files are excluded.
    ///
    /// @param rootDir  The directory to walk recursively.
    /// @return Root FolderNode, or a FolderNode with empty children if
    ///         @p rootDir is inaccessible.
    FolderNode buildTree(const std::filesystem::path& rootDir) noexcept;

private:
    /// Recursive helper — builds children of @p dir into @p out.
    void buildChildren(const std::filesystem::path& dir, FolderNode& out) noexcept;
};

} // namespace hathor::ui
