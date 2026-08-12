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
    tree->ratio_ = juce::jlimit(0.1f, 0.9f, ratio);
    tree->first_ = std::move(first);
    tree->second_ = std::move(second);
    if (tree->first_) tree->addAndMakeVisible(tree->first_.get());
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

void SplitterTree::resized()
{
    if (isLeaf())
    {
        if (group_)
            group_->setBounds(getLocalBounds());
        return;
    }

    auto area = getLocalBounds();
    int dim = (orientation_ == Orientation::Vertical) ? area.getWidth() : area.getHeight();
    int firstSize = static_cast<int>(dim * ratio_);
    int splitterWidth = 6;

    juce::Rectangle<int> firstArea, secondArea;
    if (orientation_ == Orientation::Vertical)
    {
        firstArea = { area.getX(), area.getY(), firstSize, area.getHeight() };
        secondArea = { area.getX() + firstSize + splitterWidth, area.getY(),
                       area.getWidth() - firstSize - splitterWidth, area.getHeight() };
    }
    else
    {
        firstArea = { area.getX(), area.getY(), area.getWidth(), firstSize };
        secondArea = { area.getX(), area.getY() + firstSize + splitterWidth,
                       area.getWidth(), area.getHeight() - firstSize - splitterWidth };
    }

    if (first_) first_->setBounds(firstArea);
    if (second_) second_->setBounds(secondArea);
}

void SplitterTree::paint(juce::Graphics& g)
{
    if (isSplit())
    {
        // Paint splitter grip area
        auto area = getLocalBounds();
        int dim = (orientation_ == Orientation::Vertical) ? area.getWidth() : area.getHeight();
        int firstSize = static_cast<int>(dim * ratio_);
        juce::Rectangle<int> splitterBounds;
        if (orientation_ == Orientation::Vertical)
        {
            splitterBounds = { area.getX() + firstSize, area.getY(),
                               6, area.getHeight() };
        }
        else
        {
            splitterBounds = { area.getX(), area.getY() + firstSize,
                               area.getWidth(), 6 };
        }
        g.setColour(juce::Colours::grey.darker(0.3f));
        g.fillRect(splitterBounds);
    }
}

void SplitterTree::mouseDown(const juce::MouseEvent& e)
{
    if (!isSplit())
        return;

    // Check if we're on the splitter bar
    auto area = getLocalBounds();
    int dim = (orientation_ == Orientation::Vertical) ? area.getWidth() : area.getHeight();
    int firstSize = static_cast<int>(dim * ratio_);
    int splitterStart = firstSize;
    int splitterEnd = firstSize + 6;

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
    dragStartDim_ = dim;
}

void SplitterTree::mouseDrag(const juce::MouseEvent& e)
{
    if (!isDragging_)
        return;

    juce::Point<int> mousePos = e.position.toInt();
    int mouse = (orientation_ == Orientation::Vertical) ? mousePos.x : mousePos.y;
    int delta = mouse - dragStartMouse_;

    int newFirstSize = static_cast<int>(dragStartDim_ * ratio_) + delta;
    float newRatio = static_cast<float>(newFirstSize) / static_cast<float>(dragStartDim_);
    ratio_ = juce::jlimit(0.1f, 0.9f, newRatio);

    resized();
}

// ===========================================================================
// EditorSplitSurface
// ===========================================================================

EditorSplitSurface::EditorSplitSurface(AudioEngine& audio,
                                        hathor::control::ControlInterface& ci)
    : tree_(nullptr),
      audio_(audio),
      ci_(ci)
{
    // Start with a single leaf group
    tree_ = SplitterTree::makeLeaf(audio_, ci_);
    addAndMakeVisible(tree_.get());
    tree_->setActiveLeaf(tree_.get());
}

EditorSplitSurface::~EditorSplitSurface() = default;

void EditorSplitSurface::resized()
{
    if (tree_)
        tree_->setBounds(getLocalBounds());
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

void EditorSplitSurface::splitActive(SplitterTree::Orientation orient)
{
    SplitterTree* active = tree_->activeLeaf();
    if (!active || !active->isLeaf())
        return;

    // Detach the active leaf's group and create a new sibling
    std::unique_ptr<SplitterTree> oldLeaf = std::move(tree_);
    tree_ = nullptr;

    // We need to find active within oldLeaf (should be oldLeaf itself if single leaf)
    // For now, simple case: if there's only one leaf, split it
    if (oldLeaf->leafCount() == 1)
    {
        auto newGroup = SplitterTree::makeLeaf(audio_, ci_);
        auto newSplit = SplitterTree::makeSplit(std::move(oldLeaf), std::move(newGroup), orient);
        tree_ = std::move(newSplit);
        addAndMakeVisible(tree_.get());
        tree_->resized();

        // Transfer callbacks to new group
        if (activeGroup())
        {
            activeGroup()->onActiveTabChanged = active->leafGroup()->onActiveTabChanged;
            activeGroup()->onTabCountChanged = active->leafGroup()->onTabCountChanged;
        }
    }
    else
    {
        // Multi-leaf case: replace the active leaf with a new split
        // This is complex — for L-1, simple single-leaf split is sufficient
        // TODO: implement multi-leaf split
        tree_ = std::move(oldLeaf);
        addAndMakeVisible(tree_.get());
    }
}

void EditorSplitSurface::closeSplit(SplitterTree* leaf)
{
    if (!leaf || !tree_)
        return;

    if (leaf == tree_.get())
        return;  // can't close the last one

    // For L-1, we don't implement removing splits from the tree.
    // This would require tree manipulation to merge siblings.
    // Placeholder for future enhancement.
}

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
