// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * TreeBuilder.cpp — recursive filesystem tree builder implementation.
 *
 * This is the core recursive walk that replaces the flat directory scan
 * in ExplorerPanel.cpp.
 */

#include "TreeBuilder.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// TreeBuilder implementation
// ---------------------------------------------------------------------------

FolderNode TreeBuilder::buildTree(const std::filesystem::path& rootDir) noexcept
{
    FolderNode root;

    std::error_code ec;

    if (!std::filesystem::exists(rootDir, ec))
    {
        std::fprintf(stderr,
            "[hathor:Explorer] Root path does not exist: %s\n",
            rootDir.string().c_str());
        return FolderNode{};
    }

    if (!std::filesystem::is_directory(rootDir, ec))
    {
        std::fprintf(stderr,
            "[hathor:Explorer] Root path is not a directory: %s\n",
            rootDir.string().c_str());
        return FolderNode{};
    }

    // Name from the directory basename, or "." for root.
    root.name = rootDir.has_filename()
        ? rootDir.filename().string()
        : rootDir.string();
    root.path = rootDir;
    root.expanded = true; // root is always expanded by default

    buildChildren(rootDir, root);
    return root;
}

void TreeBuilder::buildChildren(const std::filesystem::path& dir, FolderNode& out) noexcept
{
    std::error_code ec;

    // Collect child entries in a single directory iteration.
    // We gather folders and songs separately so we can sort each group
    // and present folders first, then songs — matching conventional
    // file-browser ordering.

    std::vector<std::filesystem::directory_entry> folderEntries;
    std::vector<std::pair<std::string, std::filesystem::directory_entry>> songEntries;

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
    {
        if (ec)
        {
            std::fprintf(stderr,
                "[hathor:Explorer] Cannot read directory: %s (%s)\n",
                dir.string().c_str(), ec.message().c_str());
            break;
        }

        const auto& p = entry.path();
        const FileType ft = classifyFile(p);

        if (ft == FileType::Folder)
        {
            folderEntries.push_back(entry);
        }
        else if (ft == FileType::SongHathor || ft == FileType::SongChuck)
        {
            // Store with filename for sorting.
            songEntries.emplace_back(p.filename().string(), entry);
        }
        // FileType::Other and inaccessible entries are silently excluded.
    }

    // Sort folders by name (directory_iterator is not sorted by default).
    std::sort(folderEntries.begin(), folderEntries.end(),
        [](const std::filesystem::directory_entry& a,
           const std::filesystem::directory_entry& b)
        {
            return a.path().filename().string() < b.path().filename().string();
        });

    // Recursively build each child folder.
    for (const auto& entry : folderEntries)
    {
        const auto& childPath = entry.path();
        FolderNode child(
            childPath.filename().string(),
            childPath);
        child.expanded = false;

        buildChildren(childPath, child);
        out.folders.push_back(std::move(child));
    }

    // Sort songs by filename.
    std::sort(songEntries.begin(), songEntries.end(),
        [](const auto& a, const auto& b)
        {
            return a.first < b.first;
        });

    // Add song leaves.
    for (const auto& [name, entry] : songEntries)
    {
        const auto& childPath = entry.path();
        const FileType ft = classifyFile(childPath);

        out.songs.emplace_back(
            childPath.filename().string(),
            childPath,
            ft);
    }
}

} // namespace hathor::ui
