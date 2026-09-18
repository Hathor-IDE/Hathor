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
#include <unordered_map>
#include <unordered_set>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Constants for managed asset directory layout
// ---------------------------------------------------------------------------

static constexpr const char* kChuckInstrumentsSubdir = "chuck_instruments";

static constexpr unsigned kMaxRecursionDepth = 32;

/// Directories never descended into: version control, dependencies, build
/// output, and IDE metadata. Walking them (notably build*/_deps, which can
/// hold hundreds of thousands of files) freezes the message thread and can
/// exhaust memory building nodes nobody will ever expand.
bool TreeBuilder::isIgnoredDir(const std::filesystem::path& p) noexcept
{
    const std::string name = p.filename().string();
    if (name == ".git" || name == "node_modules" || name == ".hathor"
        || name == "DerivedData" || name == ".idea" || name == ".vscode"
        || name == "CMakeFiles" || name == "_deps" || name == "DerivedData")
        return true;
    if (!name.empty() && name[0] == '.')
        return true;
    return name.rfind("build", 0) == 0;
}

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

void TreeBuilder::buildChildren(const std::filesystem::path& dir, FolderNode& out,
                                  unsigned depth) noexcept
{
    if (depth >= kMaxRecursionDepth)
    {
        std::fprintf(stderr,
            "[hathor:Explorer] Max recursion depth reached: %s\n",
            dir.string().c_str());
        return;
    }

    std::error_code ec;

    // Collect child entries in a single directory iteration.
    // Folders and songs are gathered separately so each group can be
    // sorted and folders presented first, matching conventional
    // file-browser ordering.

    std::vector<std::filesystem::directory_entry> folderEntries;
    std::vector<std::filesystem::directory_entry> songEntries;

    std::filesystem::directory_iterator it(
        dir, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec)
    {
        std::fprintf(stderr,
            "[hathor:Explorer] Cannot read directory: %s (%s)\n",
            dir.string().c_str(), ec.message().c_str());
        return;
    }

    const std::filesystem::directory_iterator end;
    while (it != end)
    {
        std::filesystem::directory_entry entry = *it;
        std::error_code iterEc;
        it.increment(iterEc);
        if (iterEc)
        {
            std::fprintf(stderr,
                "[hathor:Explorer] Skipping unreadable entry in: %s (%s)\n",
                dir.string().c_str(), iterEc.message().c_str());
            continue;
        }

        std::error_code entryEc;
        bool isSymlink = entry.is_symlink(entryEc);
        if (!entryEc && isSymlink)
        {
            std::error_code dirEc;
            if (entry.is_directory(dirEc) && !dirEc)
                continue; // never follow directory symlinks
        }

        const auto& p = entry.path();
        const FileType ft = classifyFile(p);

        // Never descend into VCS/dependency/build/IDE trees — except the
        // managed .hathor_assets dir, which collapses into logical nodes.
        if (ft != FileType::ManagedDir && isIgnoredDir(p))
            continue;

        if (ft == FileType::Folder)
        {
            folderEntries.push_back(std::move(entry));
        }
        else if (ft == FileType::ManagedDir)
        {
            // .hathor_assets is a managed directory: its internal
            // structure is collapsed into logical asset nodes. The folder
            // itself does NOT appear as an ordinary child; instead its
            // logical assets are synthesized into the parent FolderNode's
            // managedCategories / managedAssets collections.
            buildManagedAssets(p, out);
        }
        else if (ft == FileType::SongHathor || ft == FileType::SongChuck)
        {
            songEntries.push_back(std::move(entry));
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

        buildChildren(childPath, child, depth + 1);
        out.folders.push_back(std::move(child));
    }

    // Sort songs by filename.
    std::sort(songEntries.begin(), songEntries.end(),
        [](const std::filesystem::directory_entry& a,
           const std::filesystem::directory_entry& b)
        {
            return a.path().filename().string() < b.path().filename().string();
        });

    // Add song leaves.
    for (const auto& entry : songEntries)
    {
        const auto& childPath = entry.path();
        const FileType ft = classifyFile(childPath);

        out.songs.emplace_back(
            childPath.filename().string(),
            childPath,
            ft);
    }
}

// ---------------------------------------------------------------------------
// Managed .hathor_assets directory — synthesize logical asset nodes
// ---------------------------------------------------------------------------

void TreeBuilder::buildManagedAssets(
    const std::filesystem::path& assetsDir,
    FolderNode&                  parentOut) noexcept
{
    std::error_code ec;

    // ---- ChucK instruments: .hathor_assets/chuck_instruments/ -----
    //
    // Walk the chuck_instruments subdirectory and group files by stem.
    // A pair (acid_bass.ck, acid_bass.wav) becomes one AssetNode "acid_bass"
    // with both source and audio paths.  A lone .ck produces a source-only
    // asset.  A lone .wav is also accepted (audio-only, no fake source).

    const auto instrumentsDir = assetsDir / kChuckInstrumentsSubdir;

    if (std::filesystem::is_directory(instrumentsDir, ec))
    {
        // Collect .ck and .wav files by stem.
        struct InstrumentFile
        {
            std::filesystem::path ckPath;
            std::filesystem::path wavPath;
            bool                  hasCk   = false;
            bool                  hasWav  = false;
        };

        std::unordered_map<std::string, InstrumentFile> byStem;
        std::unordered_set<std::string> stemSeen;

        std::vector<std::filesystem::directory_entry> instrEntries;
        {
            std::filesystem::directory_iterator instrIt(
                instrumentsDir,
                std::filesystem::directory_options::skip_permission_denied, ec);
            if (ec)
            {
                std::fprintf(stderr,
                    "[hathor:Explorer] Cannot read instruments dir: %s (%s)\n",
                    instrumentsDir.string().c_str(), ec.message().c_str());
            }
            else
            {
                const std::filesystem::directory_iterator instrEnd;
                while (instrIt != instrEnd)
                {
                    std::filesystem::directory_entry e = *instrIt;
                    std::error_code incrEc;
                    instrIt.increment(incrEc);
                    if (incrEc)
                    {
                        std::fprintf(stderr,
                            "[hathor:Explorer] Skipping unreadable entry in: %s (%s)\n",
                            instrumentsDir.string().c_str(), incrEc.message().c_str());
                        continue;
                    }
                    instrEntries.push_back(std::move(e));
                }
            }
        }

        // Sort for deterministic iteration so first-audio-wins per stem
        // resolves in a defined order.
        std::sort(instrEntries.begin(), instrEntries.end(),
            [](const std::filesystem::directory_entry& a,
               const std::filesystem::directory_entry& b)
            {
                return a.path().filename().string() < b.path().filename().string();
            });

        for (const auto& entry : instrEntries)
        {
            std::error_code fileEc;
            if (!entry.is_regular_file(fileEc) || fileEc)
                continue;

            const auto& p      = entry.path();
            const auto  ext    = getLowercasedExtension(p);
            const auto  stem   = p.stem().string();

            if (stem.empty())
                continue;

            if (ext == ".ck")
            {
                auto [it, inserted] = byStem.emplace(stem, InstrumentFile{});
                if (inserted)
                    stemSeen.insert(stem);
                it->second.ckPath  = p;
                it->second.hasCk   = true;
            }
            else if (ext == ".wav" || ext == ".aiff" || ext == ".flac")
            {
                auto [it, inserted] = byStem.emplace(stem, InstrumentFile{});
                if (inserted)
                    stemSeen.insert(stem);
                // Only record the first audio file per stem. Iteration is
                // sorted by filename, so the winner is deterministic.
                if (!it->second.hasWav)
                {
                    it->second.wavPath = p;
                    it->second.hasWav  = true;
                }
            }
        }

        if (!stemSeen.empty())
        {
            // Sort stems alphabetically for deterministic ordering.
            std::vector<std::string> sortedStems(stemSeen.begin(), stemSeen.end());
            std::sort(sortedStems.begin(), sortedStems.end());

            // Build the "Instruments" managed category folder.
            // Its path is the instruments directory so that asset-to-
            // filesystem resolution is unambiguous.
            FolderNode instrumentsCat(
                "Instruments",
                instrumentsDir);
            instrumentsCat.expanded = false;  // child category — collapsed by default

            for (const auto& stem : sortedStems)
            {
                const auto& instr = byStem[stem];

                std::optional<std::filesystem::path> ckPath;
                std::optional<std::filesystem::path> wavPath;

                if (instr.hasCk)
                    ckPath = instr.ckPath;
                if (instr.hasWav)
                    wavPath = instr.wavPath;

                // Path safety: the stem is a filename stem extracted from a
                // real directory entry — it cannot contain path separators.

                instrumentsCat.managedAssets.emplace_back(
                    "Instruments",
                    stem,
                    std::move(ckPath),
                    std::move(wavPath));
            }

            parentOut.managedCategories.push_back(std::move(instrumentsCat));
        }
    }
    // Other subdirectories of .hathor_assets (e.g. external_imports/) are
    // not yet managed asset categories. They are intentionally not surfaced
    // in the tree at all — only recognised categories (currently ChucK
    // instruments) are exposed.
}

// ---------------------------------------------------------------------------
// Remaining buildChildren implementation is above; the managed-walk
// entry point buildManagedAssets is declared in TreeBuilder.hpp
// ---------------------------------------------------------------------------

} // namespace hathor::ui
