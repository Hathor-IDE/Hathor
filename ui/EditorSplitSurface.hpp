// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * EditorSplitSurface.hpp — split layout container for multiple editor groups.
 *
 * Manages a tree of EditorGroups separated by resizable splitters.
 * Supports horizontal/vertical splits, preserving active editor state
 * across layout changes.
 *
 * Requirement references: L-1 §2 (split editor panes, multiple editor groups)
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

#include "EditorGroup.hpp"

namespace hathor::ui {

/**
 * SplitterTree
 *
 * Recursive tree structure for split layouts. Each node is either:
 *   - A leaf (EditorGroup)
 *   - A split (horizontal or vertical) with two children
 *
 * The tree mirrors the visual layout: a split node divides its area between
 * left/right (or top/bottom) children, with a draggable splitter bar.
 */
class SplitterTree : public juce::Component
{
public:
    enum class Orientation
    {
        Vertical,   ///< left/right split
        Horizontal  ///< top/bottom split
    };

    /** Minimum pixel size for any pane to prevent collapse. */
    static constexpr int kMinPaneSize = 80;

    /** Width of the draggable splitter bar in pixels. */
    static constexpr int kSplitterWidth = 6;

    /**
     * Create a leaf node wrapping an EditorGroup.
     */
    static std::unique_ptr<SplitterTree> makeLeaf(AudioEngine& audio,
                                                   hathor::control::ControlInterface& ci);

    /**
     * Create a split node with two children.
     */
    static std::unique_ptr<SplitterTree> makeSplit(std::unique_ptr<SplitterTree> first,
                                                    std::unique_ptr<SplitterTree> second,
                                                    Orientation orient,
                                                    float ratio = 0.5f);

    /**
     * Remove a leaf from a subtree, collapsing degenerate splits.
     *
     * @param node    The subtree root (transferred in).
     * @param target  The leaf to remove.
     * @return The (possibly collapsed) new subtree root.
     */
    static std::unique_ptr<SplitterTree> removeLeafFromTree(
        std::unique_ptr<SplitterTree> node,
        SplitterTree* target);

    /** True if this is a leaf (EditorGroup) node. */
    bool isLeaf() const noexcept { return group_ != nullptr; }

    /** True if this is a split node. */
    bool isSplit() const noexcept { return !isLeaf(); }

    /** Orientation of the split (meaningful only for split nodes). */
    Orientation orientation() const noexcept { return orientation_; }

    /** The EditorGroup if this is a leaf. */
    EditorGroup* leafGroup() noexcept { return group_.get(); }
    const EditorGroup* leafGroup() const noexcept { return group_.get(); }

    /** First child (left or top). */
    SplitterTree* firstChild() noexcept { return first_.get(); }
    /** Second child (right or bottom). */
    SplitterTree* secondChild() noexcept { return second_.get(); }

    /** Split ratio for this node (0.0 = all first, 1.0 = all second). */
    float ratio() const noexcept { return ratio_; }
    void setRatio(float r) noexcept { ratio_ = juce::jlimit(0.05f, 0.95f, r); }

    /** Set the active leaf group (propagates to children). */
    void setActiveLeaf(SplitterTree* leaf);

    /** Find the active leaf in this subtree. */
    SplitterTree* activeLeaf() noexcept;
    const SplitterTree* activeLeaf() const noexcept;

    /** Number of leaf nodes in this subtree. */
    size_t leafCount() const noexcept;

    /** Collect all leaf EditorGroups in this subtree (in order). */
    void collectLeaves(std::vector<EditorGroup*>& out) noexcept;

    /**
     * Replace a leaf node within this subtree with a split containing
     * [the old leaf, a new leaf].  Called on the root of the tree; the
     * target leaf must be a descendant.
     *
     * @param targetLeaf  The leaf to split (must be in this subtree).
     * @param orient      Orientation for the new split.
     * @param audio       Needed to construct the new sibling EditorGroup.
     * @param ci          Needed to construct the new sibling EditorGroup.
     * @return true if the target was found and replaced.
     */
    bool splitLeafInPlace(SplitterTree* targetLeaf,
                          Orientation orient,
                          AudioEngine& audio,
                          hathor::control::ControlInterface& ci);

    /**
     * Remove a leaf node from this subtree.  Called on the root; the
     * target leaf must be a descendant (and must not be the root itself).
     *
     * After removal, degenerate splits (nodes with one child) are collapsed.
     * Delegates to the static removeLeafFromTree helper.
     *
     * @return true if the leaf was found and removed.
     */
    bool removeLeaf(SplitterTree* targetLeaf);

    /**
     * Find the leaf EditorGroup at the given screen position.
     * Returns nullptr if no leaf is at that position.
     */
    EditorGroup* findLeafAt(const juce::Point<int>& screenPos) const noexcept;

    /** Recursive layout entry point (calls resized() with clamping). */
    void layoutTree();

    // juce::Component
    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

    /** Destructor must be public for unique_ptr in EditorSplitSurface. */
    ~SplitterTree() override = default;

private:
    SplitterTree() = default;

    /** Recursive helper for splitLeafInPlace.  Consumes newLeaf on success. */
    bool tryReplaceLeaf(SplitterTree* target,
                        std::unique_ptr<SplitterTree>& newLeaf,
                        Orientation orient);

    /** Recursive helper for findLeafAt. */
    EditorGroup* findLeafAtRecursive(const juce::Point<int>& screenPos) const noexcept;

    std::unique_ptr<EditorGroup> group_;

    // Split node data
    std::unique_ptr<SplitterTree> first_;
    std::unique_ptr<SplitterTree> second_;
    Orientation orientation_{ Orientation::Vertical };
    float ratio_{ 0.5f };

    // Active leaf tracking
    SplitterTree* activeLeaf_{ nullptr };

    // Splitter drag state
    bool isDragging_{ false };
    int dragStartMouse_{ 0 };
    int dragStartDim_{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SplitterTree)
};

/**
 * EditorSplitSurface
 *
 * The top-level split editor container. Owns a SplitterTree and provides
 * high-level operations like split, close, and focus-group.
 *
 * This replaces the single EditorArea content-area with a split-aware
 * container while preserving all existing tab management behavior.
 */
class EditorSplitSurface : public juce::Component
{
public:
    explicit EditorSplitSurface(AudioEngine& audio,
                                 hathor::control::ControlInterface& ci);
    ~EditorSplitSurface() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

    /** The root splitter tree (exposed for EditorArea integration). */
    SplitterTree* root() noexcept { return tree_.get(); }

    /** Active leaf group (may be null). */
    EditorGroup* activeGroup() noexcept;

    /** Split the active leaf group. */
    void splitActive(SplitterTree::Orientation orient);

    /** Close a leaf group. If it's the last one, does nothing. */
    void closeSplit(SplitterTree* leaf);

    /** Open a new untitled tab in the active group. */
    HathorTab* openUntitledTab();

    /** Open a file in the active group (or focus if already open). */
    HathorTab* openFile(const juce::File& file);

    /** Move a tab from one editor leaf to another. */
    void moveTab(EditorGroup* source,
                 int sourceIndex,
                 EditorGroup* target,
                 int targetIndex = -1);

    /** Forward LSP/Ghost wiring to all groups in the tree. */
    void setLspClient(class HathorLspClient* client) noexcept;
    void setGhostClient(class GhostLlmClient* client) noexcept;

    /** Forward tick calls to all groups. */
    void ghostTick();
    void syncSlotButtonStates();

    /** Forward now-playing highlight to all groups. */
    void updateNowPlayingHighlight(
        const std::vector<hathor::Event<hathor::ParamMap>>& events);

#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
    bool saveTelemetry(const std::string& filePath) const;
    void loadTelemetry(const std::string& filePath);
#endif

private:
    /** Install shared callbacks on a leaf group (tab drag, active, etc.). */
    void wireGroupCallbacks(EditorGroup* group);

    /** Re-install callbacks on every leaf group (after tree changes). */
    void syncGroupCallbacks();

    std::unique_ptr<SplitterTree> tree_;
    AudioEngine& audio_;
    hathor::control::ControlInterface& ci_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorSplitSurface)
};

} // namespace hathor::ui
