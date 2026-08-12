// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GitRepository.hpp — L-5: JUCE-free Git repository data model.
 *
 * This is the repository-logic layer. It uses GitProcess (the async subprocess
 * transport) to run `git` commands, then parses the machine-readable output
 * (--porcelain, --format, --raw, etc.) into typed, value-semantic structures
 * that the JUCE UI components consume.
 *
 * All heavy operations (status, diff, log, etc.) run on a worker thread via
 * GitProcess. Results are delivered asynchronously through callbacks or
 * a polling interface — never blocking the JUCE message thread or audio thread.
 *
 * The model is intentionally JUCE-free so it can be unit-tested headlessly
 * (like TerminalProcess, FindReplaceModel, NavigationHistory, etc.).
 *
 * Requirement references: L-5 §Source Control Panel, L-5 §History,
 *   L-5 §Concurrency / Audio Safety
 */

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "GitProcess.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Enum types
// ---------------------------------------------------------------------------

/// Git status for a single file (XGit porcelain v1 status code).
enum class GitFileStatus
{
    Modified,    ///< M — modified in working tree
    Added,       ///< A — added to index
    Deleted,     ///< D — deleted from index
    Renamed,     ///< R — renamed (staged) or modified (unstaged)
    Copied,      ///< C — copied (staged)
    Untracked,   ///< ?? — not tracked
    UntrackedDir,///< !! — untracked directory (ignored)
    Clean,       ///< space — no changes
};

/// Whether a change is staged or unstaged.
enum class GitStaged
{
    No,   ///< in the working tree, not yet staged
    Yes,  ///< staged in the index
};

/// Merge status of the current branch.
enum class GitMergeStatus
{
    UpToDate,           ///< local == remote-tracking branch
    Ahead,              ///< local is ahead of remote
    Behind,             ///< local is behind remote
    Diverged,           ///< local and remote have diverged
    NoRemote,           ///< no upstream configured
    NoRepository,       ///< not a git repository
    Unknown,            ///< status could not be determined
};

/// Conflict state for a file.
enum class GitConflictState
{
    None,          ///< no conflict
    Unmerged,      ///< conflict in index (both modified, etc.)
    AddedByBoth,   ///< both sides added the file
    DeletedByBoth, ///< both sides deleted the file
    BothModified,  ///< both sides modified the file
    ModifiedDeleted, ///< one side modified, other deleted
    Confusing,     ///< unclassifiable
};

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

/// A single file entry in the Git status (porcelain --untracked-files=all).
struct GitStatusEntry
{
    std::string path;           ///< file path (relative to repo root)
    GitFileStatus status;       ///< file status code
    GitStaged staged;           ///< staged or unstaged
    bool isConflicted = false;  ///< true if in conflict
    GitConflictState conflictState = GitConflictState::None;
    std::string stagedPath;     ///< for renames: the staged (new) path
    std::string unstagedPath;   ///< for renames: the unstaged (new) path
};

/// A staged or unstaged diff hunk for a single file.
struct GitDiffHunk
{
    int oldStartLine = 0;
    int oldLineCount = 0;
    int newStartLine = 0;
    int newLineCount = 0;
    std::string heading;        ///< hunk heading (from the old/new context)
};

/// A single line in a diff.
struct GitDiffLine
{
    char type = ' ';            ///< ' ' (context), '+' (added), '-' (removed)
    std::string content;        ///< the line text (without the type prefix char)
    int lineNumber = 0;         ///< line number in the old or new file
    int oldLineNumber = 0;      ///< line number in the old file (for context)
    int newLineNumber = 0;      ///< line number in the new file (for context)
    std::string oldContent;     ///< original line content (for modifications)
};

/// A file-level diff (old file path → new file path + line-level diff).
struct GitFileDiff
{
    std::string oldPath;
    std::string newPath;
    std::string status;         ///< e.g. "M", "A", "D", "R", "MM", etc.
    bool isBinary = false;
    int addedLines = 0;
    int deletedLines = 0;
    std::vector<GitDiffHunk> hunks;
    std::vector<GitDiffLine> lines;  ///< flat line list for the diff view
};

/// A commit in the repository history.
struct GitCommit
{
    std::string sha;            ///< full 40-char SHA
    std::string shortSha;       ///< abbreviated SHA (7 chars)
    std::string message;        ///< commit message (full)
    std::string subject;        ///< first line of message
    std::string authorName;
    std::string authorEmail;
    std::string authorTime;     ///< ISO-8601 date string
    std::string committerName;
    std::string committerEmail;
    std::string commitTime;     ///< ISO-8601 date string
    std::string parentSha;      ///< first parent (empty for root commits)
    std::vector<std::string> parentShas;  ///< all parents (for merges)
    std::vector<std::string> refs;       ///< branch/tag names pointing here
    bool isHead = false;        ///< is this the HEAD commit?
    bool isCurrentBranch = false; ///< is this on the current branch?
};

/// A branch or tag reference.
struct GitRef
{
    enum class Kind { LocalBranch, RemoteBranch, Tag, Head };
    std::string name;           ///< short name (e.g. "main", "feature/x", "v1.0")
    std::string fullName;       ///< full ref name (e.g. "refs/heads/main")
    std::string sha;            ///< commit SHA this ref points to
    Kind kind;
    bool isCurrent = false;     ///< is this the currently checked-out branch?
    bool isHead = false;        ///< is this HEAD?
};

/// A remote (e.g. "origin").
struct GitRemote
{
    std::string name;
    std::string url;
    std::string fetchSpec;
    std::string pushSpec;
};

/// Repository capabilities (what operations are supported).
struct GitRepoCapabilities
{
    bool hasRepository = false;
    bool hasRemotes = false;
    bool canPush = false;
    bool canFetch = false;
    bool canPull = false;
};

// ---------------------------------------------------------------------------
// GitRepository — async Git repository model
// ---------------------------------------------------------------------------

/**
 * GitRepository
 *
 * The JUCE-free Git repository model. All Git commands are executed
 * asynchronously via GitProcess on a worker thread. Results are cached
 * locally and retrieved via getLatest*() methods on the message thread.
 *
 * Usage:
 *   1. Set the repository path via setRepoPath().
 *  2. Call refreshStatus() / refreshHistory() etc. — these run async.
 *   3. Poll status() and getLatest*() for results, or install callbacks.
 *
 * Thread-safety: All public methods are safe to call from the JUCE message
 * thread. Internally, a worker thread runs git and mutex-guards the cached
 * result. The audio thread should never call these methods directly (though
 * they are technically thread-safe due to the mutex, it's a bad practice).
 */
class GitRepository
{
public:
    GitRepository();
    ~GitRepository();

    // -----------------------------------------------------------------------
    // Repository path management
    // -----------------------------------------------------------------------

    /// Set the repository working-directory path. If empty, no repo is active.
    void setRepoPath(const std::string& path);

    /// Get the current repository path.
    std::string repoPath() const noexcept;

    /// True if the path is inside a valid git repository.
    bool hasRepository() const noexcept;

    /// Initialize a new git repo at the given path. Async — callback on
    /// completion.
    void initRepository(const std::string& path,
                        std::function<void(bool success)> onDone);

    // -----------------------------------------------------------------------
    // Status (async refresh)
    // -----------------------------------------------------------------------

    /// Kick off an async repository status refresh. Results are cached.
    /// onDone is invoked (on a worker thread) when the refresh completes.
    void refreshStatus(std::function<void()> onDone = {});

    /// Get the latest status entries (thread-safe; call from message thread).
    std::vector<GitStatusEntry> getStatusEntries() const;

    /// Get a summary string for the status bar (e.g. "main · 3↑ 1↓").
    std::string getStatusSummary() const;

    /// Get the merge status (ahead/behind/diverged/no-remote).
    GitMergeStatus getMergeStatus() const noexcept;

    /// Get the current branch name.
    std::string getCurrentBranch() const;

    /// Get the HEAD commit SHA (long form).
    std::string getHeadSha() const;

    /// Get all branches, remotes, and tags.
    std::vector<GitRef> getRefs() const;

    /// Get remotes.
    std::vector<GitRemote> getRemotes() const;

    /// Get repository capabilities.
    GitRepoCapabilities getCapabilities() const noexcept;

    // -----------------------------------------------------------------------
    // Staging operations (async)
    // -----------------------------------------------------------------------

    /// Stage a file (or all files if path is empty).
    void stageFile(const std::string& path,
                   std::function<void(bool success)> onDone = {});

    /// Unstage a file.
    void unstageFile(const std::string& path,
                     std::function<void(bool success)> onDone = {});

    /// Discard unstaged changes to a file (revert to index/HEAD).
    void discardFile(const std::string& path,
                     std::function<void(bool success)> onDone = {});

    /// Stage all changes (including untracked).
    void stageAll(std::function<void(bool success)> onDone = {});

    // -----------------------------------------------------------------------
    // Commit (async)
    // -----------------------------------------------------------------------

    /// Create a commit with the given message from the staged changes.
    void commit(const std::string& message,
                std::function<void(bool success, const std::string& error)> onDone = {});

    // -----------------------------------------------------------------------
    // Branch operations (async)
    // -----------------------------------------------------------------------

    /// Create and optionally check out a new branch.
    void createBranch(const std::string& branchName,
                      bool checkout,
                      std::function<void(bool success)> onDone = {});

    /// Switch to a different branch (checkout).
    void checkoutBranch(const std::string& branchName,
                        std::function<void(bool success)> onDone = {});

    /// Delete a branch.
    void deleteBranch(const std::string& branchName,
                      std::function<void(bool success)> onDone = {});

    // -----------------------------------------------------------------------
    // History (async refresh)
    // -----------------------------------------------------------------------

    /// Kick off an async history refresh. Results are cached.
    void refreshHistory(std::function<void()> onDone = {});

    /// Get the latest commit list (newest first).
    std::vector<GitCommit> getHistory() const;

    /// Get a specific commit by SHA (full or abbreviated).
    std::optional<GitCommit> getCommit(const std::string& sha) const;

    // -----------------------------------------------------------------------
    // Diff (async)
    // -----------------------------------------------------------------------

    /// Get the diff for a single file (working tree vs HEAD).
    void getFileDiff(const std::string& path,
                     std::function<void(const GitFileDiff&)> onDone = {});

    /// Get the diff for a specific commit (commit vs its parent).
    void getCommitDiff(const std::string& sha,
                       std::function<void(const GitFileDiff&)> onDone = {});

    /// Get the diff of staged changes for a file (index vs HEAD).
    void getStagedFileDiff(const std::string& path,
                           std::function<void(const GitFileDiff&)> onDone = {});

    /// Get the full working-tree diff (all changed files).
    void getWorkingTreeDiff(
        std::function<void(const std::vector<GitFileDiff>&)> onDone = {});

    // -----------------------------------------------------------------------
    // Push / Pull / Fetch (async)
    // -----------------------------------------------------------------------

    /// Fetch from the default remote.
    void fetch(std::function<void(bool success, const std::string& output)> onDone = {});

    /// Pull (fetch + merge).
    void pull(std::function<void(bool success, const std::string& output)> onDone = {});

    /// Push to the upstream branch.
    void push(std::function<void(bool success, const std::string& output)> onDone = {});

    // -----------------------------------------------------------------------
    // Merge (async)
    // -----------------------------------------------------------------------

    /// Merge a branch into the current branch.
    void merge(const std::string& branchName,
               std::function<void(bool success, const std::string& output,
                                  bool hasConflicts)> onDone = {});

    // -----------------------------------------------------------------------
    // Conflict resolution
    // -----------------------------------------------------------------------

    /// Get conflicted files (files with unmerged status).
    std::vector<GitStatusEntry> getConflicts() const;

    /// Mark a conflicted file as resolved (add to index).
    void resolveConflict(const std::string& path,
                         std::function<void(bool success)> onDone = {});

    // -----------------------------------------------------------------------
    // Process state
    // -----------------------------------------------------------------------

    /// True if a git command is currently in flight.
    bool isBusy() const noexcept;

    /// Request cancellation of the current operation.
    void cancel();

private:
    // -----------------------------------------------------------------------
    // Internal: parse git output formats
    // -----------------------------------------------------------------------

    /// Parse `git status --porcelain --untracked-files=all -z` output.
    std::vector<GitStatusEntry> parsePorcelain(
        const std::string& output) const;

    /// Parse `git log --format=... --parents` output.
    std::vector<GitCommit> parseLog(
        const std::string& output) const;

    /// Parse `git diff --raw` output for file-level changes.
    std::vector<GitFileDiff> parseRawDiff(
        const std::string& output) const;

    /// Parse `git diff` line-level output for a single file.
    GitFileDiff parseLineDiff(
        const std::string& output,
        const std::string& path) const;

    /// Parse `git for-each-ref` output for branches/tags.
    std::vector<GitRef> parseRefs(
        const std::string& output,
        const std::string& currentBranch) const;

    /// Parse `git remote -v` output.
    std::vector<GitRemote> parseRemotes(
        const std::string& output) const;

    /// Parse `git rev-parse` output for upstream/ahead-behind info.
    GitMergeStatus computeMergeStatus(
        const std::string& currentBranch,
        const std::string& upstreamResult,
        const std::string& aheadBehindResult,
        bool upstreamFound) const;

    // -----------------------------------------------------------------------
    // Internal: cached data (mutex-guarded)
    // -----------------------------------------------------------------------

    mutable std::mutex dataMutex_;

    std::string repoPath_;
    bool hasRepository_ = false;

    std::vector<GitStatusEntry> statusEntries_;
    std::string currentBranch_;
    std::string headSha_;
    GitMergeStatus mergeStatus_ = GitMergeStatus::Unknown;
    std::vector<GitRef> refs_;
    std::vector<GitRemote> remotes_;
    std::vector<GitCommit> history_;

    GitProcess process_;

    /// Run a git command synchronously (worker thread). Returns raw output.
    GitProcess::CompletionResult runGit(
        const std::vector<std::string>& args,
        int timeoutMs = 10000);

    GitRepository(const GitRepository&) = delete;
    GitRepository& operator=(const GitRepository&) = delete;
};

} // namespace hathor::ui
