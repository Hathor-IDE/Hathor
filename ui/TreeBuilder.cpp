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
// Constants for managed asset directory layout (see PROGRAM.md §V2 Architecture)
// ---------------------------------------------------------------------------

static constexpr const char* kHathorAssetsDir       = ".hathor_assets";
static constexpr const char* kChuckInstrumentsSubdir = "chuck_instruments";

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
    std::vector<std::filesystem::directory_entry> songEntries;

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
            // Special handling: .hathor_assets is a managed directory —
            // its internal structure is collapsed into logical asset nodes.
            // The folder itself does NOT appear as an ordinary child; instead
            // its logical assets are synthesized into the parent FolderNode's
            // managedCategories / managedAssets collections.
            if (isAssetDirectory(p))
            {
                buildManagedAssets(p, out);
            }
            else
            {
                folderEntries.push_back(entry);
            }
        }
        else if (ft == FileType::SongHathor || ft == FileType::SongChuck)
        {
            songEntries.push_back(entry);
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
// B8-K5: Managed .hathor_assets directory — synthesize logical asset nodes
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
        std::vector<std::string>                        stemOrder; // preserve sort

        for (const auto& entry : std::filesystem::directory_iterator(instrumentsDir, ec))
        {
            if (ec)
            {
                std::fprintf(stderr,
                    "[hathor:Explorer] Cannot read instruments dir: %s (%s)\n",
                    instrumentsDir.string().c_str(), ec.message().c_str());
                break;
            }

            if (!entry.is_regular_file(ec))
                continue;

            const auto& p      = entry.path();
            const auto  ext    = getLowercasedExtension(p);
            const auto  stem   = p.stem().string();

            if (stem.empty())
                continue;

            if (ext == ".ck")
            {
                auto& instr = byStem.try_emplace(stem).first->second;
                if (!instr.hasCk)
                    stemOrder.push_back(stem);
                instr.ckPath = p;
                instr.hasCk  = true;
            }
            else if (ext == ".wav" || ext == ".aiff" || ext == ".flac")
            {
                auto& instr = byStem.try_emplace(stem).first->second;
                if (!instr.hasCk && !instr.hasWav)
                    stemOrder.push_back(stem);
                // Only record the first audio file per stem (matches .wav
                // convention; if both .wav and .aiff exist, .wav wins since
                // it's encountered first in typical layouts).
                if (!instr.hasWav)
                {
                    instr.wavPath = p;
                    instr.hasWav  = true;
                }
            }
        }

        if (!stemOrder.empty())
        {
            // Sort stems alphabetically for deterministic ordering.
            std::sort(stemOrder.begin(), stemOrder.end());

            // Build the "Instruments" managed category folder.
            // Its path is the instruments directory so that asset-to-
            // filesystem resolution is unambiguous.
            FolderNode instrumentsCat(
                "Instruments",
                instrumentsDir);
            instrumentsCat.expanded = false;  // child category — collapsed by default

            for (const auto& stem : stemOrder)
            {
                const auto& instr = byStem[stem];

                std::optional<std::filesystem::path> ckPath;
                std::optional<std::filesystem::path> wavPath;

                if (instr.hasCk)
                    ckPath = instr.ckPath;
                if (instr.hasWav)
                    wavPath = instr.wavPath;

                // Path safety (B8-K5 §12): the stem is already a filename
                // stem extracted from a real directory entry — it cannot
                // contain path separators.  No additional sanitisation
                // needed beyond what std::filesystem guarantees.

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
    // not yet managed asset categories.  They are intentionally not surfaced
    // in the tree at all — only recognised categories (currently ChucK
    // instruments) are exposed.  This leaves room for future categories
    // (B8-K5 §11) without exposing implementation directories.
}

// ---------------------------------------------------------------------------
// Remaining buildChildren implementation is above; the managed-walk
// entry point buildManagedAssets is declared in TreeBuilder.hpp
// ---------------------------------------------------------------------------

} // namespace hathor::ui
