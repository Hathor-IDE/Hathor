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

    /**
     * Create a leaf node wrapping an EditorGroup.
     */
    static std::unique_ptr<SplitterTree> makeLeaf(AudioEngine& audio);

    /**
     * Create a split node with two children.
     */
    static std::unique_ptr<SplitterTree> makeSplit(std::unique_ptr<SplitterTree> first,
                                                    std::unique_ptr<SplitterTree> second,
                                                    Orientation orient,
                                                    float ratio = 0.5f);

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
    void setRatio(float r) noexcept { ratio_ = juce::jlimit(0.1f, 0.9f, r); }

    /** Set the active leaf group (propagates to children). */
    void setActiveLeaf(SplitterTree* leaf);

    /** Find the active leaf in this subtree. */
    SplitterTree* activeLeaf() noexcept;
    const SplitterTree* activeLeaf() const noexcept;

    /** Number of leaf nodes in this subtree. */
    size_t leafCount() const noexcept;

    /** Collect all leaf EditorGroups in this subtree (in order). */
    void collectLeaves(std::vector<EditorGroup*>& out) noexcept;

    // juce::Component
    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

    /** Destructor must be public for unique_ptr in EditorSplitSurface. */
    ~SplitterTree() override = default;

private:
    SplitterTree() = default;
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
    explicit EditorSplitSurface(AudioEngine& audio);
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
    std::unique_ptr<SplitterTree> tree_;
    AudioEngine& audio_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(EditorSplitSurface)
};

} // namespace hathor::ui
