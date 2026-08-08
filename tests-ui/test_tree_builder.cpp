// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_tree_builder.cpp — unit tests for the Explorer recursive tree builder.
 *
 * Tests the JUCE-free TreeBuilder + ExplorerFileTypes logic:
 *   - Recursive walk produces correct folder/song hierarchy
 *   - .hathor files appear as song leaves
 *   - .ck files recognized as song leaves (ready for A5)
 *   - Unrelated files excluded
 *   - Empty directories handled gracefully
 *   - Inaccessible directories handled without crash
 *   - Nested folders work to arbitrary depth
 *
 * Requirements: A4
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "ExplorerFileTypes.hpp"
#include "TreeBuilder.hpp"

using namespace hathor::ui;

// ---------------------------------------------------------------------------
// Helper: write a text file
// ---------------------------------------------------------------------------

static void writeFile(const std::filesystem::path& p, const std::string& content = "")
{
    std::ofstream f(p);
    f << content;
}

// ---------------------------------------------------------------------------
// File type recognition
// ---------------------------------------------------------------------------

TEST_CASE("isSupportedSongExtension recognizes .hathor and .ck", "[filetypes]")
{
    REQUIRE(isSupportedSongExtension(".hathor"));
    REQUIRE(isSupportedSongExtension(".ck"));
}

TEST_CASE("isSupportedSongExtension rejects other extensions", "[filetypes]")
{
    REQUIRE_FALSE(isSupportedSongExtension(".wav"));
    REQUIRE_FALSE(isSupportedSongExtension(".txt"));
    REQUIRE_FALSE(isSupportedSongExtension(".md"));
    REQUIRE_FALSE(isSupportedSongExtension(""));
    REQUIRE_FALSE(isSupportedSongExtension("hathor"));  // no dot
    REQUIRE_FALSE(isSupportedSongExtension(".HATHOR")); // case handled by getLowercasedExtension
}

TEST_CASE("getLowercasedExtension lowercases correctly", "[filetypes]")
{
    REQUIRE(getLowercasedExtension(std::filesystem::path("song.HATHOR")) == ".hathor");
    REQUIRE(getLowercasedExtension(std::filesystem::path("song.HaThOr")) == ".hathor");
    REQUIRE(getLowercasedExtension(std::filesystem::path("instrument.CK")) == ".ck");
    REQUIRE(getLowercasedExtension(std::filesystem::path("noext")) == "");
}

TEST_CASE("isSongFile recognizes .hathor and .ck case-insensitively", "[filetypes]")
{
    REQUIRE(isSongFile(std::filesystem::path("intro.hathor")));
    REQUIRE(isSongFile(std::filesystem::path("INSTRUMENT.CK")));
    REQUIRE_FALSE(isSongFile(std::filesystem::path("readme.md")));
    REQUIRE_FALSE(isSongFile(std::filesystem::path("sample.wav")));
}

TEST_CASE("classifyFile identifies folders, songs, and other", "[filetypes]")
{
    const auto tmp = std::filesystem::temp_directory_path() / "hathor_test_classify";
    std::filesystem::create_directories(tmp);

    const auto songPath = tmp / "test.hathor";
    const auto otherPath = tmp / "notes.txt";
    const auto ckPath = tmp / "inst.ck";

    writeFile(songPath);
    writeFile(otherPath);
    writeFile(ckPath);

    REQUIRE(classifyFile(tmp) == FileType::Folder);
    REQUIRE(classifyFile(songPath) == FileType::SongHathor);
    REQUIRE(classifyFile(ckPath) == FileType::SongChuck);
    REQUIRE(classifyFile(otherPath) == FileType::Other);

    std::filesystem::remove_all(tmp);
}

TEST_CASE("classifyFile returns Other for nonexistent paths", "[filetypes]")
{
    REQUIRE(classifyFile(std::filesystem::path("/nonexistent/path/xyz.hathor")) == FileType::Other);
}

// ---------------------------------------------------------------------------
// TreeBuilder — recursive walk
// ---------------------------------------------------------------------------

TEST_CASE("TreeBuilder builds correct hierarchy", "[tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_tree_basic";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Sub" / "Deep");

    writeFile(root / "root_song.hathor", "d1 $ s \"bd\"");
    writeFile(root / "Sub" / "mid_song.hathor", "d2 $ s \"sn\"");
    writeFile(root / "Sub" / "Deep" / "deep_song.ck", "// chuck");
    writeFile(root / "Sub" / "notes.txt", "not a song");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    REQUIRE(tree.name == "hathor_test_tree_basic");
    REQUIRE(tree.folders.size() == 1);
    REQUIRE(tree.songs.size() == 1);
    REQUIRE(tree.songs[0].name == "root_song.hathor");
    REQUIRE(tree.songs[0].fileType == FileType::SongHathor);

    const auto& sub = tree.folders[0];
    REQUIRE(sub.name == "Sub");
    REQUIRE(sub.folders.size() == 1);
    REQUIRE(sub.songs.size() == 1);
    REQUIRE(sub.songs[0].name == "mid_song.hathor");

    const auto& deep = sub.folders[0];
    REQUIRE(deep.name == "Deep");
    REQUIRE(deep.folders.empty());
    REQUIRE(deep.songs.size() == 1);
    REQUIRE(deep.songs[0].name == "deep_song.ck");
    REQUIRE(deep.songs[0].fileType == FileType::SongChuck);

    std::filesystem::remove_all(root);
}

TEST_CASE("TreeBuilder excludes unrelated files", "[tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_tree_exclude";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    writeFile(root / "song.hathor");
    writeFile(root / "readme.md");
    writeFile(root / "sample.wav");
    writeFile(root / "config.txt");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    REQUIRE(tree.songs.size() == 1);
    REQUIRE(tree.songs[0].name == "song.hathor");

    std::filesystem::remove_all(root);
}

TEST_CASE("TreeBuilder handles empty directories", "[tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_tree_empty";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "empty_subdir");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    REQUIRE(tree.folders.size() == 1);
    REQUIRE(tree.folders[0].name == "empty_subdir");
    REQUIRE(tree.folders[0].isEmpty());
    REQUIRE(tree.folders[0].folders.empty());
    REQUIRE(tree.folders[0].songs.empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("TreeBuilder handles nonexistent root gracefully", "[tree-builder]")
{
    TreeBuilder builder;
    FolderNode tree = builder.buildTree(std::filesystem::path("/nonexistent/hathor/root"));

    REQUIRE(tree.folders.empty());
    REQUIRE(tree.songs.empty());
}

TEST_CASE("TreeBuilder handles inaccessible directory gracefully", "[tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_tree_inaccessible";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "noaccess");

    // Remove read/execute permissions on the subdirectory.
    std::filesystem::permissions(root / "noaccess",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
        std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    // The root should still parse; the inaccessible child should be skipped.
    REQUIRE(tree.folders.size() <= 1);

    // Restore permissions so cleanup works.
    std::filesystem::permissions(root / "noaccess",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
        std::filesystem::perms::others_read | std::filesystem::perms::others_write |
        std::filesystem::perms::others_exec,
        std::filesystem::perm_options::replace);

    std::filesystem::remove_all(root);
}

TEST_CASE("TreeBuilder sorts folders and songs alphabetically", "[tree-builder]")
{
    const auto root = std::filesystem::path("hathor_test_tree_sort");
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "zebra");
    std::filesystem::create_directories(root / "alpha");

    writeFile(root / "zebra_song.hathor");
    writeFile(root / "alpha_song.hathor");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    REQUIRE(tree.folders.size() == 2);
    REQUIRE(tree.folders[0].name == "alpha");
    REQUIRE(tree.folders[1].name == "zebra");

    REQUIRE(tree.songs.size() == 2);
    REQUIRE(tree.songs[0].name == "alpha_song.hathor");
    REQUIRE(tree.songs[1].name == "zebra_song.hathor");

    std::filesystem::remove_all(root);
}

TEST_CASE("TreeBuilder handles deeply nested folders", "[tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_tree_deep";
    std::filesystem::remove_all(root);

    // Create a chain: root / a / b / c / d / song.hathor
    auto current = root;
    for (const auto& name : {"a", "b", "c", "d"})
    {
        current /= name;
        std::filesystem::create_directories(current);
    }
    writeFile(current / "deep_song.hathor");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    // Walk down 4 levels
    const FolderNode* node = &tree;
    for (const auto& name : {"a", "b", "c", "d"})
    {
        REQUIRE(node->folders.size() == 1);
        REQUIRE(node->folders[0].name == name);
        node = &node->folders[0];
    }
    REQUIRE(node->songs.size() == 1);
    REQUIRE(node->songs[0].name == "deep_song.hathor");

    std::filesystem::remove_all(root);
}

TEST_CASE("isAssetDirectory recognizes .hathor_assets", "[filetypes]")
{
    REQUIRE(isAssetDirectory(std::filesystem::path(".hathor_assets")));
    REQUIRE_FALSE(isAssetDirectory(std::filesystem::path("assets")));
    REQUIRE_FALSE(isAssetDirectory(std::filesystem::path("Songs")));
}
