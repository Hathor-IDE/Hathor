// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * EditorArea.cpp — multi-tab code editor region implementation.
 *
 * Requirements: 22.1–22.3, 22.5–22.7, 24.4
 */

#include "EditorArea.hpp"
#include "HathorFileParser.hpp"
#include "AudioEngine.hpp"

#include <algorithm>

namespace hathor::ui {

// ===========================================================================
// nextFreeSlot (Req 22.6, 24.4)
// ===========================================================================

int nextFreeSlot(const std::vector<HathorTab*>& openTabs) noexcept
{
    // Collect all slot indices already in use.
    bool occupied[AudioEngine::kNumSlots] = {};
    for (const HathorTab* tab : openTabs)
    {
        const int idx = tab->slotIndex();
        if (idx >= 0 && idx < AudioEngine::kNumSlots)
            occupied[idx] = true;
    }

    // Return lowest free index.
    for (int i = 0; i < AudioEngine::kNumSlots; ++i)
        if (!occupied[i])
            return i;

    return -1; // all slots occupied
}

// ===========================================================================
// TabBarComponent
// ===========================================================================

TabBarComponent::TabBarComponent()
{
    setInterceptsMouseClicks(true, false);
}

void TabBarComponent::rebuild(const std::vector<std::unique_ptr<HathorTab>>& tabs,
                              int activeIndex)
{
    activeIndex_ = activeIndex;
    geom_.clear();

    if (tabs.empty())
    {
        repaint();
        return;
    }

    const int totalW  = getWidth();
    const int n       = static_cast<int>(tabs.size());
    const int tabW    = std::clamp(totalW / n, kMinTabWidth, kMaxTabWidth);

    int x = 0;
    for (int i = 0; i < n; ++i)
    {
        TabGeometry g;
        g.bounds      = { x, 0, tabW, kTabHeight };
        // Close button: right-aligned inside tab, vertically centred.
        g.closeBtnBounds = { x + tabW - kCloseBoxSize - 4,
                             (kTabHeight - kCloseBoxSize) / 2,
                             kCloseBoxSize, kCloseBoxSize };
        g.label       = tabs[static_cast<std::size_t>(i)]->tabLabel();
        g.unsavedDot  = tabs[static_cast<std::size_t>(i)]->hasUnsavedDot();
        geom_.push_back(std::move(g));
        x += tabW;
    }

    repaint();
}

void TabBarComponent::paint(juce::Graphics& g)
{
    // Background strip
    g.fillAll(juce::Colour(kBgColour));

    for (int i = 0; i < static_cast<int>(geom_.size()); ++i)
    {
        const TabGeometry& tg = geom_[static_cast<std::size_t>(i)];
        const bool isActive   = (i == activeIndex_);

        // Tab background
        g.setColour(juce::Colour(isActive ? kActiveBg : kInactiveBg));
        g.fillRect(tg.bounds);

        // Bottom border for inactive, top accent line for active
        if (isActive)
        {
            g.setColour(juce::Colour(0xff569cd6)); // accent blue top line
            g.fillRect(tg.bounds.getX(), tg.bounds.getY(),
                       tg.bounds.getWidth(), 2);
        }
        else
        {
            g.setColour(juce::Colour(kSepColour));
            g.fillRect(tg.bounds.getRight() - 1, tg.bounds.getY(),
                       1, tg.bounds.getHeight()); // right separator
        }

        // Unsaved dot (Req 22.5): small amber filled circle
        if (tg.unsavedDot)
        {
            const int dotX = tg.bounds.getX() + 6;
            const int dotY = tg.bounds.getCentreY() - kDotRadius / 2;
            g.setColour(juce::Colour(kDotColour));
            g.fillEllipse(static_cast<float>(dotX),
                          static_cast<float>(dotY),
                          static_cast<float>(kDotRadius),
                          static_cast<float>(kDotRadius));
        }

        // Label — leave room for dot on left and close button on right
        const int labelLeft  = tg.bounds.getX() + (tg.unsavedDot ? kDotRadius + 10 : 8);
        const int labelRight = tg.closeBtnBounds.getX() - 4;
        const juce::Rectangle<int> labelRect(labelLeft, tg.bounds.getY(),
                                             labelRight - labelLeft,
                                             tg.bounds.getHeight());
        g.setColour(juce::Colour(isActive ? kActiveText : kTextColour));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(12.0f)));
        g.drawText(tg.label, labelRect,
                   juce::Justification::centredLeft, true);

        // Close button (×)
        g.setColour(juce::Colour(kCloseColour));
        g.setFont(juce::Font(juce::FontOptions{}.withHeight(11.0f)));
        g.drawText(juce::CharPointer_UTF8("\xC3\x97"), // × U+00D7
                   tg.closeBtnBounds,
                   juce::Justification::centred, false);
    }
}

void TabBarComponent::mouseDown(const juce::MouseEvent& e)
{
    for (int i = 0; i < static_cast<int>(geom_.size()); ++i)
    {
        const TabGeometry& tg = geom_[static_cast<std::size_t>(i)];

        if (!tg.bounds.contains(e.getPosition()))
            continue;

        // Did the user click the close button?
        if (tg.closeBtnBounds.contains(e.getPosition()))
        {
            if (onTabCloseClicked)
                onTabCloseClicked(i);
        }
        else
        {
            if (onTabClicked)
                onTabClicked(i);
        }
        return;
    }
}

// ===========================================================================
// EditorArea
// ===========================================================================

// ---------------------------------------------------------------------------
// Helper: anonymous timer for clearing the status bar
// ---------------------------------------------------------------------------
namespace {

class StatusClearTimer : public juce::Timer
{
public:
    explicit StatusClearTimer(juce::Label& label) : label_(label) {}

    void timerCallback() override
    {
        label_.setText("", juce::dontSendNotification);
        stopTimer();
    }

private:
    juce::Label& label_;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

EditorArea::EditorArea(AudioEngine& audio,
                       hathor::control::ControlInterface& ci)
    : audio_(audio)
    , ci_(ci)
{
    // ci_ is used in task 3.6 (eval keybindings). Suppress the unused warning
    // until then.
    (void)ci_;
    // Status bar styling
    statusBar_.setFont(juce::Font(juce::FontOptions{}.withHeight(12.0f)));
    statusBar_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff252526));
    statusBar_.setColour(juce::Label::textColourId,       juce::Colour(0xffd4d4d4));
    statusBar_.setJustificationType(juce::Justification::centredLeft);

    // Tab bar callbacks
    tabBar_.onTabClicked = [this](int i) { activateTab(i); };
    tabBar_.onTabCloseClicked = [this](int i) { closeTab(i); };

    addAndMakeVisible(tabBar_);
    addAndMakeVisible(statusBar_);

    // Status clear timer (heap, owned via raw ptr — stopped & deleted in destructor)
    statusClearTimer_ = new StatusClearTimer(statusBar_);
}

EditorArea::~EditorArea()
{
    // Hide all tab components before deletion to avoid dangling paint calls.
    for (auto& t : tabs_)
        t->setVisible(false);

    delete statusClearTimer_;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool EditorArea::openUntitledTab()
{
    const int slot = nextFreeSlot(tabPointers());
    if (slot == -1)
    {
        // Req 22.6: all 16 slots occupied — show error, decline to open
        showStatus("Error: all 16 pattern slots are occupied. Close a tab to open a new buffer.");
        return false;
    }

    auto tab = std::make_unique<HathorTab>(slot);
    wireUnsavedCallback(*tab);
    addAndMakeVisible(*tab);
    tabs_.push_back(std::move(tab));

    activateTab(static_cast<int>(tabs_.size()) - 1);
    return true;
}

bool EditorArea::openFile(const juce::File& file)
{
    // Focus existing tab if the file is already open.
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
    {
        const auto& fp = tabs_[static_cast<std::size_t>(i)]->filePath();
        if (fp.has_value() && *fp == file)
        {
            activateTab(i);
            return true;
        }
    }

    // Parse the file to extract front-matter.
    const juce::String contents = file.loadFileAsString();
    const auto parseResult = parseHathorFile(contents.toStdString());

    std::optional<std::string> frontLabel;
    std::optional<std::string> frontSlot;

    if (const auto* hf = std::get_if<HathorFile>(&parseResult))
    {
        frontLabel = hf->front.label;
        frontSlot  = hf->front.slot;
    }

    // Determine slot index (Req 24.4)
    int slot = -1;

    if (frontSlot.has_value())
    {
        // Parse slot name, e.g. "d1" → index 3 (if AudioEngine registers it).
        // For now resolve via findOrAddSlot (may create a new registry entry).
        slot = audio_.findOrAddSlot(*frontSlot);
        if (slot == -1)
        {
            // findOrAddSlot returns -1 only if kNumSlots exhausted in the engine;
            // fall through to nextFreeSlot logic.
            slot = -1;
        }
    }

    if (slot == -1)
    {
        // No slot in front-matter (or engine rejected) → auto-assign (Req 24.4)
        slot = nextFreeSlot(tabPointers());
        if (slot == -1)
        {
            showStatus("Error: all 16 pattern slots are occupied. Close a tab to open the file.");
            return false;
        }
    }

    // Load the file text into a new tab.
    auto tab = std::make_unique<HathorTab>(slot);
    tab->setFilePath(file);
    if (frontLabel.has_value())
        tab->setDisplayLabel(*frontLabel);

    // Populate the code document with the file body / full contents.
    tab->document().replaceAllContent(contents);

    // File was just loaded — clear the unsaved dot (it would have been set
    // by replaceAllContent triggering the CodeDocument listener).
    tab->clearUnsavedDot();

    wireUnsavedCallback(*tab);
    addAndMakeVisible(*tab);
    tabs_.push_back(std::move(tab));

    activateTab(static_cast<int>(tabs_.size()) - 1);
    return true;
}

bool EditorArea::closeTab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size()))
        return false;

    HathorTab* tab = tabs_[static_cast<std::size_t>(index)].get();

    // Req 22.7: unsaved changes → Save / Discard / Cancel modal
    if (tab->hasUnsavedDot())
    {
        const juce::String name = tab->tabLabel();

        juce::AlertWindow::showAsync(
            juce::MessageBoxOptions()
                .withIconType(juce::MessageBoxIconType::QuestionIcon)
                .withTitle("Unsaved Changes")
                .withMessage("The buffer \"" + name + "\" has unsaved changes.\n"
                             "Do you want to save before closing?")
                .withButton("Save")
                .withButton("Discard")
                .withButton("Cancel"),
            [this, index](int result)
            {
                // result: 1=Save, 2=Discard, 3=Cancel (or 0 if dismissed)
                if (result == 1)
                {
                    // Save — attempt to save the file, then close.
                    HathorTab* t = tabs_[static_cast<std::size_t>(index)].get();
                    if (t->filePath().has_value())
                    {
                        t->filePath()->replaceWithText(
                            t->document().getAllContent());
                    }
                    else
                    {
                        // Save-As via native chooser
                        auto chooser = std::make_shared<juce::FileChooser>(
                            "Save Buffer As…",
                            juce::File::getSpecialLocation(
                                juce::File::userDocumentsDirectory),
                            "*.hathor");

                        chooser->launchAsync(
                            juce::FileBrowserComponent::saveMode |
                            juce::FileBrowserComponent::canSelectFiles,
                            [this, index, chooser](const juce::FileChooser& fc)
                            {
                                const auto chosen = fc.getResult();
                                if (chosen.getFullPathName().isNotEmpty())
                                {
                                    chosen.replaceWithText(
                                        tabs_[static_cast<std::size_t>(index)]
                                            ->document().getAllContent());
                                }
                                removeTabAt(index);
                            });
                        return; // async — removeTabAt called in chooser callback
                    }
                    removeTabAt(index);
                }
                else if (result == 2)
                {
                    // Discard
                    removeTabAt(index);
                }
                // result == 3 or 0 → Cancel: leave tab open (Req 22.7)
            });

        return false; // not yet closed; closure is async
    }

    // No unsaved changes — close immediately.
    removeTabAt(index);
    return true;
}

HathorTab* EditorArea::activeTab() noexcept
{
    if (activeIndex_ < 0 || activeIndex_ >= static_cast<int>(tabs_.size()))
        return nullptr;
    return tabs_[static_cast<std::size_t>(activeIndex_)].get();
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void EditorArea::resized()
{
    auto b = getLocalBounds();

    // Tab bar at the top
    tabBar_.setBounds(b.removeFromTop(kTabBarHeight));

    // Status bar at the bottom
    statusBar_.setBounds(b.removeFromBottom(kStatusBarHeight));

    // Active tab fills the middle
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
    {
        if (tabs_[static_cast<std::size_t>(i)]->isVisible())
            tabs_[static_cast<std::size_t>(i)]->setBounds(b);
    }
}

void EditorArea::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff1e1e1e));
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

std::vector<HathorTab*> EditorArea::tabPointers() const
{
    std::vector<HathorTab*> ptrs;
    ptrs.reserve(tabs_.size());
    for (const auto& t : tabs_)
        ptrs.push_back(t.get());
    return ptrs;
}

void EditorArea::activateTab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size()))
        return;

    // Hide the previously active tab (Req 22.3 — don't interrupt the slot,
    // just hide the component; AudioEngine keeps playing the old slot's pattern).
    if (activeIndex_ >= 0 && activeIndex_ < static_cast<int>(tabs_.size()))
        tabs_[static_cast<std::size_t>(activeIndex_)]->setVisible(false);

    activeIndex_ = index;
    auto* tab = tabs_[static_cast<std::size_t>(activeIndex_)].get();
    tab->setVisible(true);
    tab->setBounds(getLocalBounds()
                       .withTrimmedTop(kTabBarHeight)
                       .withTrimmedBottom(kStatusBarHeight));
    tab->editor().grabKeyboardFocus();

    refreshTabBar();
}

void EditorArea::removeTabAt(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size()))
        return;

    // Remove the component from the hierarchy before erasing.
    removeChildComponent(tabs_[static_cast<std::size_t>(index)].get());
    tabs_.erase(tabs_.begin() + index);

    // Compute new active index.
    if (tabs_.empty())
    {
        activeIndex_ = -1;
    }
    else
    {
        activeIndex_ = std::clamp(activeIndex_, 0,
                                  static_cast<int>(tabs_.size()) - 1);
        // Make sure the new active tab is visible.
        for (std::size_t i = 0; i < tabs_.size(); ++i)
            tabs_[i]->setVisible(static_cast<int>(i) == activeIndex_);
    }

    refreshTabBar();
    resized();
}

void EditorArea::showStatus(const juce::String& msg)
{
    statusBar_.setText(msg, juce::dontSendNotification);

    // Auto-clear after 6 seconds.
    static_cast<juce::Timer*>(statusClearTimer_)->startTimer(6000);
}

void EditorArea::wireUnsavedCallback(HathorTab& tab)
{
    // Capture by raw pointer (tab is owned by tabs_ and outlives this lambda
    // unless the tab is explicitly removed, in which case we also clear the
    // callback in removeTabAt before destruction).
    tab.onUnsavedDotChanged = [this]()
    {
        refreshTabBar();
    };
}

void EditorArea::refreshTabBar()
{
    tabBar_.rebuild(tabs_, activeIndex_);
}

} // namespace hathor::ui
