// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GitDiffView.cpp — L-5: native JUCE side-by-side diff view implementation.
 *
 * Renders old/HEAD on the left and current/new on the right, with
 * color-coded added (green) / removed (red) / context (default) lines,
 * line numbers on both sides, file headers with stats, and navigation
 * between hunks.
 *
 * Requirement references: L-5 §Diff View
 */

#include "GitDiffView.hpp"

#include <algorithm>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GitDiffView::GitDiffView()
{
    // Create the two text editors for left (old) and right (new) sides.
    leftEditor_ = std::make_unique<juce::TextEditor>();
    rightEditor_ = std::make_unique<juce::TextEditor>();

    leftEditor_->setReadOnly(true);
    rightEditor_->setReadOnly(true);
    leftEditor_->setFont(HathorLookAndFeel::fontRegular(13.0f));
    rightEditor_->setFont(HathorLookAndFeel::fontRegular(13.0f));

    addAndMakeVisible(leftEditor_.get());
    addAndMakeVisible(rightEditor_.get());

    // Navigation buttons
    nextHunkBtn_ = std::make_unique<juce::TextButton>("Next Hunk");
    prevHunkBtn_ = std::make_unique<juce::TextButton>("Prev Hunk");
    closeBtn_ = std::make_unique<juce::TextButton>("Close");

    nextHunkBtn_->onClick = [this]() { navigateNext(); };
    prevHunkBtn_->onClick = [this]() { navigatePrev(); };
    closeBtn_->onClick = [this]() {
        if (onClosePanel)
            onClosePanel();
    };

    addAndMakeVisible(nextHunkBtn_.get());
    addAndMakeVisible(prevHunkBtn_.get());
    addAndMakeVisible(closeBtn_.get());

    // File label
    fileLabel_ = std::make_unique<juce::Label>();
    fileLabel_->setFont(HathorLookAndFeel::fontMedium(12.0f));
    addAndMakeVisible(fileLabel_.get());
}

GitDiffView::~GitDiffView() = default;

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------

void GitDiffView::setDiff(const GitFileDiff& diff)
{
    sourceDiffs_.clear();
    sourceDiffs_.push_back(diff);
    buildLines();
}

void GitDiffView::setDiff(const std::vector<GitFileDiff>& diffs)
{
    sourceDiffs_ = diffs;
    buildLines();
}

void GitDiffView::clear()
{
    sourceDiffs_.clear();
    sections_.clear();
    contentHeight_ = 0;
    currentHunkIndex_ = -1;
    leftEditor_->clear();
    rightEditor_->clear();
    fileLabel_->setText("", juce::dontSendNotification);
    repaint();
}

// ---------------------------------------------------------------------------
// Internal: build the flat line model
// ---------------------------------------------------------------------------

void GitDiffView::buildLines()
{
    sections_.clear();
    contentHeight_ = 0;

    for (const auto& diff : sourceDiffs_)
    {
        DiffFileSection section;
        section.oldPath = diff.oldPath;
        section.newPath = diff.newPath;
        section.status = diff.status;

        int oldLineNum = 0;
        int newLineNum = 0;

        for (const auto& dl : diff.lines)
        {
            DiffLine line;
            line.type = dl.type;
            line.content = dl.content;

            if (line.type == ' ')
            {
                // Context line: present in both old and new.
                // Use the line numbers from the diff if available.
                if (dl.oldLineNumber > 0)
                    oldLineNum = dl.oldLineNumber;
                if (dl.newLineNumber > 0)
                    newLineNum = dl.newLineNumber;
                line.oldLineNumber = ++oldLineNum;
                line.newLineNumber = ++newLineNum;
            }
            else if (line.type == '+')
            {
                // Added line: only in new.
                if (dl.newLineNumber > 0)
                    newLineNum = dl.newLineNumber - 1;
                line.oldLineNumber = 0;  // not present in old
                line.newLineNumber = ++newLineNum;
            }
            else if (line.type == '-')
            {
                // Removed line: only in old.
                if (dl.oldLineNumber > 0)
                    oldLineNum = dl.oldLineNumber - 1;
                line.oldLineNumber = ++oldLineNum;
                line.newLineNumber = 0;  // not present in new
            }

            section.lines.push_back(line);
        }

        // Count hunk boundaries for navigation.
        for (size_t i = 0; i < section.lines.size(); ++i)
        {
            const auto& line = section.lines[i];
            if (line.type == '+' || line.type == '-')
            {
                // Mark the first line of a changed region.
                if (i == 0 || section.lines[i - 1].type == ' ')
                    section.lines[i].isHunkBoundary = true;
            }
        }

        section.yOffset = contentHeight_;
        section.height = static_cast<int>(section.lines.size()) * kLineHeight;
        contentHeight_ += section.height;

        sections_.push_back(std::move(section));
    }

    // Rebuild the text editors with the diff content.
    juce::String leftText, rightText;
    for (const auto& section : sections_)
    {
        for (const auto& line : section.lines)
        {
            if (line.type == ' ' || line.type == '-')
                leftText << juce::String(line.content) << "\n";
            else
                leftText << "\n";  // gap on the left for added lines

            if (line.type == ' ' || line.type == '+')
                rightText << juce::String(line.content) << "\n";
            else
                rightText << "\n";  // gap on the right for removed lines
        }
    }

    leftEditor_->setText(leftText, juce::dontSendNotification);
    rightEditor_->setText(rightText, juce::dontSendNotification);

    // Set file label.
    if (!sections_.empty())
    {
        const auto& s = sections_.front();
        juce::String label = s.oldPath;
        if (s.oldPath != s.newPath && !s.newPath.empty())
            label = s.oldPath + " → " + s.newPath;
        fileLabel_->setText(label, juce::dontSendNotification);
    }
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

bool GitDiffView::canNavigateNext() const noexcept
{
    // Count total hunk boundaries.
    int count = 0;
    for (const auto& section : sections_)
    {
        for (const auto& line : section.lines)
        {
            if (line.isHunkBoundary)
                ++count;
        }
    }
    return count > 0 && (currentHunkIndex_ < count - 1);
}

bool GitDiffView::canNavigatePrev() const noexcept
{
    return currentHunkIndex_ > 0;
}

void GitDiffView::navigateNext()
{
    scrollToHunk(1);
}

void GitDiffView::navigatePrev()
{
    scrollToHunk(-1);
}

void GitDiffView::scrollToHunk(int direction)
{
    // Find all hunk boundary positions.
    std::vector<int> hunkY;
    int y = 0;
    for (const auto& section : sections_)
    {
        for (const auto& line : section.lines)
        {
            if (line.isHunkBoundary)
            {
                // Compute the Y position of this hunk relative to current scroll.
                hunkY.push_back(y);
            }
            y += kLineHeight;
        }
    }

    if (hunkY.empty())
        return;

    if (direction > 0)
    {
        // Find the first hunk after the current scroll position.
        for (int pos : hunkY)
        {
            if (pos > scrollY_ + kLineHeight)
            {
                scrollY_ = pos - kLineHeight;
                break;
            }
        }
    }
    else
    {
        // Find the last hunk before the current scroll position.
        for (int i = static_cast<int>(hunkY.size()) - 1; i >= 0; --i)
        {
            if (hunkY[i] < scrollY_ - kLineHeight)
            {
                scrollY_ = hunkY[i];
                break;
            }
        }
    }

    scrollY_ = std::clamp(scrollY_, 0, std::max(0, contentHeight_ - 1));
    repaint();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void GitDiffView::resized()
{
    auto b = getLocalBounds().reduced(kMargin);

    // Header buttons (left side)
    closeBtn_->setBounds(b.removeFromTop(kHeaderHeight).removeFromRight(80).withHeight(24).translated(-4, 4));

    // Reset b to account for the header.
    b = getLocalBounds().reduced(kMargin);
    b.removeFromTop(kHeaderHeight + 4);

    // Navigation buttons
    auto buttonArea = b.removeFromBottom(28);
    prevHunkBtn_->setBounds(buttonArea.removeFromLeft(100).withHeight(24));
    nextHunkBtn_->setBounds(buttonArea.removeFromLeft(100).withHeight(24).translated(8, 0));

    // File label (below buttons, on the left)
    auto labelArea = getLocalBounds().reduced(kMargin);
    labelArea.removeFromBottom(28);
    labelArea.removeFromTop(kHeaderHeight + 4);
    fileLabel_->setBounds(labelArea.removeFromTop(22));

    b = b.withTrimmedTop(22);

    // Splitter and two editors
    leftEditor_->setBounds(b.removeFromLeft(b.getWidth() / 2 - kSplitterWidth / 2));
    b.removeFromLeft(kSplitterWidth);  // splitter gap
    rightEditor_->setBounds(b);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void GitDiffView::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Background
    g.fillAll(palette.surfaceLow);

    // Splitter line
    if (getWidth() > 2 * kSplitterWidth)
    {
        int splitX = getWidth() / 2;
        g.setColour(palette.surfaceHighest);
        g.drawVerticalLine(splitX, 0.0f, static_cast<float>(getHeight()));
    }

    // Draw file headers and line-by-line diff coloring.
    // This overlays color behind the text editors.
    int y = kHeaderHeight + 24 + 22;  // below buttons, labels, and file label

    for (const auto& section : sections_)
    {
        // File header background
        g.setColour(palette.surfaceContainer);
        g.fillRect(0, y + section.yOffset, getWidth(), kLineHeight * 2);

        // Draw a separator line
        g.setColour(palette.surfaceHighest);
        g.fillRect(0, y + section.yOffset + kLineHeight * 2 - 1, getWidth(), 1);

        // Draw each line's background color
        int lineY = y + section.yOffset + kLineHeight * 2;
        for (const auto& line : section.lines)
        {
            juce::Colour bgColor;
            if (line.type == '+')
                bgColor = palette.accent.withAlpha(0.15f);
            else if (line.type == '-')
                bgColor = palette.error.withAlpha(0.15f);

            if (!bgColor.isTransparent())
            {
                g.setColour(bgColor);
                g.fillRect(0, lineY, getWidth(), kLineHeight);
            }

            lineY += kLineHeight;
        }
    }
}

} // namespace hathor::ui
