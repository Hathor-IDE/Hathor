// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * EditorSplitSurface.cpp — implementation.
 *
 * Requirement references: L-1 §2
 */

#include "EditorSplitSurface.hpp"

namespace hathor::ui {

// ===========================================================================
// SplitterTree
// ===========================================================================

std::unique_ptr<SplitterTree> SplitterTree::makeLeaf(AudioEngine& audio,
                                                      hathor::control::ControlInterface& ci)
{
    auto tree = std::unique_ptr<SplitterTree>(new SplitterTree());
    tree->group_ = std::make_unique<EditorGroup>(audio, ci);
    tree->addAndMakeVisible(tree->group_.get());
    return tree;
}

std::unique_ptr<SplitterTree> SplitterTree::makeSplit(std::unique_ptr<SplitterTree> first,
                                                       std::unique_ptr<SplitterTree> second,
                                                       Orientation orient,
                                                       float ratio)
{
    auto tree = std::unique_ptr<SplitterTree>(new SplitterTree());
    tree->orientation_ = orient;
    tree->ratio_ = juce::jlimit(0.05f, 0.95f, ratio);
    tree->first_ = std::move(first);
    tree->second_ = std::move(second);
    if (tree->first_)  tree->addAndMakeVisible(tree->first_.get());
    if (tree->second_) tree->addAndMakeVisible(tree->second_.get());
    return tree;
}

void SplitterTree::setActiveLeaf(SplitterTree* leaf)
{
    activeLeaf_ = leaf;
    if (isSplit())
    {
        if (first_) first_->setActiveLeaf(leaf);
        if (second_) second_->setActiveLeaf(leaf);
    }
}

SplitterTree* SplitterTree::activeLeaf() noexcept
{
    if (isLeaf())
        return activeLeaf_ ? activeLeaf_ : this;
    if (isSplit())
    {
        if (first_)
        {
            auto* a = first_->activeLeaf();
            if (a) return a;
        }
        if (second_)
            return second_->activeLeaf();
    }
    return nullptr;
}

const SplitterTree* SplitterTree::activeLeaf() const noexcept
{
    return const_cast<SplitterTree*>(this)->activeLeaf();
}

size_t SplitterTree::leafCount() const noexcept
{
    if (isLeaf())
        return 1;
    size_t count = 0;
    if (first_) count += first_->leafCount();
    if (second_) count += second_->leafCount();
    return count;
}

void SplitterTree::collectLeaves(std::vector<EditorGroup*>& out) noexcept
{
    if (isLeaf())
    {
        if (group_)
            out.push_back(group_.get());
    }
    else
    {
        if (first_) first_->collectLeaves(out);
        if (second_) second_->collectLeaves(out);
    }
}

// ---------------------------------------------------------------------------
// Recursive leaf splitting
// ---------------------------------------------------------------------------

bool SplitterTree::splitLeafInPlace(SplitterTree* targetLeaf,
                                     Orientation orient,
                                     AudioEngine& audio,
                                     hathor::control::ControlInterface& ci)
{
    // The special case where this IS the target leaf is handled by
    // EditorSplitSurface (which replaces the root).  Here we only handle
    // descendants.
    if (this == targetLeaf)
        return false;

    auto newLeaf = makeLeaf(audio, ci);
    // tryReplaceLeaf consumes newLeaf (moving it into the tree) only on success.
    return tryReplaceLeaf(targetLeaf, newLeaf, orient);
}

bool SplitterTree::tryReplaceLeaf(SplitterTree* target,
                                  std::unique_ptr<SplitterTree>& newLeaf,
                                  Orientation orient)
{
    if (!isSplit())
        return false;

    if (first_.get() == target)
    {
        auto oldChild = std::move(first_);
        first_ = makeSplit(std::move(oldChild), std::move(newLeaf), orient);
        if (first_)
            addAndMakeVisible(first_.get());
        return true;
    }
    if (second_.get() == target)
    {
        auto oldChild = std::move(second_);
        second_ = makeSplit(std::move(oldChild), std::move(newLeaf), orient);
        if (second_)
            addAndMakeVisible(second_.get());
        return true;
    }

    // Recurse — newLeaf is only consumed on a successful match.
    if (first_ && first_->tryReplaceLeaf(target, newLeaf, orient))
        return true;
    if (second_ && second_->tryReplaceLeaf(target, newLeaf, orient))
        return true;

    return false;
}

// ---------------------------------------------------------------------------
// Leaf removal — static recursive helper + instance wrapper
// ---------------------------------------------------------------------------

std::unique_ptr<SplitterTree> SplitterTree::removeLeafFromTree(
    std::unique_ptr<SplitterTree> node,
    SplitterTree* target)
{
    if (!node)
        return nullptr;

    // Never remove the root (protected by caller, but guard anyway).
    if (node.get() == target)
        return node;

    if (node->isLeaf())
        return node;  // not the target

    // Recurse into children.
    node->first_  = removeLeafFromTree(std::move(node->first_),  target);
    node->second_ = removeLeafFromTree(std::move(node->second_), target);

    // Re-establish JUCE parentage for surviving children.
    if (node->first_)
        node->addAndMakeVisible(node->first_.get());
    if (node->second_)
        node->addAndMakeVisible(node->second_.get());

    // Collapse degenerate splits (only one child remaining).
    if (node->first_ && !node->second_)
    {
        auto child = std::move(node->first_);
        node->first_ = nullptr;
        node->removeFromParent();
        return child;
    }
    if (!node->first_ && node->second_)
    {
        auto child = std::move(node->second_);
        node->second_ = nullptr;
        node->removeFromParent();
        return child;
    }

    // Both children gone — this split is empty.
    if (!node->first_ && !node->second_)
    {
        node->removeFromParent();
        return nullptr;
    }

    return node;
}

// ---------------------------------------------------------------------------
// Leaf removal — delegates to the static removeLeafFromTree helper
// ---------------------------------------------------------------------------

// (removeLeaf is a thin wrapper around removeLeafFromTree, kept for API
//  symmetry with splitLeafInPlace.  closeSplit calls removeLeafFromTree
//  directly so it can reassign tree_.)

// ---------------------------------------------------------------------------
// Hit-testing — find the leaf EditorGroup at a screen position
// ---------------------------------------------------------------------------

EditorGroup* SplitterTree::findLeafAt(const juce::Point<int>& screenPos) const noexcept
{
    return findLeafAtRecursive(screenPos);
}

EditorGroup* SplitterTree::findLeafAtRecursive(const juce::Point<int>& screenPos) const noexcept
{
    juce::Rectangle<int> screenBounds = getScreenBounds();
    if (!screenBounds.contains(screenPos))
        return nullptr;

    if (isLeaf())
        return group_.get();

    // Split node — check children first (they are on top in the z-order,
    // but JUCE paint order means we check both).  Children cannot overlap,
    // so only one will contain the point.
    if (first_)
    {
        auto* result = first_->findLeafAtRecursive(screenPos);
        if (result)
            return result;
    }
    if (second_)
    {
        auto* result = second_->findLeafAtRecursive(screenPos);
        if (result)
            return result;
    }

    // The point is in the splitter gap — return the first child as fallback.
    if (first_)
        return first_->findLeafAtRecursive(screenPos);
    if (second_)
        return second_->findLeafAtRecursive(screenPos);

    return nullptr;
}

// ---------------------------------------------------------------------------
// Layout — recursive with min-size clamping
// ---------------------------------------------------------------------------

void SplitterTree::layoutTree()
{
    resized();
}

void SplitterTree::resized()
{
    if (isLeaf())
    {
        if (group_)
            group_->setBounds(getLocalBounds());
        return;
    }

    auto area = getLocalBounds();
    const int splitW = kSplitterWidth;

    if (orientation_ == Orientation::Vertical)
    {
        const int totalW = area.getWidth();
        int firstSize = static_cast<int>(static_cast<float>(totalW) * ratio_);

        // Enforce minimum pane sizes.
        const int maxFirst  = juce::jmax(0, totalW - kMinPaneSize - splitW);
        firstSize = juce::jlimit(kMinPaneSize, maxFirst, firstSize);

        juce::Rectangle<int> firstArea (area.getX(), area.getY(),
                                        firstSize, area.getHeight());
        juce::Rectangle<int> secondArea (area.getX() + firstSize + splitW, area.getY(),
                                         juce::jmax(0, totalW - firstSize - splitW),
                                         area.getHeight());

        // Keep ratio_ consistent with the clamped size.
        ratio_ = static_cast<float>(firstSize) /
                 static_cast<float>(juce::jmax(1, totalW - splitW));
        ratio_ = juce::jlimit(0.05f, 0.95f, ratio_);

        if (first_)  first_->setBounds(firstArea);
        if (second_) second_->setBounds(secondArea);
    }
    else
    {
        const int totalH = area.getHeight();
        int firstSize = static_cast<int>(static_cast<float>(totalH) * ratio_);

        const int maxFirst  = juce::jmax(0, totalH - kMinPaneSize - splitW);
        firstSize = juce::jlimit(kMinPaneSize, maxFirst, firstSize);

        juce::Rectangle<int> firstArea (area.getX(), area.getY(),
                                        area.getWidth(), firstSize);
        juce::Rectangle<int> secondArea (area.getX(), area.getY() + firstSize + splitW,
                                         area.getWidth(),
                                         juce::jmax(0, totalH - firstSize - splitW));

        ratio_ = static_cast<float>(firstSize) /
                 static_cast<float>(juce::jmax(1, totalH - splitW));
        ratio_ = juce::jlimit(0.05f, 0.95f, ratio_);

        if (first_)  first_->setBounds(firstArea);
        if (second_) second_->setBounds(secondArea);
    }
}

void SplitterTree::paint(juce::Graphics& g)
{
    if (isSplit())
    {
        auto area = getLocalBounds();
        const int splitW = kSplitterWidth;
        int dim = (orientation_ == Orientation::Vertical) ? area.getWidth() : area.getHeight();
        int firstSize = static_cast<int>(static_cast<float>(dim) * ratio_);
        juce::Rectangle<int> splitterBounds;
        if (orientation_ == Orientation::Vertical)
        {
            splitterBounds = { area.getX() + firstSize, area.getY(),
                               splitW, area.getHeight() };
        }
        else
        {
            splitterBounds = { area.getX(), area.getY() + firstSize,
                               area.getWidth(), splitW };
        }
        g.setColour(juce::Colours::grey.darker(0.3f));
        g.fillRect(splitterBounds);
    }
}

// ---------------------------------------------------------------------------
// Splitter drag — with min-size enforcement
// ---------------------------------------------------------------------------

void SplitterTree::mouseDown(const juce::MouseEvent& e)
{
    if (!isSplit())
        return;

    auto area = getLocalBounds();
    const int splitW = kSplitterWidth;
    int dim = (orientation_ == Orientation::Vertical) ? area.getWidth() : area.getHeight();
    int firstSize = static_cast<int>(static_cast<float>(dim) * ratio_);
    int splitterStart = firstSize;
    int splitterEnd = firstSize + splitW;

    juce::Point<int> mousePos = e.position.toInt();
    if (orientation_ == Orientation::Vertical)
    {
        if (mousePos.x < splitterStart - 2 || mousePos.x > splitterEnd + 2)
            return;
    }
    else
    {
        if (mousePos.y < splitterStart - 2 || mousePos.y > splitterEnd + 2)
            return;
    }

    isDragging_ = true;
    dragStartMouse_ = (orientation_ == Orientation::Vertical) ? mousePos.x : mousePos.y;
    // Use the usable dimension (total minus splitter bar width).
    dragStartDim_ = dim - splitW;

    toFront(true);
}

void SplitterTree::mouseDrag(const juce::MouseEvent& e)
{
    if (!isDragging_)
        return;

    const int splitW = kSplitterWidth;
    juce::Point<int> mousePos = e.position.toInt();
    int mouse = (orientation_ == Orientation::Vertical) ? mousePos.x : mousePos.y;
    int delta = mouse - dragStartMouse_;

    int newFirstSize = static_cast<int>(static_cast<float>(dragStartDim_) * ratio_) + delta;

    // Clamp so neither pane falls below kMinPaneSize.
    int availDim = dragStartDim_ + splitW;
    int maxFirst  = juce::jmax(0, availDim - kMinPaneSize - splitW);
    newFirstSize = juce::jlimit(kMinPaneSize, maxFirst, newFirstSize);

    ratio_ = static_cast<float>(newFirstSize) /
             static_cast<float>(juce::jmax(1, availDim - splitW));
    ratio_ = juce::jlimit(0.05f, 0.95f, ratio_);

    resized();
}

// ===========================================================================
// EditorSplitSurface
// ===========================================================================

EditorSplitSurface::EditorSplitSurface(AudioEngine& audio,
                                        hathor::control::ControlInterface& ci)
    : audio_(audio),
      ci_(ci)
{
    tree_ = SplitterTree::makeLeaf(audio_, ci_);
    addAndMakeVisible(tree_.get());
    tree_->setActiveLeaf(tree_.get());
    wireGroupCallbacks(tree_->leafGroup());
}

EditorSplitSurface::~EditorSplitSurface() = default;

void EditorSplitSurface::resized()
{
    if (tree_)
    {
        tree_->setBounds(getLocalBounds());
        tree_->layoutTree();
    }
}

void EditorSplitSurface::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.05f));
}

EditorGroup* EditorSplitSurface::activeGroup() noexcept
{
    if (!tree_)
        return nullptr;
    SplitterTree* leaf = tree_->activeLeaf();
    return leaf ? leaf->leafGroup() : nullptr;
}

// ---------------------------------------------------------------------------
// Recursive split — handles any leaf in the tree
// ---------------------------------------------------------------------------

void EditorSplitSurface::splitActive(SplitterTree::Orientation orient)
{
    if (!tree_)
        return;

    SplitterTree* target = tree_->activeLeaf();
    if (!target || !target->isLeaf())
        return;

    // If the target leaf IS the root, replace the entire tree.
    if (target == tree_.get())
    {
        auto newLeaf = SplitterTree::makeLeaf(audio_, ci_);
        tree_ = SplitterTree::makeSplit(std::move(tree_),
                                        std::move(newLeaf),
                                        orient);
        addAndMakeVisible(tree_.get());
        tree_->setActiveLeaf(target);  // keep the old leaf as active
        tree_->resized();
        wireGroupCallbacks(tree_->firstChild()->leafGroup());
        wireGroupCallbacks(tree_->secondChild()->leafGroup());
    }
    else
    {
        // Multi-leaf case: replace the target leaf in its parent.
        if (tree_->splitLeafInPlace(target, orient, audio_, ci_))
        {
            tree_->setActiveLeaf(target);  // preserve active state
            tree_->resized();
            syncGroupCallbacks();
        }
    }
}

// ---------------------------------------------------------------------------
// Leaf removal
// ---------------------------------------------------------------------------

void EditorSplitSurface::closeSplit(SplitterTree* leaf)
{
    if (!leaf || !tree_ || leaf == tree_.get())
        return;  // can't close the last leaf

    auto newTree = SplitterTree::removeLeafFromTree(std::move(tree_), leaf);

    if (newTree)
    {
        tree_ = std::move(newTree);
        addAndMakeVisible(tree_.get());
        tree_->setActiveLeaf(tree_.get());
        tree_->resized();
        syncGroupCallbacks();
    }
    else
    {
        // All leaves removed — recreate a single root leaf.
        tree_ = SplitterTree::makeLeaf(audio_, ci_);
        addAndMakeVisible(tree_.get());
        tree_->setActiveLeaf(tree_.get());
        wireGroupCallbacks(tree_->leafGroup());
    }
}

// ---------------------------------------------------------------------------
// Tab operations
// ---------------------------------------------------------------------------

HathorTab* EditorSplitSurface::openUntitledTab()
{
    EditorGroup* group = activeGroup();
    if (!group)
        return nullptr;
    return group->openUntitledTab();
}

HathorTab* EditorSplitSurface::openFile(const juce::File& file)
{
    EditorGroup* group = activeGroup();
    if (!group)
        return nullptr;
    return group->openFile(file);
}

void EditorSplitSurface::moveTab(EditorGroup* source,
                                  int sourceIndex,
                                  EditorGroup* target,
                                  int targetIndex)
{
    if (!source || !target || source == target)
        return;

    if (sourceIndex < 0 || sourceIndex >= source->tabCount())
        return;

    // Detach the tab from the source group.
    std::unique_ptr<HathorTab> tab = source->takeTab(sourceIndex);
    if (!tab)
        return;

    // Insert into the target group (re-wires callbacks, activates).
    target->insertTab(std::move(tab), targetIndex);
}

// ---------------------------------------------------------------------------
// LSP / Ghost / Tick forwarding
// ---------------------------------------------------------------------------

void EditorSplitSurface::setLspClient(class HathorLspClient* client) noexcept
{
    if (!tree_)
        return;
    std::vector<EditorGroup*> groups;
    tree_->collectLeaves(groups);
    for (auto* g : groups)
        g->setLspClient(client);
}

void EditorSplitSurface::setGhostClient(class GhostLlmClient* client) noexcept
{
    if (!tree_)
        return;
    std::vector<EditorGroup*> groups;
    tree_->collectLeaves(groups);
    for (auto* g : groups)
        g->setGhostClient(client);
}

void EditorSplitSurface::ghostTick()
{
    if (!tree_)
        return;
    std::vector<EditorGroup*> groups;
    tree_->collectLeaves(groups);
    for (auto* g : groups)
        g->ghostTick();
}

void EditorSplitSurface::syncSlotButtonStates()
{
    if (!tree_)
        return;
    std::vector<EditorGroup*> groups;
    tree_->collectLeaves(groups);
    for (auto* g : groups)
        g->syncSlotButtonStates();
}

void EditorSplitSurface::updateNowPlayingHighlight(
    const std::vector<hathor::Event<hathor::ParamMap>>& events)
{
    if (!tree_)
        return;
    std::vector<EditorGroup*> groups;
    tree_->collectLeaves(groups);
    for (auto* g : groups)
        g->updateNowPlayingHighlight(events);
}

// ---------------------------------------------------------------------------
// Callback wiring
// ---------------------------------------------------------------------------

void EditorSplitSurface::wireGroupCallbacks(EditorGroup* group)
{
    if (!group)
        return;

    // Tab drag started in the group's tab bar — track for cross-pane move.
    group->onTabDragStarted = [this, group](int tabIndex)
    {
        // Determine if we should initiate a cross-pane drag by checking
        // if the mouse is still in this group's tab bar on mouse-up.
        // The actual cross-pane logic is handled in onTabDragEnded.
    };

    group->onTabDragEnded = [this, group](const juce::MouseEvent& e)
    {
        // Check if the drop target is a different leaf.
        juce::Point<int> screenPos = e.getScreenPosition().roundToInt();
        EditorGroup* target = tree_ ? tree_->findLeafAt(screenPos) : nullptr;

        if (target && target != group)
        {
            // Cross-pane drop — move the tab.
            moveTab(group, group->draggedTabIndex(), target);
        }
        else
        {
            // Same-group drop — trigger local reorder.
            group->applyLocalReorder(group->draggedTabIndex(), e);
        }
    };

    // Active tab changed — update tree's active leaf tracking.
    group->onActiveTabChanged = [this, group](HathorTab*)
    {
        if (!tree_)
            return;
        tree_->setActiveLeaf(findLeafForGroup(group));
    };
}

void EditorSplitSurface::syncGroupCallbacks()
{
    if (!tree_)
        return;
    std::vector<EditorGroup*> groups;
    tree_->collectLeaves(groups);
    for (auto* g : groups)
        wireGroupCallbacks(g);
}

#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
bool EditorSplitSurface::saveTelemetry(const std::string& filePath) const
{
    (void)filePath;
    return true;
}

void EditorSplitSurface::loadTelemetry(const std::string& filePath)
{
    (void)filePath;
}
#endif

} // namespace hathor::ui
