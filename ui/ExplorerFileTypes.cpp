// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ExplorerFileTypes.cpp — implementation of file-type recognition.
 *
 * JUCE-free (uses std::filesystem only) so it can be unit-tested without
 * the GUI layer.
 */

#include "ExplorerFileTypes.hpp"

#include <algorithm>
#include <cctype>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Supported song extensions (lowercase, including leading dot)
// ---------------------------------------------------------------------------

static const std::vector<std::string_view>& supportedSongExtensions()
{
    static const std::vector<std::string_view> exts = {
        ".hathor",  // Phase A4: always recognized
        ".ck",      // Phase A5: recognized; eval highlighted from A5 onward
    };
    return exts;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

bool isSupportedSongExtension(std::string_view ext) noexcept
{
    for (const auto& e : supportedSongExtensions())
    {
        if (ext == e)
            return true;
    }
    return false;
}

std::string getLowercasedExtension(const std::filesystem::path& p) noexcept
{
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

bool isSongFile(const std::filesystem::path& p) noexcept
{
    return isSupportedSongExtension(getLowercasedExtension(p));
}

bool isAssetDirectory(const std::filesystem::path& p) noexcept
{
    return p.filename() == ".hathor_assets";
}

FileType classifyFile(const std::filesystem::path& p) noexcept
{
    std::error_code ec;

    if (!std::filesystem::exists(p, ec) || ec)
        return FileType::Other;

    if (std::filesystem::is_directory(p, ec))
        return FileType::Folder;

    if (!std::filesystem::is_regular_file(p, ec))
        return FileType::Other;

    const std::string ext = getLowercasedExtension(p);

    if (ext == ".hathor")
        return FileType::SongHathor;

    if (ext == ".ck")
        return FileType::SongChuck;

    return FileType::Other;
}

} // namespace hathor::ui
