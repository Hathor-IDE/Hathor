// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "GitRepository.hpp"

using hathor::ui::GitRepository;
using hathor::ui::GitFileStatus;

namespace
{
    /// Create a temporary git repository with the given files and initial commit.
    std::filesystem::path createTempRepo(const std::string& initialContent = "")
    {
        // Use a temp directory under the system temp path.
        std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
        tmpDir /= "HathorGitTest_" + std::to_string(std::rand());
        std::filesystem::create_directories(tmpDir);

        // Create a dummy file so we have something to commit.
        if (!initialContent.empty())
        {
            std::ofstream file(tmpDir / "main.hathor");
            file << initialContent;
            file.close();
        }

        // Initialise git repo.
        auto run = [](const std::string& cmd, const std::filesystem::path& dir) {
            std::string fullCmd = "cd '" + dir.string() + "' && " + cmd + " 2>/dev/null";
            std::system(fullCmd.c_str());
        };

        run("git init", tmpDir);
        run("git config user.email 'test@test.com'", tmpDir);
        run("git config user.name 'Test User'", tmpDir);
        if (!initialContent.empty())
        {
            run("git add main.hathor", tmpDir);
            run("git commit -m 'Initial commit'", tmpDir);
        }

        return tmpDir;
    }

    /// Create a temporary non-git directory.
    std::filesystem::path createTempNonRepo()
    {
        std::filesystem::path tmpDir = std::filesystem::temp_directory_path();
        tmpDir /= "HathorGitTest_nongit_" + std::to_string(std::rand());
        std::filesystem::create_directories(tmpDir);
        return tmpDir;
    }
}

TEST_CASE("GitRepository detects repository", "[GitRepository]")
{
    auto tmpDir = createTempRepo("hello world");

    GitRepository repo;
    repo.setRepoPath(tmpDir.string());
    REQUIRE(repo.hasRepository());

    std::filesystem::remove_all(tmpDir);
}

TEST_CASE("GitRepository detects missing repository", "[GitRepository]")
{
    auto tmpDir = createTempNonRepo();

    GitRepository repo;
    repo.setRepoPath(tmpDir.string());
    REQUIRE_FALSE(repo.hasRepository());

    std::filesystem::remove_all(tmpDir);
}

TEST_CASE("GitRepository parses status — modified file", "[GitRepository]")
{
    auto tmpDir = createTempRepo("initial");

    GitRepository repo;
    repo.setRepoPath(tmpDir.string());

    // Modify the file.
    {
        std::ofstream file(tmpDir / "main.hathor");
        file << "modified content";
        file.close();
    }

    // Refresh status.
    bool refreshDone = false;
    repo.refreshStatus([&refreshDone]() { refreshDone = true; });

    // Wait for async refresh.
    for (int i = 0; i < 300 && !refreshDone; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(refreshDone);
    auto entries = repo.getStatusEntries();
    REQUIRE_FALSE(entries.empty());

    // The first entry should be a modified (M) file.
    auto entry = entries[0];
    REQUIRE(entry.path == "main.hathor");
    REQUIRE(entry.status == GitFileStatus::Modified);

    std::filesystem::remove_all(tmpDir);
}

TEST_CASE("GitRepository parses status — untracked file", "[GitRepository]")
{
    auto tmpDir = createTempRepo("initial");

    // Create a new untracked file.
    std::ofstream file(tmpDir / "newfile.hathor");
    file << "new";
    file.close();

    GitRepository repo;
    repo.setRepoPath(tmpDir.string());

    bool refreshDone = false;
    repo.refreshStatus([&refreshDone]() { refreshDone = true; });

    for (int i = 0; i < 300 && !refreshDone; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(refreshDone);
    auto entries = repo.getStatusEntries();
    bool foundUntracked = false;
    for (const auto& e : entries)
    {
        if (e.path == "newfile.hathor" && e.status == GitFileStatus::Untracked)
            foundUntracked = true;
    }
    REQUIRE(foundUntracked);

    std::filesystem::remove_all(tmpDir);
}

TEST_CASE("GitRepository parses log — commit history", "[GitRepository]")
{
    auto tmpDir = createTempRepo("first");

    // Create a second commit.
    std::ofstream file(tmpDir / "main.hathor");
    file << "second";
    file.close();
    std::string cmd = "cd '" + tmpDir.string() + "' && git add -A && git commit -m 'Second commit' 2>/dev/null";
    std::system(cmd.c_str());

    GitRepository repo;
    repo.setRepoPath(tmpDir.string());

    bool refreshDone = false;
    repo.refreshHistory([&refreshDone]() { refreshDone = true; });

    for (int i = 0; i < 300 && !refreshDone; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(refreshDone);
    auto history = repo.getHistory();
    REQUIRE(history.size() >= 2);

    // Most recent commit should be first.
    REQUIRE(history[0].message.find("Second commit") != std::string::npos);
    REQUIRE(history[1].message.find("Initial commit") != std::string::npos);

    std::filesystem::remove_all(tmpDir);
}

TEST_CASE("GitRepository parses refs — branch list", "[GitRepository]")
{
    auto tmpDir = createTempRepo("initial");

    // Create a new branch.
    std::string cmd = "cd '" + tmpDir.string() + "' && git branch feature-branch 2>/dev/null";
    std::system(cmd.c_str());

    GitRepository repo;
    repo.setRepoPath(tmpDir.string());

    bool refreshDone = false;
    repo.refreshStatus([&refreshDone]() { refreshDone = true; });

    for (int i = 0; i < 300 && !refreshDone; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    REQUIRE(refreshDone);

    auto refs = repo.getRefs();
    REQUIRE_FALSE(refs.empty());

    bool foundBranch = false;
    for (const auto& ref : refs)
    {
        if (ref.name == "feature-branch")
            foundBranch = true;
    }
    REQUIRE(foundBranch);

    std::filesystem::remove_all(tmpDir);
}
