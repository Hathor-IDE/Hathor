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
#include <optional>

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

/// A managed logical asset — e.g. a ChucK instrument represented as a single
/// entry that may have a .ck source and/or a baked .wav audio component.
///
/// B8-K5: `.hathor_assets/chuck_instruments/acid_bass.ck` + `acid_bass.wav`
/// is presented as a single `acid_bass` instrument node under an
/// `Instruments` category, rather than two unrelated files.
///
/// The physical filesystem layout is never modified — the managed node
/// resolves back to the real .ck and .wav paths.
struct AssetNode
{
    /// The asset category (e.g. "Instruments").  This is the display name of
    /// the managed category folder, not a filesystem path.
    std::string              category;

    /// The logical asset name (e.g. "acid_bass") — the stem shared by the
    /// .ck source and .wav rendering.
    std::string              name;

    /// Path to the .ck source file, if it exists.
    std::optional<std::filesystem::path> ckSource;

    /// Path to the baked .wav file, if it exists.
    std::optional<std::filesystem::path> wavAsset;

    /// True if the .wav has been baked (exists on disk).
    bool hasBakedAudio() const noexcept { return wavAsset.has_value(); }

    /// True if the .ck source exists on disk.
    bool hasSource() const noexcept { return ckSource.has_value(); }

    AssetNode() = default;

    AssetNode(std::string cat, std::string n,
              std::optional<std::filesystem::path> ck,
              std::optional<std::filesystem::path> wav)
        : category(std::move(cat))
        , name(std::move(n))
        , ckSource(std::move(ck))
        , wavAsset(std::move(wav))
    {}
};

/// A folder (album) node in the tree.
struct FolderNode
{
    std::string                      name;       ///< display name (directory basename)
    std::filesystem::path            path;       ///< absolute or relative path
    bool                             expanded;   ///< UI expand/collapse state
    std::vector<FolderNode>          folders;   ///< child folders (sorted)
    std::vector<SongNode>            songs;     ///< child song files (sorted)

    /// Managed logical assets synthesized from `.hathor_assets`.
    /// Each entry is a category (e.g. "Instruments") → list of asset nodes.
    /// Populated by the managed-directory walker; empty for ordinary folders.
    std::vector<FolderNode>          managedCategories;  ///< category folder nodes
    std::vector<AssetNode>           managedAssets;       ///< logical asset leaves

    // Convenience accessor: true if this FolderNode is a managed category
    // folder (e.g. "Instruments") synthesized from .hathor_assets.
    // A managed category is distinguished by having managedAssets populated
    // and a non-empty path pointing inside .hathor_assets.
    bool isManagedCategory() const noexcept
    {
        return !name.empty() && !managedAssets.empty()
            && path.has_parent_path()
            && path.parent_path().filename() == ".hathor_assets";
    }

    FolderNode() = default;

    FolderNode(std::string n, std::filesystem::path p)
        : name(std::move(n)), path(std::move(p)), expanded(false) {}

    bool isEmpty() const noexcept { return folders.empty() && songs.empty() && managedAssets.empty(); }
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
    /// Recursively build children of @p dir into @p out.
    void buildChildren(const std::filesystem::path& dir, FolderNode& out) noexcept;

    /// B8-K5: Walk a managed `.hathor_assets` directory and synthesize
    /// logical asset nodes (Instrument category + instrument entries)
    /// into @p parentOut's managedCategories / managedAssets collections.
    /// The physical filesystem structure is never modified.
    void buildManagedAssets(const std::filesystem::path& assetsDir,
                            FolderNode&                parentOut) noexcept;
};

} // namespace hathor::ui
