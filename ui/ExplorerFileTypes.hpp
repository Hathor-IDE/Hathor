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
 * Supported song extensions for Phase A4:
 *   - ".hathor" — always recognized (mini-notation source)
 *   - ".ck"     — recognized (ChucK source), ready for A5. The file appears
 *                 as a song leaf because the architecture is prepared, but
 *                 the editor-side eval/highlight is introduced by A5. No
 *                 unsupported behavior is invented here.
 *
 * All other files (".wav", ".txt", ".md", etc.) are excluded from the tree
 * as song leaves.
 *
 * The ".hathor_assets" directory is a managed project-assets folder (see
 * V2 Architecture in PROGRAM.md §Phase C). Its internals are surfaced
 * structurally but individual non-song files within it are not treated as
 * song leaves unless they match a supported extension.
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
    SongChuck,     ///< a .ck ChucK source file (A5 — recognized but eval not yet wired)
    Other,         ///< unsupported — should not appear as a song leaf
};

/// Classify a filesystem entry. If the entry does not exist or is
/// inaccessible, returns FileType::Other.
FileType classifyFile(const std::filesystem::path& p) noexcept;

} // namespace hathor::ui
