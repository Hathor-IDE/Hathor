// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * GitGraph.hpp — L-5: native JUCE visual Git commit graph.
 *
 * Renders a commit graph with:
 *   - Commit nodes (circles with short SHA)
 *   - Branch lines connecting parents and children
 *   - Branch/ref labels (colored) beside relevant nodes
 *   - HEAD/current-branch highlight (filled node + colored line)
 *   - Merge commit styling (octagon or double-border)
 *   - Commit messages beside each node
 *   - Commit details on selection (author, date, message)
 *
 * The graph is laid out top-to-bottom by commit date, with branches
 * rendered as colored columns. For linear history, all commits fall in
 * a single column, producing a single straight line. For branching/merging
 * histories, lines diverge and converge visually.
 *
 * This is a pure visualization of Git's repository model — it does NOT
 * invent or maintain a separate graph. It reads from GitRepository's
 * commit list and parent relationships.
 *
 * Requirement references: L-5 §Git Graph
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "GitRepository.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

/// Color palette for distinct branches in the graph.
struct GitGraphColors
{
    static constexpr juce::Colour branchColors[] = {
        juce::Colours::green,
        juce::Colours::blue,
        juce::Colours::yellow,
        juce::Colours::orange,
        juce::Colours::magenta,
        juce::Colours::cyan,
        juce::Colours::red,
        juce::Colours::greyLavender,
    };
    static constexpr int numColors = 8;
};

/// A node in the graph layout — a commit positioned at a row and column.
struct GitGraphNode
{
    const GitCommit* commit = nullptr;  ///< nullptr if not resolved
    int row = 0;                         ///< vertical position (0 = newest)
    int column = 0;                      ///< horizontal lane (branch lane)
    juce::Colour color;                  ///< branch color for this node
    bool isHead = false;                 ///< is this HEAD?
    bool isCurrentBranchTip = false;     ///< is this the current branch tip?
    bool isMerge = false;                ///< is this a merge commit?
    std::vector<std::string> branchLabels; ///< branch names on this commit
};

/// A line segment connecting two nodes in the graph.
struct GitGraphEdge
{
    int fromRow = 0;
    int fromCol = 0;
    int toRow = 0;
    int toCol = 0;
    juce::Colour color;
    bool isCurrentBranch = false;
};

/**
 * GitGraph
 *
 * A JUCE component that renders the commit graph. It receives a list of
 * GitCommit objects (from GitRepository) and computes a 2D layout where:
 *   - Each commit occupies a row (top = newest).
 *   - Branches occupy columns (lanes).
 *   - Lines connect each commit to its parent(s).
 *
 * Linear history naturally produces a single column (one straight line).
 * Branching/merging produces diverging/converging lines.
 */
class GitGraph : public juce::Component
{
public:
    GitGraph();
    ~GitGraph() override = default;

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------

    /** Set the commits to display and rebuild the layout.
        Commits should be sorted newest-first. */
    void setCommits(const std::vector<GitCommit>& commits);

    /** Set the current branch name (for HEAD highlighting). */
    void setCurrentBranch(const std::string& branchName);

    /** Set the HEAD commit SHA. */
    void setHeadSha(const std::string& sha);

    /** Clear all data. */
    void clear();

    /** Rebuild the graph layout (called automatically when data changes). */
    void rebuildLayout();

    // -----------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

    // -----------------------------------------------------------------------
    // Interaction
    // -----------------------------------------------------------------------

    /// Height of a single commit row.
    static constexpr int kRowHeight = 60;

    /// Horizontal spacing between commit nodes.
    static constexpr int kNodeWidth = 320;

    /// Width of the graph column (node + lines).
    static constexpr int kGraphColumnWidth = 40;

    /// Callback fired when a commit node is clicked.
    std::function<void(const GitCommit& commit)> onCommitSelected;

    /// Callback fired when a branch label is clicked.
    std::function<void(const std::string& branchName)> onBranchClicked;

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    /** Get the currently selected commit, or nullptr. */
    const GitCommit* selectedCommit() const noexcept { return selectedCommit_; }

    /** Get the layout bounds of a commit at the given row (for scrolling). */
    juce::Rectangle<int> getCommitBounds(int row) const;

private:
    // -----------------------------------------------------------------------
    // Layout computation
    // -----------------------------------------------------------------------

    /**
     * Compute the graph layout: assign each commit to a row (index in the
     * list) and a column (lane). The algorithm:
     *   1. Process commits oldest-first (reverse of the list).
     *   2. For each commit, assign it to a lane based on its parentage.
     *   3. Use color to track branch membership.
     */
    std::vector<GitGraphNode> nodes_;
    std::vector<GitGraphEdge> edges_;

    /// Map from SHA → row index.
    std::map<std::string, int> shaToRow_;

    /// Map from branch name → color.
    std::map<std::string, juce::Colour> branchColors_;

    void assignColors();
    juce::Colour colorForBranch(const std::string& branchName);
    juce::Colour colorForCommit(const std::string& sha);

    /** Draw the edges (branch lines) between nodes. */
    void drawEdges(juce::Graphics& g);

    /** Draw a single commit node. */
    void drawNode(juce::Graphics& g, const GitGraphNode& node,
                  const juce::Rectangle<int>& bounds);

    /** Draw branch/ref labels. */
    void drawLabels(juce::Graphics& g, const GitGraphNode& node,
                    const juce::Rectangle<int>& bounds);

    /** Draw the commit message. */
    void drawCommitMessage(juce::Graphics& g, const GitGraphNode& node,
                           const juce::Rectangle<int>& bounds);

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    std::vector<GitCommit> commits_;
    std::string currentBranch_;
    std::string headSha_;

    const GitCommit* selectedCommit_ = nullptr;

    int totalGraphHeight_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GitGraph)
};

} // namespace hathor::ui
