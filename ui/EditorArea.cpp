// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * EditorArea.cpp — multi-tab code editor region implementation.
 *
 * Requirements: 22.1–22.3, 22.5–22.7, 23.1–23.7, 24.4
 */

#include "EditorArea.hpp"
#include "HathorFileParser.hpp"
#include "AudioEngine.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <thread>

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

void TabBarComponent::rebuild(const std::vector<TabInfo>& tabs,
                              int activeIndex)
{
    geom_.clear();

    if (tabs.empty())
    {
        repaint();
        return;
    }

    activeIndex_ = activeIndex;

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
        g.label       = tabs[static_cast<std::size_t>(i)].label;
        g.unsavedDot  = tabs[static_cast<std::size_t>(i)].unsavedDot;
        geom_.push_back(std::move(g));
        x += tabW;
    }

    repaint();
}

void TabBarComponent::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Background strip
    g.fillAll(palette.background);

    for (int i = 0; i < static_cast<int>(geom_.size()); ++i)
    {
        const TabGeometry& tg = geom_[static_cast<std::size_t>(i)];
        const bool isActive   = (i == activeIndex_);

        // Tab background
        g.setColour(isActive ? palette.surface : palette.background);
        g.fillRect(tg.bounds);

        // Bottom border for inactive, top accent line for active
        if (isActive)
        {
            // accent green top line
            g.setColour(palette.accent);
            g.fillRect(tg.bounds.getX(), tg.bounds.getY(),
                       tg.bounds.getWidth(), 2);
        }
        else
        {
            g.setColour(palette.surfaceHighest);
            g.fillRect(tg.bounds.getRight() - 1, tg.bounds.getY(),
                       1, tg.bounds.getHeight()); // right separator
        }

        // Unsaved dot (Req 22.5): small amber filled circle
        if (tg.unsavedDot)
        {
            const int dotX = tg.bounds.getX() + 6;
            const int dotY = tg.bounds.getCentreY() - kDotRadius / 2;
            g.setColour(palette.warning); ///< amber unsaved dot
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
        g.setColour(isActive ? palette.textPrimary : palette.textSecondary);
        g.setFont(HathorLookAndFeel::fontMedium(12.0f));
        g.drawText(tg.label, labelRect,
                   juce::Justification::centredLeft, true);

        // Close button (×)
        g.setColour(palette.textSecondary);
        g.setFont(HathorLookAndFeel::fontMedium(11.0f));
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
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Status bar styling — label-md: 11px, Medium 500 (mockup)
    statusBar_.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::labelMd));
    statusBar_.setColour(juce::Label::backgroundColourId, palette.surfaceLow);
    statusBar_.setColour(juce::Label::textColourId,       palette.textSecondary);
    statusBar_.setJustificationType(juce::Justification::centredLeft);

    // Tab bar callbacks
    tabBar_.onTabClicked = [this](int i) { activateTab(i); };
    tabBar_.onTabCloseClicked = [this](int i) { closeTab(i); };

    addAndMakeVisible(tabBar_);
    addAndMakeVisible(statusBar_);

    // B8-K6: Create the Bake Orchestrator with a status callback.
    bakeOrchestrator_ = std::make_unique<BakeOrchestrator>(
        audio_,
        [this](const juce::String& msg) { showStatus(msg); });

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
    const int slot = nextFreeSlot(buildHathorTabPointers());
    if (slot == -1)
    {
        // Req 22.6: all 16 slots occupied — show error, decline to open
        showStatus("Error: all 16 pattern slots are occupied. Close a tab to open a new buffer.");
        return false;
    }

    auto tab = std::make_unique<HathorTab>(slot);
    wireUnsavedCallback(*tab);
    wirePlayStopCallback(*tab);
    installKeyListenerForTab(*tab);
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
        slot = nextFreeSlot(buildHathorTabPointers());
        if (slot == -1)
        {
            showStatus("Error: all 16 pattern slots are occupied. Close a tab to open the file.");
            return false;
        }
    }

    // Load the file text into a new tab.
    auto tab = std::make_unique<HathorTab>(slot, file);
    tab->setFilePath(file);
    if (frontLabel.has_value())
        tab->setDisplayLabel(*frontLabel);

    // Populate the code document with the file body / full contents.
    tab->document().replaceAllContent(contents);

    // File was just loaded — clear the unsaved dot (it would have been set
    // by replaceAllContent triggering the CodeDocument listener).
    tab->clearUnsavedDot();

    wireUnsavedCallback(*tab);
    wirePlayStopCallback(*tab);
    installKeyListenerForTab(*tab);
    addAndMakeVisible(*tab);
    tabs_.push_back(std::move(tab));

    activateTab(static_cast<int>(tabs_.size()) - 1);
    return true;
}

bool EditorArea::closeTab(int index)
{
    const int hathorTabCount = static_cast<int>(tabs_.size());

    // Handle Settings tab close (A2) — simple discard, no Save/Discard modal.
    if (index >= hathorTabCount)
    {
        if (settingsTab_ == nullptr)
            return false;

        // Discard pending edits (same semantics as close without Apply).
        settingsTab_->resetToCommitted();

        // Hide and destroy the settings tab.
        settingsTab_->setVisible(false);
        removeChildComponent(settingsTab_.get());
        settingsTab_.reset();
        settingsActive_ = false;

        // Re-activate the last HathorTab if any exist.
        if (hathorTabCount > 0)
            activateTab(hathorTabCount - 1);
        else
            activeIndex_ = -1;

        refreshTabBar();
        resized();
        return true;
    }

    if (index < 0 || index >= hathorTabCount)
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
                        // Save-As via native chooser — include both supported
                        // file type filters (.hathor and .ck).
                        auto chooser = std::make_shared<juce::FileChooser>(
                            "Save Buffer As…",
                            juce::File::getSpecialLocation(
                                juce::File::userDocumentsDirectory),
                            "*.hathor;*.ck");

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

SettingsComponent* EditorArea::openSettingsTab(juce::ApplicationProperties* props)
{
    // If already open, just focus it.
    if (settingsTab_ != nullptr)
    {
        activateTab(static_cast<int>(tabs_.size()));
        return settingsTab_.get();
    }

    // Create the Settings tab (A2).
    settingsTab_ = std::make_unique<SettingsComponent>(props, &audio_);
    addAndMakeVisible(*settingsTab_);
    settingsTab_->setVisible(false);  // will be shown by activateTab

    // Activate it (sets visibility, bounds, refreshTabBar).
    activateTab(static_cast<int>(tabs_.size()));
    return settingsTab_.get();
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

    // Settings tab (if active) also fills the same content area.
    if (settingsActive_ && settingsTab_ != nullptr)
        settingsTab_->setBounds(b);
}

void EditorArea::paint(juce::Graphics& g)
{
    g.fillAll(HathorLookAndFeel::fromComponent(*this).getPalette().surface);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

std::vector<TabInfo> EditorArea::tabInfos() const
{
    std::vector<TabInfo> infos;

    for (const auto& t : tabs_)
    {
        TabInfo info;
        info.label      = t->tabLabel();
        info.unsavedDot = t->hasUnsavedDot();
        infos.push_back(std::move(info));
    }

    // Settings tab appears after all HathorTab tabs (if open).
    if (settingsTab_ != nullptr)
    {
        TabInfo info;
        info.label      = settingsTab_->tabLabel();
        info.unsavedDot = settingsTab_->hasPendingChanges();
        infos.push_back(std::move(info));
    }

    return infos;
}

std::vector<HathorTab*> EditorArea::buildHathorTabPointers() const
{
    std::vector<HathorTab*> ptrs;
    ptrs.reserve(tabs_.size());
    for (const auto& t : tabs_)
        ptrs.push_back(t.get());
    return ptrs;
}

void EditorArea::activateTab(int index)
{
    if (index < 0)
        return;

    const int totalTabs = static_cast<int>(tabs_.size())
                        + (settingsTab_ != nullptr ? 1 : 0);
    if (index >= totalTabs)
        return;

    // Hide the previously active content.
    if (settingsActive_)
    {
        if (settingsTab_ != nullptr)
            settingsTab_->setVisible(false);
    }
    else if (activeIndex_ >= 0 && activeIndex_ < static_cast<int>(tabs_.size()))
    {
        tabs_[static_cast<std::size_t>(activeIndex_)]->setVisible(false);
    }

    // Determine if we're activating the settings tab or a HathorTab.
    const int hathorTabCount = static_cast<int>(tabs_.size());
    if (index >= hathorTabCount)
    {
        // Settings tab.
        settingsActive_ = true;
        activeIndex_    = -1;  // no HathorTab active
        if (settingsTab_ != nullptr)
        {
            settingsTab_->setVisible(true);
            settingsTab_->setBounds(getLocalBounds()
                                        .withTrimmedTop(kTabBarHeight)
                                        .withTrimmedBottom(kStatusBarHeight));
        }
    }
    else
    {
        // HathorTab.
        settingsActive_ = false;
        activeIndex_    = index;
        auto* tab = tabs_[static_cast<std::size_t>(index)].get();
        tab->setVisible(true);
        tab->setBounds(getLocalBounds()
                           .withTrimmedTop(kTabBarHeight)
                           .withTrimmedBottom(kStatusBarHeight));
        tab->editor().grabKeyboardFocus();
    }

    refreshTabBar();
}

void EditorArea::removeTabAt(int index)
{
    const int hathorTabCount = static_cast<int>(tabs_.size());
    const bool hasSettings   = (settingsTab_ != nullptr);
    const int totalTabs      = hathorTabCount + (hasSettings ? 1 : 0);

    if (index < 0 || index >= totalTabs)
        return;

    // Closing the Settings tab (A2).
    if (index >= hathorTabCount)
    {
        if (settingsTab_ != nullptr && settingsTab_->hasPendingChanges())
        {
            // Discard edits (same as Reset) — per A2: close without Apply discards.
            settingsTab_->resetToCommitted();
        }

        // Hide and destroy.
        settingsTab_->setVisible(false);
        removeChildComponent(settingsTab_.get());
        settingsTab_.reset();
        settingsActive_ = false;

        // Re-activate the last HathorTab, or nothing if none.
        if (hathorTabCount > 0)
            activateTab(hathorTabCount - 1);
        else
        {
            activeIndex_ = -1;
            settingsActive_ = false;
        }

        refreshTabBar();
        resized();
        return;
    }

    // Remove key listener from the editor before removing the tab.
    if (index < static_cast<int>(keyListeners_.size()))
    {
        auto& kl = keyListeners_[static_cast<std::size_t>(index)];
        tabs_[static_cast<std::size_t>(index)]->editor()
            .removeKeyListener(kl.get());
        keyListeners_.erase(keyListeners_.begin() + index);
    }

    // Remove the component from the hierarchy before erasing.
    removeChildComponent(tabs_[static_cast<std::size_t>(index)].get());
    tabs_.erase(tabs_.begin() + index);

    // Compute new active index.
    if (tabs_.empty() && !hasSettings)
    {
        activeIndex_ = -1;
        settingsActive_ = false;
    }
    else
    {
        activeIndex_ = std::clamp(activeIndex_, 0,
                                  static_cast<int>(tabs_.size()) - 1);
        settingsActive_ = false;
        // Make sure the new active tab is visible.
        for (std::size_t i = 0; i < tabs_.size(); ++i)
            tabs_[i]->setVisible(static_cast<int>(i) == activeIndex_);

        // If settings was active, re-show it if it still exists.
        if (settingsTab_ != nullptr)
            settingsTab_->setVisible(false);
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

void EditorArea::wirePlayStopCallback(HathorTab& tab)
{
    // The callback dispatches slot-play/slot-stop via ControlInterface on a
    // detached worker thread (same pattern as SliderPanel).  The slot name is
    // resolved from the engine via audio_.slotName(slotIndex).
    //
    // For .ck tabs (B4-K7): the play/stop button triggers ck_stop (destroy
    // the VM + clear handoff). The "play" direction is handled by Ctrl+Enter
    // (which compiles and activates the VM). Clicking stop when a shred is
    // loaded sends ck_stop to the worker.
    //
    // We capture the slot index (int, stable) rather than a pointer to the
    // tab, since tabs_ is a vector of unique_ptr and may reallocate.
    const int slotIdx = tab.slotIndex();
    const bool isChuck = tab.isChuckTab();
    hathor::control::ControlInterface& ci = ci_;

    tab.onPlayStopClicked = [this, slotIdx, isChuck, &ci]()
    {
        if (isChuck)
        {
            // B4-K7: .ck tab — dispatch ck_stop via the AudioEngine.
            // On a detached thread so the JUCE message thread isn't blocked.
            std::thread([this, slotIdx]()
            {
                audio_.stopCkTab(slotIdx);
                juce::MessageManager::callAsync([this, slotIdx]()
                {
                    // Find the tab and update its state.
                    for (const auto& t : tabs_)
                    {
                        if (t->slotIndex() == slotIdx && t->isChuckTab())
                        {
                            t->setCkEvalState(HathorTab::CkevalState::Idle);
                            showStatus("Stopped .ck tab");
                            break;
                        }
                    }
                });
            }).detach();
        }
        else
        {
            // Mini-notation path (existing B1 behavior).
            const std::string slotName =
                audio_.slotName(slotIdx).empty()
                    ? ("d" + std::to_string(slotIdx))
                    : audio_.slotName(slotIdx);

            const bool currentlyRunning = audio_.isSlotRunning(slotIdx);
            const bool start = !currentlyRunning;

            const std::string cmd =
                (start ? "slot-play " : "slot-stop ") + slotName;

            std::thread([&ci, cmd]()
            {
                ci.dispatch(cmd);
            }).detach();
        }
    };
}

void EditorArea::syncSlotButtonStates()
{
    for (const auto& t : tabs_)
    {
        // Sync mini-notation slot running visual (existing B1 behavior).
        if (!t->isChuckTab())
            t->setSlotRunningVisual(audio_.isSlotRunning(t->slotIndex()));
        // For .ck tabs, the eval state is managed by ckEval/stopCkTab
        // and the eval callback. No action needed here beyond the
        // button visual already set by setCkEvalState().
    }
}

// ---------------------------------------------------------------------------
// C1: Now-playing highlight update — called by UITimer each tick
// ---------------------------------------------------------------------------

void EditorArea::updateNowPlayingHighlight(
    const std::vector<hathor::Event<hathor::ParamMap>>& events)
{
    // -----------------------------------------------------------------------
    // Step 1: Build a set of (slotId, latest sourceOffset) from the events.
    // We track the *latest* event per slot (by slotId) because the ring buffer
    // may contain events from multiple slots in one frame, and we want the
    // most recently fired atom per slot.
    // -----------------------------------------------------------------------
    // Since we coalesce per-slot, use a small fixed-capacity map (max 16 slots).
    struct SlotLatest {
        int8_t slotId;
        std::size_t sourceOffset;
        bool valid;
    };
    SlotLatest latest[AudioEngine::kNumSlots] = {};
    for (int i = 0; i < AudioEngine::kNumSlots; ++i)
        latest[i] = { static_cast<int8_t>(i), 0, false };

    for (const auto& ev : events)
    {
        if (ev.slotId < 0 || ev.slotId >= static_cast<int8_t>(AudioEngine::kNumSlots))
            continue;

        // Only consider events with a non-zero sourceOffset (0 means no position).
        if (ev.sourceOffset == 0)
            continue;

        // Track the latest offset per slot (events may arrive out of order).
        // Since we want "now playing", pick any event from this slot's frame.
        latest[ev.slotId].valid = true;
        latest[ev.slotId].sourceOffset = ev.sourceOffset;
    }

    // -----------------------------------------------------------------------
    // Step 2: Route each slot's latest offset to the corresponding tab.
    // -----------------------------------------------------------------------
     for (int i = 0; i < AudioEngine::kNumSlots; ++i)
    {
        if (!latest[i].valid)
        {
            // No events for this slot — clear its highlight if it was active.
            for (const auto& t : tabs_)
            {
                if (t->slotIndex() == i && !t->isChuckTab())
                {
                    t->clearNowPlayingHighlight();
                    break;
                }
            }
            continue;
        }

        // Find the tab assigned to this slot.
        HathorTab* targetTab = nullptr;
        for (const auto& t : tabs_)
        {
            if (t->slotIndex() == i && !t->isChuckTab())
            {
                targetTab = t.get();
                break;
            }
        }

        if (targetTab == nullptr)
        {
            // No open tab for this slot — ignore (C1 §5.3: discard the highlight
            // while preserving normal playback).
            continue;
        }

        // Step 3: Resolve sourceOffset → glyph bounds in the editor.
        const std::size_t offset = latest[i].sourceOffset;
        juce::Rectangle<int> glyphBounds = resolveGlyphBounds(*targetTab, offset);

        if (glyphBounds.isEmpty())
        {
            // Could not resolve — skip this update (C1 §4: skip rather than
            // draw an incorrect box).
            continue;
        }

        targetTab->setNowPlayingHighlight(offset, glyphBounds);
    }
}

juce::Rectangle<int> EditorArea::resolveGlyphBounds(HathorTab& tab,
                                                     std::size_t sourceOffset)
{
    // -----------------------------------------------------------------------
    // Resolve a byte offset in the mini-notation source to the glyph bounds
    // of the atom at that position in the editor.
    //
    // The sourceOffset corresponds to the byte position in the text that was
    // evaluated (see evalOnWorkerThread / evalCkOnWorkerThread). For full-
    // buffer eval (Ctrl+Alt+Enter) this is directly the document position.
    // For eval-block eval (Ctrl+Enter) the block text is a subset of the
    // document; we track the base offset per tab to translate.
    // -----------------------------------------------------------------------
    const juce::CodeDocument& doc = tab.document();
    const int docLen = doc.getNumCharacters();
    const int charIdx = static_cast<int>(sourceOffset);

    if (charIdx < 0 || charIdx > docLen)
        return {};

    // -----------------------------------------------------------------------
    // Create a CodeDocument::Position at the source offset, then ask the
    // CodeEditorComponent for its on-screen glyph bounds.
    // -----------------------------------------------------------------------
    juce::CodeDocument::Position docPos(tab.document(), charIdx);

    // The editor component maps document positions to pixel coordinates.
    // getCharacterBounds returns the rectangle of the character at the
    // given position, in the editor's local coordinate space (which matches
    // the highlight overlay's coordinate space since they share the same
    // parent layout in HathorTab::resized()).
    juce::Rectangle<int> bounds = tab.editor().getCharacterBounds(docPos);

    // If the position is invalid or produces a degenerate rect, bail out.
    if (bounds.getWidth() <= 0 || bounds.getHeight() <= 0)
        return {};

    // Expand the bounds slightly to cover the full atom token (the character
    // bounds may be just one character; we want to highlight the whole atom).
    // The tokeniser produces tokens with known lengths — we can look at the
    // line text to find the extent of the atom starting at this position.
    const int lineNum = docPos.getLineNumber();
    if (lineNum >= 0 && lineNum < doc.getNumLines())
    {
            const juce::String lineText = doc.getLine(lineNum);
            const int col = docPos.getIndexInLine();

            // Scan forward from col to find the end of the current atom.
            // An atom is a maximal run of non-whitespace, non-special characters
            // (matching the tokeniser's TK_ATOM rule).
            int atomStart = col;
            while (atomStart < lineText.length())
            {
                const juce::juce_wchar c = lineText[atomStart];
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
                    c == '[' || c == ']' || c == '<' || c == '>' ||
                    c == '*' || c == '/' || c == '!' || c == '~' ||
                    c == '(' || c == ')' || c == ',')
                    break;
                ++atomStart;
            }

        if (atomStart > col)
        {
            // Get the bounds of the character at atomStart-1 to extend the rect.
            juce::CodeDocument::Position endPos(tab.document(),
                                                 docPos.getPosition() + (atomStart - col));
            juce::Rectangle<int> endBounds = tab.editor().getCharacterBounds(endPos);

            // Extend the original bounds to cover the full atom.
            if (!endBounds.isEmpty())
            {
                bounds.setRight(endBounds.getRight());
            }
        }
    }

    return bounds;
}

// ---------------------------------------------------------------------------
// B8-K6: Bake to Song
// ---------------------------------------------------------------------------

void EditorArea::bakeActiveTab()
{
    HathorTab* tab = activeTab();
    if (tab == nullptr)
    {
        showStatus("No active tab to bake.");
        return;
    }

    if (!tab->isChuckTab())
    {
        showStatus("Bake to Song only applies to ChucK (.ck) tabs.");
        return;
    }

    const int slotIdx = tab->slotIndex();
    const juce::String code = tab->document().getAllContent();

    juce::String filePathStr;
    if (const auto& fp = tab->filePath(); fp.has_value())
        filePathStr = fp->getFullPathName();

    bakeOrchestrator_->bakeFromTab(
        filePathStr,
        code,
        static_cast<uint8_t>(slotIdx),
        this);
}

void EditorArea::refreshTabBar()
{
    int combinedActive = -1;
    if (settingsActive_)
    {
        combinedActive = static_cast<int>(tabs_.size());
    }
    else if (activeIndex_ >= 0)
    {
        combinedActive = activeIndex_;
    }
    tabBar_.rebuild(tabInfos(), combinedActive);
}

// ===========================================================================
// Eval helpers (Req 23.1–23.7)
// ===========================================================================

// ---------------------------------------------------------------------------
// installKeyListenerForTab
// ---------------------------------------------------------------------------

void EditorArea::installKeyListenerForTab(HathorTab& tab)
{
    auto listener = std::make_unique<TabKeyListener>(*this, &tab);
    tab.editor().addKeyListener(listener.get());
    keyListeners_.push_back(std::move(listener));
}

// ---------------------------------------------------------------------------
// handleKeyPress — intercepts Ctrl+Enter and Ctrl+Alt+Enter (Req 23.1–23.6)
// ---------------------------------------------------------------------------

bool EditorArea::handleKeyPress(const juce::KeyPress& key, HathorTab* tab)
{
    // -----------------------------------------------------------------
    // B8-K6: Ctrl+Shift+B — Bake to Song
    // -----------------------------------------------------------------
    const bool isBKey = (key.getKeyCode() == 'b' || key.getKeyCode() == 'B');
    const bool ctrlHeld = key.getModifiers().isCtrlDown();
    const bool shiftHeld = key.getModifiers().isShiftDown();

    if (isBKey && ctrlHeld && shiftHeld)
    {
        bakeActiveTab();
        return true;
    }

    const bool isEnter = (key.getKeyCode() == juce::KeyPress::returnKey);

    if (!isEnter)
        return false; // Req 23.6: only these two keystrokes trigger eval

    const bool altHeld  = key.getModifiers().isAltDown();

    // Only handle Ctrl+Enter or Ctrl+Alt+Enter.
    if (!ctrlHeld)
        return false;

    // -----------------------------------------------------------------
    // B4-K7: Route .ck tabs through the ChucK compile→load→execute path.
    // Ctrl+Enter and Ctrl+Alt+Enter both evaluate the entire .ck source —
    // ChucK does not have Tidal-style "Eval_Block" semantics, so the
    // whole file is always compiled.
    // -----------------------------------------------------------------
    if (tab->isChuckTab())
    {
        const juce::String code = tab->document().getAllContent();
        evalCkOnWorkerThread(tab, code);
        return true;
    }

    // -----------------------------------------------------------------
    // Mini-notation path (existing — .hathor tabs)
    // -----------------------------------------------------------------

    // Determine slot name from the AudioEngine (e.g. "d0").
    // If the engine hasn't registered the slot yet, derive a default name.
    juce::String slotName;
    const std::string engineName = audio_.slotName(tab->slotIndex());
    if (!engineName.empty())
        slotName = juce::String(engineName);
    else
        slotName = "d" + juce::String(tab->slotIndex()); // fallback

    if (altHeld)
    {
        // Ctrl+Alt+Enter — evaluate entire buffer (Req 23.3)
        const juce::String text = tab->document().getAllContent();
        evalOnWorkerThread(tab, slotName, text);
        return true;
    }

    // Ctrl+Enter — evaluate Eval_Block (Req 23.1, 23.2)
    const int cursorLine = tab->editor().getCaretPos().getLineNumber();
    const auto block = extractEvalBlock(tab->document(), cursorLine);

    if (!block.has_value())
    {
        // Cursor is on a blank line (Req 23.2)
        showStatus("Cursor is on a blank line \xe2\x80\x94 nothing to evaluate");
        return true;
    }

    evalOnWorkerThread(tab, slotName, *block);
    return true;
}

// ---------------------------------------------------------------------------
// extractEvalBlock — maximal contiguous non-blank lines containing cursorLine
// ---------------------------------------------------------------------------

std::optional<juce::String>
EditorArea::extractEvalBlock(const juce::CodeDocument& doc,
                             int cursorLine) noexcept
{
    const int totalLines = doc.getNumLines();

    if (cursorLine < 0 || cursorLine >= totalLines)
        return std::nullopt;

    // Helper: true if a line contains at least one non-whitespace character.
    auto isNonBlank = [&](int lineNum) -> bool
    {
        if (lineNum < 0 || lineNum >= totalLines)
            return false;
        const juce::String lineText = doc.getLine(lineNum);
        for (int i = 0; i < lineText.length(); ++i)
        {
            const juce::juce_wchar c = lineText[i];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
                return true;
        }
        return false;
    };

    // Cursor line is blank → no eval block (Req 23.2)
    if (!isNonBlank(cursorLine))
        return std::nullopt;

    // Walk upward to find the block start.
    int blockStart = cursorLine;
    while (blockStart > 0 && isNonBlank(blockStart - 1))
        --blockStart;

    // Walk downward to find the block end (inclusive).
    int blockEnd = cursorLine;
    while (blockEnd < totalLines - 1 && isNonBlank(blockEnd + 1))
        ++blockEnd;

    // Collect lines verbatim, joined by newlines.
    juce::String result;
    for (int ln = blockStart; ln <= blockEnd; ++ln)
    {
        if (ln > blockStart)
            result += "\n";
        // getLine() includes the trailing newline — strip it for clean joining.
        juce::String line = doc.getLine(ln);
        while (line.endsWithChar('\n') || line.endsWithChar('\r'))
            line = line.dropLastCharacters(1);
        result += line;
    }

    return result;
}

// ---------------------------------------------------------------------------
// evalOnWorkerThread — enqueue set-pattern job with UI callback (Req 23.7)
// ---------------------------------------------------------------------------

void EditorArea::evalOnWorkerThread(HathorTab* tab,
                                    const juce::String& slotName,
                                    const juce::String& text)
{
    // Capture raw pointer to tab. The tab is owned by tabs_ and will only
    // be removed on the JUCE message thread. The lambda below always
    // re-dispatches to the message thread via callAsync, so by the time
    // clearUnsavedDot() or showStatus() runs, we can check whether the tab
    // is still in tabs_.
    HathorTab* tabPtr = tab;

    ci_.enqueueSetPattern(
        slotName.toStdString(),
        text.toStdString(),
        [this, tabPtr](nlohmann::json resp)
        {
            // Worker thread — marshal result to JUCE message thread.
            juce::MessageManager::callAsync(
                [this, tabPtr, resp = std::move(resp)]() mutable
                {
                    // Verify the tab is still open (it could have been closed
                    // while compilation was in progress).
                    bool tabStillOpen = false;
                    for (const auto& t : tabs_)
                    {
                        if (t.get() == tabPtr)
                        {
                            tabStillOpen = true;
                            break;
                        }
                    }

                    const bool ok = resp.value("ok", false);

                    if (ok)
                    {
                        // Req 23.4 — clear unsaved dot and repaint tab bar.
                        if (tabStillOpen)
                            tabPtr->clearUnsavedDot();
                        // clearUnsavedDot fires onUnsavedDotChanged → refreshTabBar
                    }
                    else
                    {
                        // Req 23.5 — show error in status bar; do not touch pattern.
                        const std::string errMsg =
                            resp.value("error", "unknown error");
                        showStatus("Error: " + juce::String(errMsg));
                    }
                });
        });
}

// ---------------------------------------------------------------------------
// B4-K7: .ck tab evaluation — compile→load→execute path
// ---------------------------------------------------------------------------

void EditorArea::evalCkOnWorkerThread(HathorTab* tab,
                                       const juce::String& code)
{
    HathorTab* tabPtr = tab;
    const int slotIdx = tab->slotIndex();

    // Set "compiling" state immediately so the user gets feedback.
    juce::MessageManager::callAsync([this, tabPtr]() {
        for (const auto& t : tabs_)
        {
            if (t.get() == tabPtr)
            {
                tabPtr->setCkEvalState(HathorTab::CkevalState::Compiling);
                break;
            }
        }
    });

    // Dispatch ckEval on a detached thread. The AudioEngineFacade::ckEval
    // method sends ck_compile via the control plane and returns synchronously
    // (bounded 5s timeout).
    std::thread([this, tabPtr, slotIdx, code = code.toStdString()]()
    {
        const bool ok = audio_.ckEval(slotIdx, code);

        // Marshal result to the JUCE message thread.
        juce::MessageManager::callAsync(
            [this, tabPtr, slotIdx, ok]() mutable
            {
                // Verify the tab is still open.
                bool tabStillOpen = false;
                for (const auto& t : tabs_)
                {
                    if (t.get() == tabPtr)
                    {
                        tabStillOpen = true;
                        break;
                    }
                }

                if (!tabStillOpen)
                    return;

                if (ok)
                {
                    tabPtr->clearUnsavedDot();
                    tabPtr->setCkEvalState(HathorTab::CkevalState::Running);
                    showStatus("\xe2\x9c\x93 compiled (Ctrl+Enter: re-eval, Play/Stop: stop)");
                }
                else
                {
                    const std::string qStatus = audio_.queryCkTab(slotIdx);
                    tabPtr->setCkEvalState(HathorTab::CkevalState::Error);
                    showStatus("ChucK compile error: " + juce::String(qStatus));
                }
            });
    }).detach();
}

} // namespace hathor::ui
