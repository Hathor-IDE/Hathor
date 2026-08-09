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
 *   - B8-K5: Managed .hathor_assets directory collapsed into logical
 *     instrument nodes
 *
 * Requirements: A4, B8-K5
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "ExplorerFileTypes.hpp"
#include "TreeBuilder.hpp"
#include "ChuckKeywords.hpp"

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

    // Remove read/execute permissions on the subdirectory to make it inaccessible.
    std::filesystem::permissions(root / "noaccess",
        std::filesystem::perms::none,
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

TEST_CASE("classifyFile distinguishes ManagedDir from Folder", "[filetypes]")
{
    const auto tmp = std::filesystem::temp_directory_path() / "hathor_test_classify_managed";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp / ".hathor_assets" / "chuck_instruments");
    std::filesystem::create_directories(tmp / "normal_dir");

    REQUIRE(classifyFile(tmp / ".hathor_assets") == FileType::ManagedDir);
    REQUIRE(classifyFile(tmp / "normal_dir") == FileType::Folder);

    std::filesystem::remove_all(tmp);
}

// ===========================================================================
// B8-K5: Managed .hathor_assets directory tests
// ===========================================================================

TEST_CASE("B8-K5: TreeBuilder collapses .hathor_assets into Instruments category", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_basic";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck",
              "SinOsc osc => dac;");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav",
              "fake wav content");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    // The .hathor_assets directory should NOT appear as an ordinary folder.
    REQUIRE(tree.folders.empty());

    // There should be one managed category: "Instruments".
    REQUIRE(tree.managedCategories.size() == 1);
    const auto& instruments = tree.managedCategories[0];
    REQUIRE(instruments.name == "Instruments");

    // The Instruments category should contain one logical asset: "acid_bass".
    REQUIRE(instruments.managedAssets.size() == 1);
    const auto& asset = instruments.managedAssets[0];
    REQUIRE(asset.name == "acid_bass");
    REQUIRE(asset.category == "Instruments");

    // The asset should have both source and audio paths.
    REQUIRE(asset.hasSource());
    REQUIRE(asset.hasBakedAudio());
    REQUIRE(asset.ckSource.has_value());
    REQUIRE(asset.wavAsset.has_value());

    // Physical paths should point to the real filesystem locations.
    REQUIRE(asset.ckSource->filename() == "acid_bass.ck");
    REQUIRE(asset.wavAsset->filename() == "acid_bass.wav");
    REQUIRE(asset.ckSource->string().find(".hathor_assets") != std::string::npos);
    REQUIRE(asset.wavAsset->string().find(".hathor_assets") != std::string::npos);

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Source-only instrument (no WAV yet)", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_source_only";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck",
              "SinOsc osc => dac;");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    REQUIRE(tree.folders.empty());
    REQUIRE(tree.managedCategories.size() == 1);

    const auto& assets = tree.managedCategories[0].managedAssets;
    REQUIRE(assets.size() == 1);
    REQUIRE(assets[0].name == "acid_bass");

    // Source exists but no WAV — must not falsely claim baked audio.
    REQUIRE(assets[0].hasSource());
    REQUIRE_FALSE(assets[0].hasBakedAudio());
    REQUIRE(assets[0].ckSource.has_value());
    REQUIRE_FALSE(assets[0].wavAsset.has_value());

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: WAV-only instrument (audio without source)", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_wav_only";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav",
              "fake wav content");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    REQUIRE(tree.managedCategories.size() == 1);

    const auto& assets = tree.managedCategories[0].managedAssets;
    REQUIRE(assets.size() == 1);
    REQUIRE(assets[0].name == "acid_bass");

    // WAV exists but no .ck source — must not invent a fake source.
    REQUIRE(assets[0].hasBakedAudio());
    REQUIRE_FALSE(assets[0].hasSource());
    REQUIRE_FALSE(assets[0].ckSource.has_value());
    REQUIRE(assets[0].wavAsset.has_value());

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Physical filesystem remains unchanged", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_physical";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");
    std::filesystem::create_directories(root / "songs");

    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck", "ck");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav", "wav");
    writeFile(root / "songs" / "intro.hathor", "d1 $ s \"bd\"");
    writeFile(root / "songs" / "outro.hathor", "d2 $ s \"sn\"");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    // Physical files must still exist on disk.
    REQUIRE(std::filesystem::exists(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck"));
    REQUIRE(std::filesystem::exists(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav"));

    // .hathor_assets is not visible as an ordinary folder.
    REQUIRE(tree.folders.size() == 1);
    REQUIRE(tree.folders[0].name == "songs");

    // The songs folder contains the .hathor files as ordinary song leaves.
    REQUIRE(tree.folders[0].songs.size() == 2);

    // Managed assets are present.
    REQUIRE(tree.managedCategories.size() == 1);

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Multiple instruments sorted alphabetically", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_multi";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    writeFile(root / ".hathor_assets" / "chuck_instruments" / "zebra.ck", "");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "zebra.wav", "wav");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "alpha.ck", "");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "alpha.wav", "wav");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "mango.ck", "");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "mango.wav", "wav");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    REQUIRE(tree.managedCategories.size() == 1);
    const auto& assets = tree.managedCategories[0].managedAssets;
    REQUIRE(assets.size() == 3);

    // Should be sorted alphabetically by stem.
    REQUIRE(assets[0].name == "alpha");
    REQUIRE(assets[1].name == "mango");
    REQUIRE(assets[2].name == "zebra");

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Deleted instrument is reflected after rebuild", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_delete";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck", "");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav", "wav");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "hi_hat.ck", "");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "hi_hat.wav", "wav");

    // Initial build — two instruments.
    TreeBuilder builder;
    FolderNode tree1 = builder.buildTree(root);
    REQUIRE(tree1.managedCategories.front().managedAssets.size() == 2);

    // Delete one instrument's files.
    std::filesystem::remove(root / ".hathor_assets" / "chuck_instruments" / "hi_hat.ck");
    std::filesystem::remove(root / ".hathor_assets" / "chuck_instruments" / "hi_hat.wav");

    // Rebuild — should now have only one instrument.
    FolderNode tree2 = builder.buildTree(root);
    REQUIRE(tree2.managedCategories.front().managedAssets.size() == 1);
    REQUIRE(tree2.managedCategories.front().managedAssets[0].name == "acid_bass");

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Re-baked instrument updates the managed view", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_rebake";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    // Source only — no WAV yet.
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck", "");

    TreeBuilder builder;
    FolderNode tree1 = builder.buildTree(root);
    REQUIRE(tree1.managedCategories.front().managedAssets.size() == 1);
    REQUIRE_FALSE(tree1.managedCategories.front().managedAssets[0].hasBakedAudio());

    // Simulate a re-bake: write the WAV.
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav", "new wav");

    FolderNode tree2 = builder.buildTree(root);
    REQUIRE(tree2.managedCategories.front().managedAssets.size() == 1);
    REQUIRE(tree2.managedCategories.front().managedAssets[0].hasBakedAudio());

    // Simulate a re-bake overwrite: modify the WAV.
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav", "updated wav");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    FolderNode tree3 = builder.buildTree(root);
    REQUIRE(tree3.managedCategories.front().managedAssets.size() == 1);
    REQUIRE(tree3.managedCategories.front().managedAssets[0].hasBakedAudio());

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Renamed instrument is reflected after rebuild", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_rename";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    writeFile(root / ".hathor_assets" / "chuck_instruments" / "old_name.ck", "");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "old_name.wav", "wav");

    TreeBuilder builder;
    FolderNode tree1 = builder.buildTree(root);
    REQUIRE(tree1.managedCategories.front().managedAssets.size() == 1);
    REQUIRE(tree1.managedCategories.front().managedAssets[0].name == "old_name");

    // Rename the files.
    std::filesystem::rename(
        root / ".hathor_assets" / "chuck_instruments" / "old_name.ck",
        root / ".hathor_assets" / "chuck_instruments" / "new_name.ck");
    std::filesystem::rename(
        root / ".hathor_assets" / "chuck_instruments" / "old_name.wav",
        root / ".hathor_assets" / "chuck_instruments" / "new_name.wav");

    FolderNode tree2 = builder.buildTree(root);
    REQUIRE(tree2.managedCategories.front().managedAssets.size() == 1);
    REQUIRE(tree2.managedCategories.front().managedAssets[0].name == "new_name");

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Empty .hathor_assets produces no managed categories", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_empty";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    // .hathor_assets should NOT appear as a regular folder.
    REQUIRE(tree.folders.empty());

    // No managed categories because chuck_instruments/ doesn't exist.
    REQUIRE(tree.managedCategories.empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Non-ChucK content in .hathor_assets is not exposed", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_other";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "external_imports");
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    // Non-ChucK content in external_imports — should NOT appear in the tree.
    writeFile(root / ".hathor_assets" / "external_imports" / "808_kick.wav", "wav");

    // ChucK instrument — should appear as a managed asset.
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck", "");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav", "wav");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    // No ordinary folders from .hathor_assets internals.
    REQUIRE(tree.folders.empty());

    // Only the Instruments category is exposed.
    REQUIRE(tree.managedCategories.size() == 1);
    REQUIRE(tree.managedCategories[0].name == "Instruments");
    REQUIRE(tree.managedCategories[0].managedAssets.size() == 1);

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Asset node source path resolves to real .ck file", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_resolve";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    const auto ckPath = root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck";
    const auto wavPath = root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav";
    writeFile(ckPath, "SinOsc s => dac;");
    writeFile(wavPath, "wav data");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    const auto& asset = tree.managedCategories[0].managedAssets[0];

    // The .ck source path must resolve to the real file.
    REQUIRE(asset.ckSource.has_value());
    REQUIRE(std::filesystem::exists(*asset.ckSource));
    REQUIRE(*asset.ckSource == ckPath);

    // The .wav path must also resolve to the real file.
    REQUIRE(asset.wavAsset.has_value());
    REQUIRE(std::filesystem::exists(*asset.wavAsset));
    REQUIRE(*asset.wavAsset == wavPath);

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Managed view reconstructs from filesystem on restart", "[b8-k5][tree-builder]")
{
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_restart";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck", "");
    writeFile(root / ".hathor_assets" / "chuck_instruments" / "acid_bass.wav", "wav");

    // First build.
    TreeBuilder builder1;
    FolderNode tree1 = builder1.buildTree(root);
    REQUIRE(tree1.managedCategories.size() == 1);
    REQUIRE(tree1.managedCategories[0].managedAssets.size() == 1);

    // Second build (simulates restart — fresh TreeBuilder from filesystem).
    TreeBuilder builder2;
    FolderNode tree2 = builder2.buildTree(root);
    REQUIRE(tree2.managedCategories.size() == 1);
    REQUIRE(tree2.managedCategories[0].managedAssets.size() == 1);
    REQUIRE(tree2.managedCategories[0].managedAssets[0].name == "acid_bass");
    REQUIRE(tree2.managedCategories[0].managedAssets[0].hasBakedAudio());

    std::filesystem::remove_all(root);
}

TEST_CASE("B8-K5: Asset node source .ck path is recognized as ChucK", "[b8-k5]")
{
    // The .ck source file path from an AssetNode should have the .ck
    // extension, which is recognized by isChuckExtension so that the
    // editor applies the ChucK tokeniser for highlighting.
    const auto root = std::filesystem::temp_directory_path() / "hathor_test_b8k5_ext";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / ".hathor_assets" / "chuck_instruments");

    const auto ckPath = root / ".hathor_assets" / "chuck_instruments" / "acid_bass.ck";
    writeFile(ckPath, "SinOsc s => dac;");

    TreeBuilder builder;
    FolderNode tree = builder.buildTree(root);

    const auto& asset = tree.managedCategories[0].managedAssets[0];
    REQUIRE(asset.ckSource.has_value());

    // The .ck source path must have the .ck extension.
    REQUIRE(asset.ckSource->extension() == ".ck");
    REQUIRE(isChuckExtension(asset.ckSource->extension().string()));

    std::filesystem::remove_all(root);
}
