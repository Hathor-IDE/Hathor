// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ExplorerFileTypes.hpp — file-type recognition for the Explorer tree.
 *
 * Determines which files appear as song leaves in the recursive Explorer tree.
 * The logic is kept JUCE-free (plain std::filesystem) so it can be unit-tested
 * independently of the GUI layer.
 *
 * Supported song extensions for Phase A4/A5:
 *   - ".hathor" — always recognized (mini-notation source)
 *   - ".ck"     — recognized (ChucK source). Eval is wired in A5: clicking
 *                 a .ck file in the Explorer opens it in a tab and evaluates
 *                 it via ckEval (mirroring .hathor's Ctrl+Enter surface).
 *
 * All other files (".wav", ".txt", ".md", etc.) are excluded from the tree
 * as song leaves.
 *
 * The ".hathor_assets" directory is a managed project-assets folder (see
 * V2 Architecture in PROGRAM.md §Phase C and B8-K5). Its internal structure
 * (chuck_instruments/ etc.) is collapsed by the TreeBuilder into logical
 * asset nodes (AssetNode) that surface as "Instruments → acid_bass" in the
 * Explorer, while the physical layout remains unchanged on disk.
 */

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Supported song file extensions
// ---------------------------------------------------------------------------

/// Returns true if @p ext (including the leading dot, lowercase) is a
/// supported song/source extension.
bool isSupportedSongExtension(std::string_view ext) noexcept;

/// Overload taking a juce-independent path. Returns the extension as a
/// lowercase string_view including the leading dot (e.g. ".hathor").
std::string getLowercasedExtension(const std::filesystem::path& p) noexcept;

/// Returns true if a file should appear as a song leaf in the explorer tree.
bool isSongFile(const std::filesystem::path& p) noexcept;

/// Returns true if a directory should be treated as a managed-assets folder
/// (e.g. ".hathor_assets"). These are still shown in the tree but may be
/// rendered with a distinct icon in later phases.
bool isAssetDirectory(const std::filesystem::path& p) noexcept;

// ---------------------------------------------------------------------------
// File type enum for icon selection
// ---------------------------------------------------------------------------

enum class FileType
{
    Folder,        ///< a directory node
    SongHathor,    ///< a .hathor song file
    SongChuck,     ///< a .ck ChucK source file (A5 — eval wired via ckEval on Explorer click)
    ManagedDir,    ///< a managed asset directory (.hathor_assets) — collapsed by TreeBuilder
    Other,         ///< unsupported — should not appear as a song leaf
};

/// Classify a filesystem entry. If the entry does not exist or is
/// inaccessible, returns FileType::Other.
FileType classifyFile(const std::filesystem::path& p) noexcept;

} // namespace hathor::ui
