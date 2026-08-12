// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GitRepository.cpp — L-5: JUCE-free Git repository data model implementation.
 *
 * Parses machine-readable `git` output (--porcelain, --format, --raw, etc.)
 * into typed structures. All git commands run on worker threads via
 * GitProcess. Cached results are mutex-guarded for safe access from the
 * JUCE message thread.
 *
 * Requirement references: L-5 §Source Control Panel, L-5 §History,
 *   L-5 §Concurrency / Audio Safety
 */

#include "GitRepository.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <thread>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GitRepository::GitRepository() = default;

GitRepository::~GitRepository()
{
    cancel();
}

// ---------------------------------------------------------------------------
// Repository path management
// ---------------------------------------------------------------------------

void GitRepository::setRepoPath(const std::string& path)
{
    std::lock_guard lock(dataMutex_);
    repoPath_ = path;
    hasRepository_ = false;

    if (!path.empty())
    {
        // Check if this is inside a git repository.
        auto result = runGit({"rev-parse", "--is-inside-work-tree"}, 5000);
        if (!result.output.empty() && result.exitCode == 0)
        {
            // Parse "true\n" or "false\n"
            std::string trimmed = result.output;
            trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(),
                                         ::isspace), trimmed.end());
            hasRepository_ = (trimmed == "true");
        }
    }
}

std::string GitRepository::repoPath() const noexcept
{
    std::lock_guard lock(dataMutex_);
    return repoPath_;
}

bool GitRepository::hasRepository() const noexcept
{
    return hasRepository_;
}

void GitRepository::initRepository(const std::string& path,
                                   std::function<void(bool success)> onDone)
{
    std::thread([this, path, onDone = std::move(onDone)]() mutable {
        auto result = runGit({"init"}, 10000);
        // After init, set the repo path.
        if (result.exitCode == 0)
        {
            setRepoPath(path);
        }
        if (onDone)
            onDone(result.exitCode == 0);
    }).detach();
}

// ---------------------------------------------------------------------------
// Internal helper: run a git command
// ---------------------------------------------------------------------------

GitProcess::CompletionResult
GitRepository::runGit(const std::vector<std::string>& args,
                      int timeoutMs)
{
    std::lock_guard lock(dataMutex_);
    std::string path = repoPath_.empty() ? "." : repoPath_;
    return process_.runSync(args, path, timeoutMs);
}

// ---------------------------------------------------------------------------
// Status (async refresh)
// ---------------------------------------------------------------------------

void GitRepository::refreshStatus(std::function<void()> onDone)
{
    std::string path;
    {
        std::lock_guard lock(dataMutex_);
        path = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    // Clone the path to avoid holding the lock during the async run.
    std::thread([this, path, onDone = std::move(onDone)]() mutable {
        // 1. Get status (porcelain v1 with untracked files)
        auto statusResult = process_.runSync(
            {"status", "--porcelain", "--untracked-files=all",
             "--branch", "--untracked-files=all"},
            path, 10000);

        if (statusResult.exitCode == 0)
        {
            auto entries = parsePorcelain(statusResult.output);

            std::lock_guard lock(dataMutex_);
            statusEntries_ = std::move(entries);

            // Extract current branch from the status output (first line "-- branch:").
            // The --branch flag adds a "## branch-name" line at the top.
        }

        // 2. Get current branch
        auto branchResult = process_.runSync({"rev-parse", "--abbrev-ref", "HEAD"},
                                             path, 5000);
        std::string branch = branchResult.output;
        // Trim whitespace
        branch.erase(std::remove_if(branch.begin(), branch.end(),
                                    ::isspace), branch.end());

        // 3. Get merge status (ahead/behind)
        // Try to get upstream info.
        auto upstreamResult = process_.runSync(
            {"rev-parse", "--abbrev-ref", "--symbolic-full-name",
             "HEAD"},
            path, 5000);

        auto aheadBehindResult = process_.runSync(
            {"rev-list", "--count", "--left-right",
             "HEAD", "@{upstream}"},
            path, 5000);

        // 4. Get refs (branches, tags)
        auto refsResult = process_.runSync(
            {"for-each-ref",
             "--format=%(refname:short)\t%(objectname)\t"
             "%(HEAD)\t%(upstream:short)\t%(refname)",
             "refs/heads/", "refs/tags/"},
            path, 5000);

        // 5. Get remotes
        auto remotesResult = process_.runSync(
            {"remote", "-v"},
            path, 5000);

        {
            std::lock_guard lock(dataMutex_);
            currentBranch_ = branch;
            refs_ = parseRefs(refsResult.output, branch);
            remotes_ = parseRemotes(remotesResult.output);
            mergeStatus_ = computeMergeStatus(branch, upstreamResult.output,
                                              aheadBehindResult.output,
                                              !upstreamResult.output.empty());
        }

        if (onDone)
            onDone();
    }).detach();
}

std::vector<GitStatusEntry> GitRepository::getStatusEntries() const
{
    std::lock_guard lock(dataMutex_);
    return statusEntries_;
}

std::string GitRepository::getStatusSummary() const
{
    std::lock_guard lock(dataMutex_);
    // Count staged and unstaged changes.
    int staged = 0, unstaged = 0;
    for (const auto& e : statusEntries_)
    {
        if (e.staged == GitStaged::Yes)
            ++staged;
        else
            ++unstaged;
    }

    std::string summary = currentBranch_;
    if (summary.empty())
        summary = "detached";

    if (staged > 0)
        summary += " · +" + std::to_string(staged) + " staged";
    if (unstaged > 0)
        summary += " · " + std::to_string(unstaged) + " unstaged";

    if (staged == 0 && unstaged == 0)
        summary += " · clean";

    return summary;
}

GitMergeStatus GitRepository::getMergeStatus() const noexcept
{
    return mergeStatus_;
}

std::string GitRepository::getCurrentBranch() const
{
    std::lock_guard lock(dataMutex_);
    return currentBranch_;
}

std::vector<GitRef> GitRepository::getRefs() const
{
    std::lock_guard lock(dataMutex_);
    return refs_;
}

std::vector<GitRemote> GitRepository::getRemotes() const
{
    std::lock_guard lock(dataMutex_);
    return remotes_;
}

GitRepoCapabilities GitRepository::getCapabilities() const noexcept
{
    std::lock_guard lock(dataMutex_);
    GitRepoCapabilities caps;
    caps.hasRepository = hasRepository_;
    caps.hasRemotes = !remotes_.empty();
    caps.canPush = hasRepository_ && caps.hasRemotes;
    caps.canFetch = hasRepository_ && caps.hasRemotes;
    caps.canPull = hasRepository_ && caps.hasRemotes;
    return caps;
}

// ---------------------------------------------------------------------------
// Staging operations (async)
// ---------------------------------------------------------------------------

void GitRepository::stageFile(const std::string& path,
                              std::function<void(bool success)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    std::thread([this, repoPath, path, onDone = std::move(onDone)]() mutable {
        std::vector<std::string> args = {"add", "--", path};
        auto result = process_.runSync(args, repoPath, 10000);
        bool success = (result.exitCode == 0);
        // Refresh status after staging.
        if (success)
            refreshStatus();
        if (onDone)
            onDone(success);
    }).detach();
}

void GitRepository::unstageFile(const std::string& path,
                                std::function<void(bool success)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    std::thread([this, repoPath, path, onDone = std::move(onDone)]() mutable {
        // `git rm --cached` removes from index but keeps working tree file.
        auto result = process_.runSync(
            {"rm", "--cached", "--", path},
            repoPath, 10000);
        bool success = (result.exitCode == 0);
        if (success)
            refreshStatus();
        if (onDone)
            onDone(success);
    }).detach();
}

void GitRepository::discardFile(const std::string& path,
                                std::function<void(bool success)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    std::thread([this, repoPath, path, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"checkout", "--", path},
            repoPath, 10000);
        bool success = (result.exitCode == 0);
        if (success)
            refreshStatus();
        if (onDone)
            onDone(success);
    }).detach();
}

void GitRepository::stageAll(std::function<void(bool success)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    std::thread([this, repoPath, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"add", "-A"},
            repoPath, 10000);
        bool success = (result.exitCode == 0);
        if (success)
            refreshStatus();
        if (onDone)
            onDone(success);
    }).detach();
}

// ---------------------------------------------------------------------------
// Commit (async)
// ---------------------------------------------------------------------------

void GitRepository::commit(const std::string& message,
                           std::function<void(bool success, const std::string& error)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone(false, "Not a git repository");
            return;
        }
    }

    std::thread([this, repoPath, message, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"commit", "-m", message},
            repoPath, 10000);
        bool success = (result.exitCode == 0);
        if (success)
        {
            refreshStatus();
            refreshHistory();
        }
        if (onDone)
            onDone(success, result.output);
    }).detach();
}

// ---------------------------------------------------------------------------
// Branch operations (async)
// ---------------------------------------------------------------------------

void GitRepository::createBranch(const std::string& branchName, bool checkout,
                                 std::function<void(bool success)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    std::thread([this, repoPath, branchName, checkout, onDone = std::move(onDone)]() mutable {
        // Create the branch.
        auto result = process_.runSync(
            {"branch", branchName},
            repoPath, 10000);

        if (result.exitCode != 0)
        {
            if (onDone)
                onDone(false);
            return;
        }

        // Optionally check it out.
        if (checkout)
        {
            auto coResult = process_.runSync(
                {"checkout", branchName},
                repoPath, 10000);
            if (coResult.exitCode != 0)
            {
                if (onDone)
                    onDone(false);
                return;
            }
        }

        refreshStatus();
        refreshHistory();
        if (onDone)
            onDone(true);
    }).detach();
}

void GitRepository::checkoutBranch(const std::string& branchName,
                                   std::function<void(bool success)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    std::thread([this, repoPath, branchName, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"checkout", branchName},
            repoPath, 10000);
        bool success = (result.exitCode == 0);
        if (success)
            refreshStatus();
        if (onDone)
            onDone(success);
    }).detach();
}

void GitRepository::deleteBranch(const std::string& branchName,
                                 std::function<void(bool success)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    std::thread([this, repoPath, branchName, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"branch", "-D", "--", branchName},
            repoPath, 10000);
        bool success = (result.exitCode == 0);
        if (success)
            refreshStatus();
        if (onDone)
            onDone(success);
    }).detach();
}

// ---------------------------------------------------------------------------
// History (async refresh)
// ---------------------------------------------------------------------------

void GitRepository::refreshHistory(std::function<void()> onDone)
{
    std::string path;
    {
        std::lock_guard lock(dataMutex_);
        path = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    std::thread([this, path, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"log", "--all", "--format=%H\t%s\t%an\t%ae\t%aI\t%cn\t%ce\t%cI\t%P",
             "--abbrev-commit", "--graph"},
            path, 10000);

        if (result.exitCode == 0)
        {
            auto commits = parseLog(result.output);
            std::lock_guard lock(dataMutex_);
            history_ = std::move(commits);
        }

        if (onDone)
            onDone();
    }).detach();
}

std::vector<GitCommit> GitRepository::getHistory() const
{
    std::lock_guard lock(dataMutex_);
    return history_;
}

std::optional<GitCommit> GitRepository::getCommit(const std::string& sha) const
{
    std::lock_guard lock(dataMutex_);
    for (const auto& c : history_)
    {
        if (c.sha == sha || c.shortSha == sha)
            return c;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Diff (async)
// ---------------------------------------------------------------------------

void GitRepository::getFileDiff(const std::string& path,
                                std::function<void(const GitFileDiff&)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone(GitFileDiff{});
            return;
        }
    }

    std::thread([this, repoPath, path, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"diff", "--", path},
            repoPath, 10000);
        GitFileDiff diff;
        if (result.exitCode == 0)
            diff = parseLineDiff(result.output, path);
        if (onDone)
            onDone(diff);
    }).detach();
}

void GitRepository::getCommitDiff(const std::string& sha,
                                  std::function<void(const GitFileDiff&)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone(GitFileDiff{});
            return;
        }
    }

    std::thread([this, repoPath, sha, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"show", sha, "--format=diff", "--no-color"},
            repoPath, 10000);
        GitFileDiff diff;
        if (result.exitCode == 0)
            diff = parseLineDiff(result.output, sha);
        if (onDone)
            onDone(diff);
    }).detach();
}

void GitRepository::getStagedFileDiff(const std::string& path,
                                      std::function<void(const GitFileDiff&)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone(GitFileDiff{});
            return;
        }
    }

    std::thread([this, repoPath, path, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"diff", "--cached", "--", path},
            repoPath, 10000);
        GitFileDiff diff;
        if (result.exitCode == 0)
            diff = parseLineDiff(result.output, path);
        if (onDone)
            onDone(diff);
    }).detach();
}

void GitRepository::getWorkingTreeDiff(
    std::function<void(const std::vector<GitFileDiff>&)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone({});
            return;
        }
    }

    std::thread([this, repoPath, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"diff", "--name-only"},
            repoPath, 10000);

        std::vector<GitFileDiff> diffs;
        if (result.exitCode == 0 && !result.output.empty())
        {
            std::istringstream iss(result.output);
            std::string file;
            while (std::getline(iss, file))
            {
                if (!file.empty())
                {
                    auto fileDiffResult = process_.runSync(
                        {"diff", "--", file},
                        repoPath, 10000);
                    diffs.push_back(parseLineDiff(fileDiffResult.output, file));
                }
            }
        }

        if (onDone)
            onDone(diffs);
    }).detach();
}

// ---------------------------------------------------------------------------
// Push / Pull / Fetch (async)
// ---------------------------------------------------------------------------

void GitRepository::fetch(std::function<void(bool success, const std::string& output)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone(false, "Not a git repository");
            return;
        }
    }

    std::thread([this, repoPath, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"fetch", "--prune"},
            repoPath, 30000);
        if (result.exitCode == 0)
            refreshStatus();
        if (onDone)
            onDone(result.exitCode == 0, result.output);
    }).detach();
}

void GitRepository::pull(std::function<void(bool success, const std::string& output)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone(false, "Not a git repository");
            return;
        }
    }

    std::thread([this, repoPath, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"pull"},
            repoPath, 30000);
        if (result.exitCode == 0)
        {
            refreshStatus();
            refreshHistory();
        }
        if (onDone)
            onDone(result.exitCode == 0, result.output);
    }).detach();
}

void GitRepository::push(std::function<void(bool success, const std::string& output)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone(false, "Not a git repository");
            return;
        }
    }

    std::thread([this, repoPath, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"push"},
            repoPath, 30000);
        if (result.exitCode == 0)
        {
            refreshStatus();
            refreshHistory();
        }
        if (onDone)
            onDone(result.exitCode == 0, result.output);
    }).detach();
}

// ---------------------------------------------------------------------------
// Merge (async)
// ---------------------------------------------------------------------------

void GitRepository::merge(const std::string& branchName,
                          std::function<void(bool success, const std::string& output,
                                             bool hasConflicts)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone(false, "Not a git repository", false);
            return;
        }
    }

    std::thread([this, repoPath, branchName, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"merge", "--no-ff", branchName},
            repoPath, 30000);
        bool success = (result.exitCode == 0);
        bool conflicts = result.output.find("CONFLICT") != std::string::npos ||
                         result.output.find("conflict") != std::string::npos;
        if (success)
            refreshStatus();
        if (onDone)
            onDone(success, result.output, conflicts);
    }).detach();
}

// ---------------------------------------------------------------------------
// Conflict resolution
// ---------------------------------------------------------------------------

std::vector<GitStatusEntry> GitRepository::getConflicts() const
{
    std::lock_guard lock(dataMutex_);
    std::vector<GitStatusEntry> conflicts;
    for (const auto& e : statusEntries_)
    {
        if (e.isConflicted)
            conflicts.push_back(e);
    }
    return conflicts;
}

void GitRepository::resolveConflict(const std::string& path,
                                    std::function<void(bool success)> onDone)
{
    std::string repoPath;
    {
        std::lock_guard lock(dataMutex_);
        repoPath = repoPath_.empty() ? "." : repoPath_;
        if (!hasRepository_)
        {
            if (onDone)
                onDone();
            return;
        }
    }

    std::thread([this, repoPath, path, onDone = std::move(onDone)]() mutable {
        auto result = process_.runSync(
            {"add", "--", path},
            repoPath, 10000);
        bool success = (result.exitCode == 0);
        if (success)
            refreshStatus();
        if (onDone)
            onDone(success);
    }).detach();
}

// ---------------------------------------------------------------------------
// Process state
// ---------------------------------------------------------------------------

bool GitRepository::isBusy() const noexcept
{
    // We can't directly check process_ since it's not atomic, but we can
    // check the process state. For thread-safety, we access it directly.
    return process_.state() == TerminalProcess::State::Running;
}

void GitRepository::cancel()
{
    process_.cancel();
}

// ---------------------------------------------------------------------------
// Parsing functions
// ---------------------------------------------------------------------------

std::vector<GitStatusEntry>
GitRepository::parsePorcelain(const std::string& output) const
{
    std::vector<GitStatusEntry> entries;
    if (output.empty())
        return entries;

    std::istringstream iss(output);
    std::string line;

    // The first line(s) starting with "## " are branch info (from --branch).
    // Skip them.
    while (std::getline(iss, line))
    {
        if (line.size() < 2)
            continue;

        // Skip branch info lines ("## ...")
        if (line.substr(0, 2) == "##")
            continue;

        // Porcelain v1 format: XY <space> path
        // X = staged status, Y = unstaged status
        std::string xStr(1, line[0]);  // X
        std::string yStr(1, line[1]);  // Y

        GitStatusEntry entry;

        // Parse staged status (X)
        if (line[0] == ' ')
        {
            entry.staged = GitStaged::No;
        }
        else
        {
            entry.staged = GitStaged::Yes;
        }

        // Determine file status from X and Y.
        // If X is not space, use X; otherwise use Y.
        char statusChar = ' ';
        if (line[0] != ' ')
            statusChar = line[0];
        else if (line.size() > 1 && line[1] != ' ')
            statusChar = line[1];

        if (statusChar == 'M')
            entry.status = GitFileStatus::Modified;
        else if (statusChar == 'A')
            entry.status = GitFileStatus::Added;
        else if (statusChar == 'D')
            entry.status = GitFileStatus::Deleted;
        else if (statusChar == 'R' || statusChar == 'C')
            entry.status = GitFileStatus::Renamed;
        else if (line.substr(0, 3) == "??")
            entry.status = GitFileStatus::Untracked;
        else if (line.substr(0, 2) == "!!")
            entry.status = GitFileStatus::UntrackedDir;
        else if (statusChar == ' ')
            entry.status = GitFileStatus::Clean;
        else
            entry.status = GitFileStatus::Modified;

        // Check for conflicts — git uses 'U' for unmerged, or specific
        // pairs like AA, DD, AU, UA, etc.
        if (line[0] == 'U' || line[1] == 'U' ||
            (line[0] == 'A' && line[1] == 'A') ||
            (line[0] == 'D' && line[1] == 'D') ||
            (line[0] == 'A' && line[1] == 'M') ||
            (line[0] == 'D' && line[1] == 'M'))
        {
            entry.isConflicted = true;
            if (line[0] == 'U' || line[1] == 'U')
                entry.conflictState = GitConflictState::Unmerged;
            else if (line[0] == 'A' && line[1] == 'A')
                entry.conflictState = GitConflictState::AddedByBoth;
            else if (line[0] == 'D' && line[1] == 'D')
                entry.conflictState = GitConflictState::DeletedByBoth;
            else
                entry.conflictState = GitConflictState::BothModified;
        }

        // Extract the path. For renames ("R"), the format is:
        //   "R  old -> new"
        // We need to handle the "->" for renames.
        std::string rest = line.substr(3); // skip "XY "

        // Handle renames: "old_path -> new_path"
        auto arrowPos = rest.find(" -> ");
        if (arrowPos != std::string::npos)
        {
            entry.stagedPath = rest.substr(0, arrowPos);
            entry.unstagedPath = rest.substr(arrowPos + 4);
            // Use the new path as the primary path
            entry.path = entry.staged == GitStaged::Yes
                ? entry.unstagedPath
                : entry.stagedPath;
        }
        else
        {
            entry.path = rest;
        }

        // Trim whitespace
        entry.path.erase(std::remove_if(entry.path.begin(), entry.path.end(),
                                        ::isspace), entry.path.end());

        if (!entry.path.empty())
            entries.push_back(entry);
    }

    return entries;
}

std::vector<GitCommit>
GitRepository::parseLog(const std::string& output) const
{
    std::vector<GitCommit> commits;
    if (output.empty())
        return commits;

    std::istringstream iss(output);
    std::string line;

    while (std::getline(iss, line))
    {
        if (line.empty())
            continue;

        // The --graph flag adds ASCII art lines starting with characters
        // like * | / \ _ .  and spaces. We need to skip the graph art
        // and find the actual commit data.
        // Format with --format and --graph:
        // "* commit_sha\tsubject\t..."
        // The graph prefix precedes the commit hash.

        // Find the start of the commit hash — it follows graph characters.
        // Graph characters are: * | / \ _ . and spaces
        size_t i = 0;
        while (i < line.size())
        {
            char c = line[i];
            if (c == '*' || c == '|' || c == '/' || c == '\\' ||
                c == '_' || c == '.' || c == ' ')
                ++i;
            else
                break;
        }

        if (i >= line.size())
            continue;

        std::string data = line.substr(i);

        // Parse tab-separated fields:
        // %H (sha), %s (subject), %an (author name), %ae (author email),
        // %aI (author date), %cn (committer name), %ce (committer email),
        // %cI (commit date), %P (parents)
        std::istringstream dataStream(data);
        std::string field;
        std::vector<std::string> fields;

        while (std::getline(dataStream, field, '\t'))
            fields.push_back(field);

        if (fields.size() < 8)
            continue;

        GitCommit commit;
        commit.sha = fields[0];
        // Abbreviated SHA: first 7 chars
        commit.shortSha = commit.sha.substr(0,
            std::min<size_t>(7, commit.sha.size()));
        commit.subject = fields[1];
        commit.message = fields[1];
        commit.authorName = fields[2];
        commit.authorEmail = fields[3];
        commit.authorTime = fields[4];
        commit.committerName = fields[5];
        commit.committerEmail = fields[6];
        commit.commitTime = fields[7];

        // Parse parents (%P is tab-separated, space-delimited list)
        if (fields.size() > 8 && !fields[8].empty())
        {
            std::istringstream parentsStream(fields[8]);
            std::string parent;
            while (parentsStream >> parent)
            {
                commit.parentShas.push_back(parent);
            }
            if (!commit.parentShas.empty())
                commit.parentSha = commit.parentShas[0];
        }

        commits.push_back(commit);
    }

    return commits;
}

std::vector<GitFileDiff>
GitRepository::parseRawDiff(const std::string& output) const
{
    std::vector<GitFileDiff> diffs;
    if (output.empty())
        return diffs;

    std::istringstream iss(output);
    std::string line;

    while (std::getline(iss, line))
    {
        // --raw format: :<old_mode> <new_mode> <old_sha> <new_sha> <status>\t<file>
        if (line.empty() || line[0] != ':')
            continue;

        std::istringstream ls(line.substr(1));
        std::string oldMode, newMode, oldSha, newSha, status;
        ls >> oldMode >> newMode >> oldSha >> newSha >> status;

        // The rest after the status code is the file path(s), tab-separated.
        std::string rest;
        std::getline(ls, rest);
        // Remove leading tab/space
        while (!rest.empty() && (rest[0] == '\t' || rest[0] == ' '))
            rest.erase(rest.begin());

        // Handle renames (status starts with 'R')
        auto tabPos = rest.find('\t');
        if (status.size() > 0 && (status[0] == 'R' || status[0] == 'C'))
        {
            if (tabPos != std::string::npos)
            {
                // old_path -> new_path
                GitFileDiff diff;
                diff.status = status;
                diff.oldPath = rest.substr(0, tabPos);
                diff.newPath = rest.substr(tabPos + 1);
                diffs.push_back(diff);
            }
        }
        else
        {
            GitFileDiff diff;
            diff.status = status;
            diff.oldPath = rest;
            diff.newPath = rest;
            diffs.push_back(diff);
        }
    }

    return diffs;
}

GitFileDiff
GitRepository::parseLineDiff(const std::string& output,
                             const std::string& path) const
{
    GitFileDiff diff;
    diff.oldPath = path;
    diff.newPath = path;

    if (output.empty())
        return diff;

    // Track added/deleted line counts.
    int added = 0, deleted = 0;

    std::istringstream iss(output);
    std::string line;

    while (std::getline(iss, line))
    {
        if (line.empty())
            continue;

        char type = line[0];
        std::string content = line.substr(1); // strip the +/-/space prefix

        GitDiffLine dl;
        dl.type = type;
        dl.content = content;
        diff.lines.push_back(dl);

        if (type == '+')
            ++added;
        else if (type == '-')
            ++deleted;
    }

    diff.addedLines = added;
    diff.deletedLines = deleted;
    diff.isBinary = (added == 0 && deleted == 0 && output.find("Binary") != std::string::npos);

    return diff;
}

std::vector<GitRef>
GitRepository::parseRefs(const std::string& output,
                       const std::string& currentBranch) const
{
    std::vector<GitRef> refs;
    if (output.empty())
        return refs;

    std::istringstream iss(output);
    std::string line;

    while (std::getline(iss, line))
    {
        if (line.empty())
            continue;

        // Format: refname_short \t objectname \t HEAD_indicator \t upstream \t fullname
        std::istringstream ls(line);
        std::string name, sha, headInd, upstream, fullName;
        std::getline(ls, name, '\t');
        std::getline(ls, sha, '\t');
        std::getline(ls, headInd, '\t');
        std::getline(ls, upstream, '\t');
        std::getline(ls, fullName, '\t');

        GitRef ref;
        ref.name = name;
        ref.fullName = fullName;
        ref.sha = sha;

        if (fullName.find("refs/heads/") == 0)
            ref.kind = GitRef::Kind::LocalBranch;
        else if (fullName.find("refs/remotes/") == 0)
            ref.kind = GitRef::Kind::RemoteBranch;
        else if (fullName.find("refs/tags/") == 0)
            ref.kind = GitRef::Kind::Tag;
        else
            ref.kind = GitRef::Kind::Head;

        ref.isCurrent = (name == currentBranch && ref.kind == GitRef::Kind::LocalBranch);
        ref.isHead = (headInd == "*");

        refs.push_back(ref);
    }

    return refs;
}

std::vector<GitRemote>
GitRepository::parseRemotes(const std::string& output) const
{
    std::vector<GitRemote> remotes;
    if (output.empty())
        return remotes;

    std::istringstream iss(output);
    std::string line;

    // `git remote -v` output: name \t url (fetch) \n name \t url (push)
    // Each remote appears twice (fetch + push). We deduplicate by name.
    std::map<std::string, GitRemote> remoteMap;

    while (std::getline(iss, line))
    {
        if (line.empty())
            continue;

        std::istringstream ls(line);
        std::string name, url, type;
        ls >> name >> url >> type;

        std::string typeLower = type;
        std::transform(typeLower.begin(), typeLower.end(), typeLower.begin(), ::tolower);

        auto it = remoteMap.find(name);
        if (it == remoteMap.end())
        {
            GitRemote remote;
            remote.name = name;
            remote.url = url;
            if (typeLower.find("fetch") != std::string::npos)
                remote.fetchSpec = url;
            if (typeLower.find("push") != std::string::npos)
                remote.pushSpec = url;
            remoteMap[name] = remote;
        }
        else
        {
            if (typeLower.find("push") != std::string::npos)
                it->second.pushSpec = url;
            if (typeLower.find("fetch") != std::string::npos)
                it->second.fetchSpec = url;
        }
    }

    for (const auto& [name, remote] : remoteMap)
        remotes.push_back(remote);

    return remotes;
}

GitMergeStatus
GitRepository::computeMergeStatus(
    const std::string& /*currentBranch*/,
    const std::string& /*upstreamResult*/,
    const std::string& aheadBehindResult,
    bool upstreamFound) const
{
    if (!hasRepository_)
        return GitMergeStatus::NoRepository;

    if (!upstreamFound)
        return GitMergeStatus::NoRemote;

    // `git rev-list --count --left-right HEAD @{upstream}` output format:
    //   <ahead>\t<behind>
    // If HEAD@{upstream} doesn't exist, git prints an error.
    if (aheadBehindResult.empty())
        return GitMergeStatus::Unknown;

    std::string trimmed = aheadBehindResult;
    trimmed.erase(std::remove_if(trimmed.begin(), trimmed.end(),
                                 ::isspace), trimmed.end());

    // The --left-right format with --count outputs "ahead\tbehind"
    // But actually it outputs "  ahead  behind" — let me parse more carefully.
    // Actually `git rev-list --count --left-right HEAD @{upstream}` outputs:
    //   <ahead_count>\t<behind_count>\n
    // e.g. "3\t1\n" means 3 ahead, 1 behind.
    std::istringstream iss(aheadBehindResult);
    int ahead = 0, behind = 0;
    iss >> ahead >> behind;

    if (ahead == 0 && behind == 0)
        return GitMergeStatus::UpToDate;
    if (ahead > 0 && behind == 0)
        return GitMergeStatus::Ahead;
    if (ahead == 0 && behind > 0)
        return GitMergeStatus::Behind;
    if (ahead > 0 && behind > 0)
        return GitMergeStatus::Diverged;

    return GitMergeStatus::Unknown;
}

} // namespace hathor::ui
