// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * GitGraph.cpp — L-5: native JUCE visual Git commit graph implementation.
 *
 * Renders a commit graph with colored branch lanes, merge commits, HEAD
 * highlighting, and commit messages. Uses a topological layout where each
 * commit is assigned to a horizontal lane (column) based on branch
 * membership, so linear history produces a single straight line and
 * branching/merging history produces diverging/converging lines.
 *
 * Requirement references: L-5 §Git Graph
 */

#include "GitGraph.hpp"

#include <algorithm>
#include <set>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GitGraph::GitGraph()
{
    setSize(kNodeWidth * 4, kRowHeight * 10);  // default size
}

// ---------------------------------------------------------------------------
// Data management
// ---------------------------------------------------------------------------

void GitGraph::setCommits(const std::vector<GitCommit>& commits)
{
    commits_ = commits;
    selectedCommit_ = nullptr;
    branchColors_.clear();
    rebuildLayout();
}

void GitGraph::setCurrentBranch(const std::string& branchName)
{
    currentBranch_ = branchName;
    rebuildLayout();
}

void GitGraph::setHeadSha(const std::string& sha)
{
    headSha_ = sha;
    rebuildLayout();
}

void GitGraph::clear()
{
    commits_.clear();
    selectedCommit_ = nullptr;
    nodes_.clear();
    edges_.clear();
    shaToRow_.clear();
    branchColors_.clear();
    totalGraphHeight_ = 0;
    repaint();
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void GitGraph::assignColors()
{
    // Use the current branch name for the primary color, then assign
    // distinct colors to other branches.
    int colorIdx = 0;

    for (const auto& commit : commits_)
    {
        // Assign color based on refs on this commit
        for (const auto& refName : commit.refs)
        {
            if (branchColors_.find(refName) == branchColors_.end())
            {
                branchColors_[refName] = GitGraphColors::branchColor(colorIdx++ % 8);
            }
        }
    }

    // Ensure current branch has a color (default to green/accent).
    if (branchColors_.find(currentBranch_) == branchColors_.end())
        branchColors_[currentBranch_] = GitGraphColors::branchColor(0);
}

juce::Colour GitGraph::colorForBranch(const std::string& branchName)
{
    auto it = branchColors_.find(branchName);
    if (it != branchColors_.end())
        return it->second;
    return HathorLookAndFeel::fromComponent(*this).getPalette().accent;
}

juce::Colour GitGraph::colorForCommit(const std::string& sha)
{
    for (const auto& node : nodes_)
    {
        if (node.commit && node.commit->sha == sha)
            return node.color;
    }
    return HathorLookAndFeel::fromComponent(*this).getPalette().accent;
}

void GitGraph::rebuildLayout()
{
    nodes_.clear();
    edges_.clear();
    shaToRow_.clear();
    assignColors();

    if (commits_.empty())
    {
        totalGraphHeight_ = kRowHeight;
        return;
    }

    // Build SHA → row mapping (commits_ is newest-first, row 0 = newest).
    for (int i = 0; i < static_cast<int>(commits_.size()); ++i)
    {
        shaToRow_[commits_[i].sha] = i;
    }

    // Track which lane each SHA occupies (for the line routing algorithm).
    // We use a simple "longest path from root" approach: each commit's
    // column is determined by its ancestry graph.
    //
    // Algorithm:
    //   - Process commits oldest-first (reverse order).
    //   - For each commit, its column is one more than the max column of
    //     its children (so parents are to the left of children).
    //   - Track active branches and their lane assignments.

    // We assign columns based on a simple lane-allocation algorithm:
    // - Each commit gets a column. Children are to the right, parents to
    //   the left (or same column for linear history).
    // - When a commit has multiple parents (merge), we create new lanes.

    std::map<std::string, int> shaToColumn;
    std::set<int> usedColumns;

    // Process oldest-first to assign columns bottom-up.
    for (int i = static_cast<int>(commits_.size()) - 1; i >= 0; --i)
    {
        const auto& commit = commits_[i];
        int column = 0;

        // Find the max column among parents (already processed since we go oldest-first).
        for (const auto& parentSha : commit.parentShas)
        {
            auto it = shaToColumn.find(parentSha);
            if (it != shaToColumn.end())
                column = std::max(column, it->second + 1);
        }

        // Also, if this commit is the child of a parent, the parent should
        // be in the same column or adjacent.
        // Find the column where this commit's children are (already processed).
        // Actually, for a clean graph, we want commits in the same branch
        // to share a column. Let's use a simpler approach:
        //
        // Use the "git graph" style: assign columns based on branch membership.

        // Check if any child of this commit already has a column.
        for (int j = i + 1; j < static_cast<int>(commits_.size()); ++j)
        {
            const auto& child = commits_[j];
            for (const auto& pSha : child.parentShas)
            {
                if (pSha == commit.sha)
                {
                    // This commit is a parent of child j.
                    // We want this commit in the same column as the child
                    // if it's a linear parent-child chain.
                    auto cit = shaToColumn.find(child.sha);
                    if (cit != shaToColumn.end())
                    {
                        column = std::max(column, cit->second);
                    }
                }
            }
        }

        shaToColumn[commit.sha] = column;
        usedColumns.insert(column);
    }

    // Build nodes.
    int numColumns = usedColumns.empty() ? 1 : *usedColumns.rbegin() + 1;
    // Limit to a reasonable number.
    numColumns = std::min(numColumns, 8);

    for (int i = 0; i < static_cast<int>(commits_.size()); ++i)
    {
        const auto& commit = commits_[i];
        GitGraphNode node;
        node.commit = &commit;
        node.row = i;

        auto cit = shaToColumn.find(commit.sha);
        node.column = (cit != shaToColumn.end()) ? cit->second : 0;
        node.isHead = (commit.sha == headSha_ || commit.shortSha == headSha_);
        node.isCurrentBranchTip = (!currentBranch_.empty() && !commit.refs.empty());
        node.isMerge = commit.parentShas.size() > 1;
        node.branchLabels = commit.refs;

        // Determine node color: use the current branch color, or the first
        // ref color.
        if (!commit.refs.empty())
            node.color = colorForBranch(commit.refs.front());
        else
        {
            // No refs — color by ancestry (find the branch color that reaches here).
            // Use the color of the nearest child that has a ref.
            node.color = colorForBranch(currentBranch_);
        }

        nodes_.push_back(node);
    }

    // Build edges: connect each commit to its parents.
    for (const auto& node : nodes_)
    {
        if (!node.commit)
            continue;

        for (const auto& parentSha : node.commit->parentShas)
        {
            auto pit = shaToRow_.find(parentSha);
            if (pit == shaToRow_.end())
                continue;

            int parentRow = pit->second;
            auto cit = shaToColumn.find(node.commit->sha);
            auto pitCol = shaToColumn.find(parentSha);
            if (cit == shaToColumn.end() || pitCol == shaToColumn.end())
                continue;

            GitGraphEdge edge;
            edge.fromRow = node.row;      // child (newer)
            edge.toRow = parentRow;       // parent (older)
            edge.fromCol = cit->second;
            edge.toCol = pitCol->second;
            edge.color = node.color;
            edge.isCurrentBranch = (node.commit->sha == headSha_);

            // For merge commits, draw edges to all parents with their
            // respective branch colors.
            if (node.isMerge)
            {
                // Find the color of the parent's branch.
                auto parentIt = std::find_if(nodes_.begin(), nodes_.end(),
                    [&](const GitGraphNode& n) {
                        return n.commit && n.commit->sha == parentSha;
                    });
                if (parentIt != nodes_.end())
                    edge.color = parentIt->color;
            }

            edges_.push_back(edge);
        }
    }

    totalGraphHeight_ = static_cast<int>(commits_.size()) * kRowHeight;
}

juce::Rectangle<int> GitGraph::getCommitBounds(int row) const
{
    // The graph column is the left part; the message area is to the right.
    const int graphX = 8;
    const int messageX = graphX + kGraphColumnWidth + 8;
    const int y = row * kRowHeight + 4;
    const int w = kNodeWidth - graphX - kGraphColumnWidth;

    return { messageX, y, w, kRowHeight - 8 };
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void GitGraph::resized()
{
    // The component's height is determined by the content; we set a minimum.
    // The parent (ScrollView or Viewport) handles scrolling.
    const int contentW = kNodeWidth * std::max(1, static_cast<int>(commits_.size()) / 5 + 1);
    const int contentH = std::max(getHeight(), totalGraphHeight_);
    setSize(std::max(getWidth(), contentW), contentH);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void GitGraph::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Background
    g.fillAll(palette.background);

    if (commits_.empty())
    {
        g.setColour(palette.textMuted);
        g.setFont(HathorLookAndFeel::uiFontRegular(12.0f));
        g.drawText("No commits to display.",
                   getLocalBounds().removeFromTop(100),
                   juce::Justification::centredTop, false);
        return;
    }

    // Draw edges (branch lines) first, so nodes paint on top.
    drawEdges(g);

    // Draw nodes and commit info.
    for (const auto& node : nodes_)
    {
        juce::Rectangle<int> rowBounds(
            8,
            node.row * kRowHeight + 4,
            getWidth() - 16,
            kRowHeight - 8
        );
        drawNode(g, node, rowBounds);
        drawLabels(g, node, rowBounds);
        drawCommitMessage(g, node, rowBounds);
    }
}

void GitGraph::drawEdges(juce::Graphics& g)
{
    if (commits_.empty() || nodes_.empty())
        return;

    // Number of columns and column width.
    int numColumns = 1;
    for (const auto& node : nodes_)
        numColumns = std::max(numColumns, node.column + 1);

    const int graphX = 8;
    const int columnWidth = kGraphColumnWidth / std::max(1, numColumns);

    for (const auto& edge : edges_)
    {
        // Compute X positions for from and to columns.
        int fromX = graphX + edge.fromCol * columnWidth + columnWidth / 2;
        int toX = graphX + edge.toCol * columnWidth + columnWidth / 2;

        int fromY = edge.fromRow * kRowHeight + kRowHeight / 2;
        int toY = edge.toRow * kRowHeight + kRowHeight / 2;

        // Draw a line (or bezier curve for merges).
        g.setColour(edge.color.withAlpha(edge.isCurrentBranch ? 1.0f : 0.6f));
        g.drawLine(static_cast<float>(fromX), static_cast<float>(fromY),
                   static_cast<float>(toX), static_cast<float>(toY),
                   edge.isCurrentBranch ? 3.0f : 2.0f);

        // For merge edges (from != to column), draw a small curve.
        if (edge.fromCol != edge.toCol && !edge.isCurrentBranch)
        {
            juce::Path path;
            const float midY = (fromY + toY) / 2.0f;
            path.startNewSubPath(static_cast<float>(fromX), static_cast<float>(fromY));
            path.cubicTo(static_cast<float>(fromX), midY,
                         static_cast<float>(toX), midY,
                         static_cast<float>(toX), static_cast<float>(toY));
            g.strokePath(path, juce::PathStrokeType(2.0f));
        }
    }
}

void GitGraph::drawNode(juce::Graphics& g, const GitGraphNode& node,
                        const juce::Rectangle<int>& bounds)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Node position: centered in the graph column, vertically centered in the row.
    int numColumns = 1;
    for (const auto& n : nodes_)
        numColumns = std::max(numColumns, n.column + 1);

    const int columnWidth = kGraphColumnWidth / std::max(1, numColumns);
    const int nodeX = 8 + node.column * columnWidth + columnWidth / 2 - 10;
    const int nodeY = bounds.getY() + (bounds.getHeight() - 20) / 2;

    // Determine if this node is selected.
    bool isSelected = (selectedCommit_ != nullptr &&
                       node.commit && selectedCommit_->sha == node.commit->sha);

    // Node circle/rectangle.
    juce::Colour nodeColor = node.isHead ? palette.accent : node.color;
    if (isSelected)
        nodeColor = palette.accent;

    if (node.isMerge)
    {
        // Merge commits: draw a diamond (octagon-like).
        juce::Path diamond;
        const int size = 16;
        diamond.addRectangle(nodeX, nodeY, size, size);
        // Actually, draw a hexagon/diamond for merges.
        juce::Path diamondPath;
        diamondPath.startNewSubPath(static_cast<float>(nodeX + size / 2),
                                    static_cast<float>(nodeY));
        diamondPath.lineTo(static_cast<float>(nodeX + size),
                           static_cast<float>(nodeY + size / 2));
        diamondPath.lineTo(static_cast<float>(nodeX + size),
                           static_cast<float>(nodeY + size / 2 + size / 2));
        // Simplify: just draw a filled rectangle with accent for merges.
        g.setColour(nodeColor);
        g.fillEllipse(static_cast<float>(nodeX), static_cast<float>(nodeY),
                      20.0f, 20.0f);
        // Ring for merge commits.
        g.setColour(palette.background.withAlpha(0.7f));
        g.drawEllipse(static_cast<float>(nodeX), static_cast<float>(nodeY),
                      20.0f, 20.0f, 2.0f);
    }
    else
    {
        // Regular commit: filled circle.
        g.setColour(node.isHead ? palette.accent : nodeColor.darker(0.2f));
        g.fillEllipse(static_cast<float>(nodeX) - 2, static_cast<float>(nodeY) - 2,
                      24.0f, 24.0f);
        // Inner fill.
        g.setColour(palette.background);
        g.fillEllipse(static_cast<float>(nodeX) + 2, static_cast<float>(nodeY) + 2,
                      20.0f, 20.0f);
        // Outline.
        g.setColour(nodeColor);
        g.drawEllipse(static_cast<float>(nodeX), static_cast<float>(nodeY),
                      20.0f, 20.0f, 2.0f);
    }

    // If selected, draw a thicker outline.
    if (isSelected)
    {
        g.setColour(palette.accent);
        g.drawEllipse(static_cast<float>(nodeX) - 4, static_cast<float>(nodeY) - 4,
                      28.0f, 28.0f, 2.0f);
    }

    // Draw the short SHA inside the node.
    if (node.commit)
    {
        g.setColour(palette.textPrimary);
        g.setFont(HathorLookAndFeel::uiFontRegular(9.0f));
        juce::String shaText(node.commit->shortSha);
        if (shaText.isEmpty())
            shaText = node.commit->sha.substr(0, 7);
        // Only show if it fits.
        g.drawText(shaText,
                   nodeX, nodeY + 2, 20, 16,
                   juce::Justification::centred, false);
    }
}

void GitGraph::drawLabels(juce::Graphics& g, const GitGraphNode& node,
                          const juce::Rectangle<int>& bounds)
{
    if (node.branchLabels.empty())
        return;

    g.setFont(HathorLookAndFeel::uiFontMedium(10.0f));

    int numColumns = 1;
    for (const auto& n : nodes_)
        numColumns = std::max(numColumns, n.column + 1);

    const int columnWidth = kGraphColumnWidth / std::max(1, numColumns);
    const int labelX = 8 + node.column * columnWidth + columnWidth;
    const int labelY = bounds.getY();

    for (size_t i = 0; i < node.branchLabels.size(); ++i)
    {
        const std::string& label = node.branchLabels[i];
        juce::Colour labelColor = colorForBranch(label);

        g.setColour(labelColor);
        juce::String labelStr(label);
        if (label == currentBranch_)
            labelStr = label + " (current)";

        g.drawText(labelStr,
                   labelX, labelY + static_cast<int>(i * 14),
                   120, 14,
                   juce::Justification::centredLeft, false);
    }
}

void GitGraph::drawCommitMessage(juce::Graphics& g, const GitGraphNode& node,
                                 const juce::Rectangle<int>& bounds)
{
    if (!node.commit)
        return;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    const int msgX = 8 + kGraphColumnWidth + 8;
    const int msgY = bounds.getY();

    // Commit subject
    juce::String msg = node.commit->subject.empty()
        ? juce::String(node.commit->message)
        : juce::String(node.commit->subject);

    // Highlight if it's HEAD.
    if (node.isHead)
        msg = "* " + msg;

    g.setColour(node.isHead ? palette.accent : palette.textPrimary);
    g.setFont(HathorLookAndFeel::uiFontMedium(12.0f));
    g.drawText(msg,
               msgX, msgY,
               bounds.getWidth() - kGraphColumnWidth - 16, 16,
               juce::Justification::centredLeft, false);

    // Author and date (smaller, muted)
    juce::String authorDate = node.commit->authorName;
    if (!node.commit->authorTime.empty())
    {
        authorDate += " · ";
        // Trim ISO timestamp to date only.
        std::string time = node.commit->authorTime;
        if (time.size() > 10)
            authorDate += juce::String(time.substr(0, 10));
        else
            authorDate += juce::String(time);
    }

    g.setColour(palette.textMuted);
    g.setFont(HathorLookAndFeel::uiFontRegular(10.0f));
    g.drawText(authorDate,
               msgX, msgY + 16,
               bounds.getWidth() - kGraphColumnWidth - 16, 14,
               juce::Justification::centredLeft, false);
}

// ---------------------------------------------------------------------------
// Mouse input
// ---------------------------------------------------------------------------

void GitGraph::mouseDown(const juce::MouseEvent& e)
{
    // Check if a node was clicked.
    for (const auto& node : nodes_)
    {
        if (!node.commit)
            continue;

        int numColumns = 1;
        for (const auto& n : nodes_)
            numColumns = std::max(numColumns, n.column + 1);

        const int columnWidth = kGraphColumnWidth / std::max(1, numColumns);
        const int nodeX = 8 + node.column * columnWidth + columnWidth / 2 - 10;
        const int nodeY = node.row * kRowHeight + 4 + (kRowHeight - 20) / 2;

        juce::Rectangle<int> nodeBounds(nodeX, nodeY, 20, 20);
        if (nodeBounds.contains(e.position.toInt()))
        {
            selectedCommit_ = node.commit;
            if (onCommitSelected)
                onCommitSelected(*node.commit);
            repaint();
            return;
        }
    }

    // Check if a branch label was clicked.
    for (const auto& node : nodes_)
    {
        for (const auto& label : node.branchLabels)
        {
            int numColumns = 1;
            for (const auto& n : nodes_)
                numColumns = std::max(numColumns, n.column + 1);

            const int columnWidth = kGraphColumnWidth / std::max(1, numColumns);
            const int labelX = 8 + node.column * columnWidth + columnWidth;
            const int labelY = node.row * kRowHeight + 4;

            juce::Rectangle<int> labelBounds(labelX, labelY, 120, 14);
            if (labelBounds.contains(e.position.toInt()))
            {
                if (onBranchClicked)
                    onBranchClicked(label);
                repaint();
                return;
            }
        }
    }
}

} // namespace hathor::ui
