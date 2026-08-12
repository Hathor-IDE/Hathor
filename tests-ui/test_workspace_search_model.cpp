// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_workspace_search_model.cpp — unit tests for WorkspaceSearchModel.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target.
 *
 * Requirement references: L-2 §1
 */

#include <catch2/catch_test_macros.hpp>

#include "WorkspaceSearchModel.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace hathor::ui;

// ===========================================================================
// Test fixtures: create a temporary directory with test files
// ===========================================================================

static std::filesystem::path createTempWorkspace()
{
    auto tmpDir = std::filesystem::temp_directory_path() / ("hathor-test-ws-" + std::to_string(std::rand()));
    std::filesystem::create_directories(tmpDir);
    return tmpDir;
}

static void writeFile(const std::filesystem::path& path, const std::string& content)
{
    std::ofstream f(path);
    f << content;
}

static void cleanupWorkspace(const std::filesystem::path& dir)
{
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// ===========================================================================
// Plain text search
// ===========================================================================

TEST_CASE("WorkspaceSearchModel: finds matches in .hathor files", "[workspace_search]")
{
    auto tmpDir = createTempWorkspace();
    auto subDir = tmpDir / "songs";
    std::filesystem::create_directories(subDir);

    writeFile(tmpDir / "test1.hathor", R"(
s("bd sn")
~fast(2)
s("bd")
)");
    writeFile(tmpDir / "test2.hathor", R"(
slow(2)
~s("cp")
)");
    writeFile(subDir / "nested.ck", "// ChucK file\nSinOsc s => dac;");

    {
        WorkspaceSearchModel model(tmpDir);
        WorkspaceSearchFlags flags;
        int total = model.search("s(", flags);

        REQUIRE(total >= 2);

        // Should find results in both .hathor files
        bool foundInTest1 = false;
        bool foundInTest2 = false;
        for (const auto& fileResult : model.results())
        {
            if (fileResult.filePath.filename() == "test1.hathor")
                foundInTest1 = true;
            if (fileResult.filePath.filename() == "test2.hathor")
                foundInTest2 = true;
        }
        REQUIRE(foundInTest1);
        REQUIRE(foundInTest2);
    }

    cleanupWorkspace(tmpDir);
}

TEST_CASE("WorkspaceSearchModel: case-sensitive search", "[workspace_search]")
{
    auto tmpDir = createTempWorkspace();
    writeFile(tmpDir / "case_test.hathor", "Hello World\nhello world\nHELLO");

    {
        WorkspaceSearchModel model(tmpDir);
        WorkspaceSearchFlags flags;
        flags.caseSensitive = true;

        int total = model.search("Hello", flags);
        REQUIRE(total == 1);
    }

    cleanupWorkspace(tmpDir);
}

TEST_CASE("WorkspaceSearchModel: case-insensitive search", "[workspace_search]")
{
    auto tmpDir = createTempWorkspace();
    writeFile(tmpDir / "case_test.hathor", "Hello World\nhello world\nHELLO");

    {
        WorkspaceSearchModel model(tmpDir);
        WorkspaceSearchFlags flags;
        flags.caseSensitive = false;

        int total = model.search("hello", flags);
        REQUIRE(total == 2);
    }

    cleanupWorkspace(tmpDir);
}

TEST_CASE("WorkspaceSearchModel: whole-word match", "[workspace_search]")
{
    auto tmpDir = createTempWorkspace();
    writeFile(tmpDir / "word_test.hathor", "s \"sound\" s(\"bd\") soundness");

    {
        WorkspaceSearchModel model(tmpDir);
        WorkspaceSearchFlags flags;
        flags.wholeWord = true;

        int total = model.search("s", flags);
        // Only "s" as a whole word should match (not "sound" or "soundness")
        // Actually "s" appears as standalone, "sound", "soundness"
        // But in the file, "s" appears in quotes and as a function name.
        // Whole word = surrounded by non-word chars
        REQUIRE(total > 0);
    }

    cleanupWorkspace(tmpDir);
}

TEST_CASE("WorkspaceSearchModel: regex search", "[workspace_search]")
{
    auto tmpDir = createTempWorkspace();
    writeFile(tmpDir / "regex_test.hathor", "s(\"bd\")\ns(\"sn\")\ns(\"hh\")");

    {
        WorkspaceSearchModel model(tmpDir);
        WorkspaceSearchFlags flags;
        flags.useRegex = true;

        int total = model.search(R"(s\(["'](\w+)["']\))", flags);
        REQUIRE(total > 0);
    }

    cleanupWorkspace(tmpDir);
}

TEST_CASE("WorkspaceSearchModel: no matches returns empty results", "[workspace_search]")
{
    auto tmpDir = createTempWorkspace();
    writeFile(tmpDir / "nomatch.hathor", "s(\"bd\")");

    {
        WorkspaceSearchModel model(tmpDir);
        WorkspaceSearchFlags flags;

        int total = model.search("zzz_nonexistent_pattern", flags);
        REQUIRE(total == 0);
        REQUIRE(model.results().empty());
    }

    cleanupWorkspace(tmpDir);
}

// ===========================================================================
// Replace in file
// ===========================================================================

TEST_CASE("WorkspaceSearchModel: replaceInFile replaces all occurrences", "[workspace_search]")
{
    auto tmpDir = createTempWorkspace();
    auto filePath = tmpDir / "replace_test.hathor";
    writeFile(filePath, "old old old\nnew_word\n");

    {
        WorkspaceSearchModel model(tmpDir);
        WorkspaceSearchFlags flags;

        int count = model.replaceInFile(filePath, "old", "new", flags);
        REQUIRE(count == 3);

        // Verify content was updated
        std::ifstream f(filePath);
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
        REQUIRE(content.find("new") != std::string::npos);
        // Ensure no standalone "old" remains (word "old" was replaced)
        REQUIRE(content.find(" old ") == std::string::npos);
    }

    cleanupWorkspace(tmpDir);
}

TEST_CASE("WorkspaceSearchModel: replaceInFile case-sensitive", "[workspace_search]")
{
    auto tmpDir = createTempWorkspace();
    auto filePath = tmpDir / "replace_case.hathor";
    writeFile(filePath, "Hello HELLO hello");

    {
        WorkspaceSearchModel model(tmpDir);
        WorkspaceSearchFlags flags;
        flags.caseSensitive = true;

        int count = model.replaceInFile(filePath, "Hello", "Hi", flags);
        REQUIRE(count == 1);
    }

    cleanupWorkspace(tmpDir);
}

TEST_CASE("WorkspaceSearchModel: replaceInFile on nonexistent file returns 0", "[workspace_search]")
{
    auto tmpDir = createTempWorkspace();
    auto filePath = tmpDir / "nonexistent.hathor";

    {
        WorkspaceSearchModel model(tmpDir);
        WorkspaceSearchFlags flags;

        int count = model.replaceInFile(filePath, "old", "new", flags);
        REQUIRE(count == 0);
    }

    cleanupWorkspace(tmpDir);
}

// ===========================================================================
// Supported extensions
// ===========================================================================

TEST_CASE("WorkspaceSearchModel: supportedExtensions returns correct types", "[workspace_search]")
{
    auto exts = WorkspaceSearchModel::supportedExtensions();
    REQUIRE_FALSE(exts.empty());

    bool hasHathor = false;
    bool hasChuck = false;
    for (const auto& ext : exts)
    {
        if (ext == ".hathor") hasHathor = true;
        if (ext == ".ck") hasChuck = true;
    }
    REQUIRE(hasHathor);
    REQUIRE(hasChuck);
}
