// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GitDiffView.hpp — L-5: native JUCE side-by-side diff view.
 *
 * Renders a Git diff in a side-by-side layout:
 *
 *   OLD / HEAD                  CURRENT
 *   ────────────               ────────────
 *   removed line               added line
 *   unchanged                  unchanged
 *
 * Features:
 *   - Added lines (green background)
 *   - Removed lines (red background, strikethrough for removed, underline for added)
 *   - Modified regions (highlighted)
 *   - File-level diff (shows all changed files)
 *   - Commit-level diff (shows all changed files for a commit)
 *   - Read-only mode (default — the diff is never editable)
 *   - Navigation between changed regions (Next/Previous hunk buttons)
 *   - Line numbers on both sides
 *   - Correlation lines connecting old ↔ new positions for changed regions
 *
 * The view consumes GitFileDiff structures from GitRepository and computes
 * a line-by-line alignment using the Myers diff algorithm (simplified LCS
 * implementation). It does NOT maintain its own copy of the file content —
 * it only displays the diff that Git itself produced.
 *
 * Requirement references: L-5 §Diff View
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "GitRepository.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

/**
 * GitDiffView
 *
 * A JUCE component that displays a Git diff side-by-side. It takes a
 * GitFileDiff (or a vector of them for a full commit diff) and renders
 * the old/new versions with color-coded added/removed/context lines.
 *
 * The view is read-only — the user cannot edit the diff content.
 */
class GitDiffView : public juce::Component,
                    private juce::TextEditor::Listener
{
public:
    GitDiffView();
    ~GitDiffView() override;

    // ---------------------------------------------------------------------------
    // Data
    // ---------------------------------------------------------------------------

    /**
     * Set the diff to display. If multiple file diffs are provided, they are
     * laid out sequentially with file headers.
     */
    void setDiff(const GitFileDiff& diff);
    void setDiff(const std::vector<GitFileDiff>& diffs);

    /** Clear the diff. */
    void clear();

    // ---------------------------------------------------------------------------
    // Navigation
    // ---------------------------------------------------------------------------

    /** Navigate to the next changed region (hunk). */
    void navigateNext();

    /** Navigate to the previous changed region (hunk). */
    void navigatePrev();

    /** True if there is a next/previous hunk. */
    bool canNavigateNext() const noexcept;
    bool canNavigatePrev() const noexcept;

    // -----------------------------------------------------------------------
    // Read-only mode
    // -----------------------------------------------------------------------

    /** The diff is always read-only. This is a no-op that exists for API
        completeness. */
    void setReadOnly(bool /*readOnly*/) noexcept {}

    // ---------------------------------------------------------------------------
    // Callbacks
    // ---------------------------------------------------------------------------

    /// Fired when the user clicks a file header to navigate to that file.
    std::function<void(const std::string& path)> onFileSelected;

    /// Fired when the view is closed.
    std::function<void()> onClosePanel;

    // ---------------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------

    void resized() override;
    void paint(juce::Graphics& g) override;

    // ---------------------------------------------------------------------------
    // juce::TextEditor::Listener
    // ---------------------------------------------------------------------------

    void textEditorTextChanged(juce::TextEditor&) override {}
    void textEditorFocusLost(juce::TextEditor&) override {}

    // -----------------------------------------------------------------------
    // Layout constants
    // -----------------------------------------------------------------------

    static constexpr int kLineHeight      = 16;
    static constexpr int kLineNumberWidth = 60;
    static constexpr int kSplitterWidth   = 1;
    static constexpr int kHeaderHeight    = 60;
    static constexpr int kMargin          = 8;

private:
    // ---------------------------------------------------------------------------
    // Internal diff line model
    // ---------------------------------------------------------------------------

    /// A rendered diff line with computed alignment data.
    struct DiffLine
    {
        char type = ' ';              ///< ' ', '+', '-'
        std::string content;
        int oldLineNumber = 0;         ///< line number in the old file (0 = none)
        int newLineNumber = 0;         ///< line number in the new file (0 = none)
        bool isHunkBoundary = false;   ///< true if this line starts a new hunk
        std::string hunkHeader;        ///< hunk header text (e.g. "@@ -1,5 +1,6 @@")
    };

    /// A file section in the diff.
    struct DiffFileSection
    {
        std::string oldPath;
        std::string newPath;
        std::string status;
        int yOffset = 0;               ///< pixel Y offset within the full diff
        int height = 0;                ///< pixel height of this section
        std::vector<DiffLine> lines;
    };

    // ---------------------------------------------------------------------------
    // Internal helpers
    // ---------------------------------------------------------------------------

    /** Convert a GitFileDiff into the flat DiffLine model. */
    void buildLines();

    /** Paint a single diff line (old side / new side). */
    void paintLine(juce::Graphics& g, const DiffLine& line, int y,
                   bool isLeftSide);

    /** Paint a file header (old/new path + stats). */
    void paintFileHeader(juce::Graphics& g, const DiffFileSection& section,
                         int y);

    /** Compute the total content height. */
    int computeContentHeight() const;

    /** Scroll to the next/previous hunk boundary. */
    void scrollToHunk(int direction);

    // ---------------------------------------------------------------------------
    // Child components
    // ---------------------------------------------------------------------------

    std::unique_ptr<juce::TextEditor> leftEditor_;
    std::unique_ptr<juce::TextEditor> rightEditor_;
    std::unique_ptr<juce::TextButton> closeBtn_;
    std::unique_ptr<juce::TextButton> nextHunkBtn_;
    std::unique_ptr<juce::TextButton> prevHunkBtn_;
    std::unique_ptr<juce::Label>      fileLabel_;

    // ---------------------------------------------------------------------------
    // State
    // ---------------------------------------------------------------------------

    std::vector<GitFileDiff> sourceDiffs_;
    std::vector<DiffFileSection> sections_;

    int scrollY_ = 0;
    int contentHeight_ = 0;
    int currentHunkIndex_ = -1;  ///< index into hunk boundaries

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GitDiffView)
};

} // namespace hathor::ui
