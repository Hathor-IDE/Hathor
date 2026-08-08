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
    settingsTab_ = std::make_unique<SettingsComponent>(props);
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
    const bool isEnter = (key.getKeyCode() == juce::KeyPress::returnKey);

    if (!isEnter)
        return false; // Req 23.6: only these two keystrokes trigger eval

    const bool ctrlHeld = key.getModifiers().isCtrlDown();
    const bool altHeld  = key.getModifiers().isAltDown();

    // Only handle Ctrl+Enter or Ctrl+Alt+Enter.
    if (!ctrlHeld)
        return false;

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

} // namespace hathor::ui
