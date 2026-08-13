// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * EditorGroup.cpp — implementation of tab management extracted from EditorArea.
 *
 * Requirement references: L-1 §1, §2, §3
 */

#include "EditorGroup.hpp"

#include "../app/AudioEngine.hpp"
#include "../control/ControlInterface.hpp"
#include "HathorFileParser.hpp"
#include "EditorArea.hpp"  // for nextFreeSlot()

namespace hathor::ui {

// ===========================================================================
// EnhancedTabBar constants
// ===========================================================================

static constexpr int kMinTabWidth  = 80;
static constexpr int kMaxTabWidth  = 220;

// ===========================================================================
// EnhancedTabBar
// ===========================================================================

EnhancedTabBar::EnhancedTabBar()
{
    rebuild({}, 0, nullptr);
}

void EnhancedTabBar::rebuild(const std::vector<TabDisplayInfo>& tabs,
                              int activeIndex,
                              const TabReorderModel* reorderModel)
{
    geom_.clear();
    activeIndex_ = activeIndex;
    reorderModel_ = reorderModel;

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
        TabGeometry tg;
        tg.bounds      = { x, 0, tabW, kTabHeight };
        tg.closeBtnBounds = { x + tabW - kCloseBoxSize - 4,
                              (kTabHeight - kCloseBoxSize) / 2,
                              kCloseBoxSize, kCloseBoxSize };
        tg.pinBtnBounds = { x + 3,
                            (kTabHeight - kPinIconSize) / 2,
                            kPinIconSize, kPinIconSize };
        tg.label       = tabs[static_cast<size_t>(i)].label;
        tg.unsavedDot  = tabs[static_cast<size_t>(i)].unsavedDot;
        tg.pinned      = tabs[static_cast<size_t>(i)].pinned;
        geom_.push_back(std::move(tg));
        x += tabW;
    }

    repaint();
}

void EnhancedTabBar::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::darkgrey.darker(0.7f));

    for (size_t i = 0; i < geom_.size(); ++i)
    {
        const TabGeometry& tg = geom_[i];
        bool isActive = (static_cast<int>(i) == activeIndex_);

        // Tab background
        g.setColour(isActive
                    ? juce::Colours::darkgrey.darker(0.5f)
                    : juce::Colours::darkgrey.darker(0.7f));
        g.fillRect(tg.bounds);

        // Tab border
        g.setColour(juce::Colours::grey.darker(0.5f));
        g.drawRect(tg.bounds, 1);

        // Pin icon (if pinned)
        if (tg.pinned)
        {
            g.setColour(juce::Colours::yellow.darker(0.3f));
            juce::Path pinPath;
            pinPath.startNewSubPath(static_cast<float>(tg.pinBtnBounds.getCentreX() - 4),
                                    static_cast<float>(tg.pinBtnBounds.getCentreY() + 3));
            pinPath.lineTo(static_cast<float>(tg.pinBtnBounds.getCentreX()),
                           static_cast<float>(tg.pinBtnBounds.getCentreY() - 3));
            pinPath.lineTo(static_cast<float>(tg.pinBtnBounds.getCentreX() + 4),
                           static_cast<float>(tg.pinBtnBounds.getCentreY() + 3));
            pinPath.lineTo(static_cast<float>(tg.pinBtnBounds.getCentreX() + 2),
                           static_cast<float>(tg.pinBtnBounds.getCentreY() + 5));
            pinPath.lineTo(static_cast<float>(tg.pinBtnBounds.getCentreX() - 2),
                           static_cast<float>(tg.pinBtnBounds.getCentreY() + 5));
            pinPath.closeSubPath();
            g.fillPath(pinPath);
        }

        // Label
        g.setColour(juce::Colours::white);
        g.setFont(juce::Font(juce::FontOptions{}
            .withName(juce::Font::getDefaultSansSerifFontName())
            .withHeight(13.0f)));
        g.drawFittedText(tg.label,
                         juce::Rectangle<int>(tg.bounds.getX() + 4, tg.bounds.getY(),
                                              tg.bounds.getWidth() - 8, tg.bounds.getHeight()),
                         juce::Justification::centredLeft, 1);

        // Unsaved dot
        if (tg.unsavedDot)
        {
            g.setColour(juce::Colours::orange);
            g.fillEllipse(
                static_cast<float>(tg.bounds.getX() + tg.bounds.getWidth() - kCloseBoxSize - 3 - kUnsavedDotRadius),
                static_cast<float>(tg.bounds.getY() + (kTabHeight - kUnsavedDotRadius * 2) / 2),
                static_cast<float>(kUnsavedDotRadius * 2),
                static_cast<float>(kUnsavedDotRadius * 2));
        }

        // Close button (X)
        g.setColour(juce::Colours::lightgrey);
        g.drawLine(static_cast<float>(tg.closeBtnBounds.getX()), static_cast<float>(tg.closeBtnBounds.getY()),
                   static_cast<float>(tg.closeBtnBounds.getRight()), static_cast<float>(tg.closeBtnBounds.getBottom()), 1.5f);
        g.drawLine(static_cast<float>(tg.closeBtnBounds.getX()), static_cast<float>(tg.closeBtnBounds.getBottom()),
                   static_cast<float>(tg.closeBtnBounds.getRight()), static_cast<float>(tg.closeBtnBounds.getY()), 1.5f);
    }

    // Separator line at bottom
    g.setColour(juce::Colours::grey.darker(0.3f));
    g.drawHorizontalLine(kTabHeight - 1, 0.0f, static_cast<float>(getWidth()));
}

void EnhancedTabBar::mouseDown(const juce::MouseEvent& e)
{
    // Check for close button click
    for (size_t i = 0; i < geom_.size(); ++i)
    {
        if (geom_[i].closeBtnBounds.contains(e.position.toInt()))
        {
            if (onTabCloseClicked)
                onTabCloseClicked(static_cast<int>(i));
            return;
        }
    }

    // Check for pin button click
    for (size_t i = 0; i < geom_.size(); ++i)
    {
        if (geom_[i].pinBtnBounds.contains(e.position.toInt()))
        {
            if (onTabPinClicked)
                onTabPinClicked(static_cast<int>(i));
            return;
        }
    }

    // Check for tab click
    for (size_t i = 0; i < geom_.size(); ++i)
    {
        if (geom_[i].bounds.contains(e.position.toInt()))
        {
            if (onTabClicked)
                onTabClicked(static_cast<int>(i));
            return;
        }
    }
}

void EnhancedTabBar::mouseUp(const juce::MouseEvent& e)
{
    if (isDragging_ && e.mouseWasDraggedSinceMouseDown())
    {
        isDragging_ = false;
        draggedTabIndex_ = -1;
        if (onReorderRequested)
            onReorderRequested();
    }
}

void EnhancedTabBar::mouseDrag(const juce::MouseEvent& e)
{
    if (!isDragging_ && e.mouseWasDraggedSinceMouseDown())
    {
        for (size_t i = 0; i < geom_.size(); ++i)
        {
            if (geom_[i].bounds.contains(e.position.toFloat().roundToInt()))
            {
                isDragging_ = true;
                draggedTabIndex_ = static_cast<int>(i);
                dragStartPoint_ = e.position.toFloat().roundToInt();
                return;
            }
        }
    }
}

// ===========================================================================
// EditorGroup
// ===========================================================================

EditorGroup::EditorGroup(AudioEngine& audio,
                          hathor::control::ControlInterface& ci)
    : tabBar_(),
      statusBar_(),
      audio_(audio),
      ci_(ci)
{
    // ci_ is retained for future editor↔control wiring (see completion audit,
    // EditorGroup.hpp:222). No editor↔control calls are wired yet — suppressing
    // the unused-private-field warning locally until Phase 1 implements them.
    (void)ci_;

    addAndMakeVisible(tabBar_);
    addAndMakeVisible(statusBar_);

    statusBar_.setJustificationType(juce::Justification::left);
    statusBar_.setColour(juce::Label::textColourId, juce::Colours::white);
    statusBar_.setColour(juce::Label::backgroundColourId, juce::Colours::black.withAlpha(0.3f));

    // Wire tab bar callbacks
    tabBar_.onTabClicked = [this](int idx) { activateTab(idx); };
    tabBar_.onTabCloseClicked = [this](int idx) { closeTab(idx); };
    tabBar_.onTabPinClicked = [this](int idx) {
        reorderModel_.togglePin(static_cast<size_t>(idx));
        refreshTabBar();
    };
    tabBar_.onReorderRequested = [this]() { refreshTabBar(); };
}

EditorGroup::~EditorGroup() = default;

HathorTab* EditorGroup::openUntitledTab()
{
    int slot = nextFreeSlot(buildHathorTabPointers());
    if (slot < 0)
        return nullptr;

    auto tab = std::make_unique<HathorTab>(slot);
    HathorTab* ptr = tab.get();
    tabs_.push_back(std::move(tab));

    addAndMakeVisible(*ptr);
    ptr->setVisible(false);

    reorderModel_.resize(tabs_.size());

    // Wire unsaved-dot callback
    ptr->onUnsavedDotChanged = [this, ptr]() {
        if (ptr == activeTab())
            refreshTabBar();
    };

    // Wire LSP/Ghost if available
    if (lspClient_)
        ptr->installLspClient(lspClient_);
    if (ghostClient_)
        ptr->installGhostClient(ghostClient_);

    activateTab(static_cast<int>(tabs_.size()) - 1);

    if (onTabCountChanged)
        onTabCountChanged();

    return ptr;
}

HathorTab* EditorGroup::openFile(const juce::File& file)
{
    // Check if already open
    for (size_t i = 0; i < tabs_.size(); ++i)
    {
        if (tabs_[i]->filePath().has_value() &&
            tabs_[i]->filePath()->getFullPathName() == file.getFullPathName())
        {
            activateTab(static_cast<int>(i));
            return tabs_[i].get();
        }
    }

    int slot = nextFreeSlot(buildHathorTabPointers());
    if (slot < 0)
        return nullptr;

    auto tab = std::make_unique<HathorTab>(slot, file);
    HathorTab* ptr = tab.get();
    tabs_.push_back(std::move(tab));

    addAndMakeVisible(*ptr);
    ptr->setVisible(false);

    reorderModel_.resize(tabs_.size());

    // Wire unsaved-dot callback
    ptr->onUnsavedDotChanged = [this, ptr]() {
        if (ptr == activeTab())
            refreshTabBar();
    };

    // Wire LSP/Ghost if available
    if (lspClient_)
        ptr->installLspClient(lspClient_);
    if (ghostClient_)
        ptr->installGhostClient(ghostClient_);

    // Load file content
    juce::String content;
    {
        std::unique_ptr<juce::FileInputStream> stream(file.createInputStream());
        if (stream != nullptr && stream->openedOk())
            content = stream->readEntireStreamAsString();
    }

    if (!content.isEmpty())
    {
        if (file.getFileExtension() == ".hathor")
        {
            // Parse front matter
            auto result = hathor::ui::parseHathorFile(content.toStdString());
            if (auto* hf = std::get_if<hathor::ui::HathorFile>(&result))
            {
                ptr->document().replaceAllContent(juce::String(hf->body));
                if (hf->front.label)
                    ptr->setDisplayLabel(*hf->front.label);
            }
            else
            {
                ptr->document().replaceAllContent(content);
            }
        }
        else
        {
            ptr->document().replaceAllContent(content);
        }
    }

    activateTab(static_cast<int>(tabs_.size()) - 1);

    if (onTabCountChanged)
        onTabCountChanged();

    return ptr;
}

bool EditorGroup::closeTab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size()))
        return false;

    HathorTab* tab = tabs_[index].get();

    // Check for unsaved changes
    if (tab->hasUnsavedDot())
    {
        juce::String name = tab->tabLabel();

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
                if (result == 3 || result == 0)
                    return;  // Cancel — keep tab open

                if (result == 1)
                {
                    // Save — stub for now; real save writes to file
                }

                // Proceed with closing
                if (index < 0 || index >= static_cast<int>(this->tabs_.size()))
                    return;

                HathorTab* closureTab = this->tabs_[static_cast<size_t>(index)].get();

                TabSnapshot snap;
                snap.label = closureTab->tabLabel().toStdString();
                snap.fileName = closureTab->filePath().has_value()
                                    ? closureTab->filePath()->getFullPathName().toStdString()
                                    : "";
                snap.content = closureTab->document().getAllContent().toStdString();
                snap.cursorOffset = static_cast<size_t>(closureTab->editor().getCaretPosition());
                this->closedTabsHistory_.push(std::move(snap));

                if (this->activeIndex_ == index)
                    this->activeIndex_ = -1;

                closureTab->setVisible(false);
                this->tabs_.erase(this->tabs_.begin() + index);
                this->reorderModel_.resize(this->tabs_.size());

                if (!this->tabs_.empty())
                {
                    this->activeIndex_ = std::min(this->activeIndex_,
                                                  static_cast<int>(this->tabs_.size()) - 1);
                    this->activateTab(this->activeIndex_);
                }
                else
                {
                    this->activeIndex_ = -1;
                }

                this->refreshTabBar();

                if (this->onTabCountChanged)
                    this->onTabCountChanged();
            });

        // The close is async — return true (if Cancel, the tab remains).
        return true;
    }

    // Save snapshot for reopen
    TabSnapshot snap;
    snap.label = tab->tabLabel().toStdString();
    snap.fileName = tab->filePath().has_value() ? tab->filePath()->getFullPathName().toStdString() : "";
    snap.content = tab->document().getAllContent().toStdString();
    snap.cursorOffset = static_cast<size_t>(tab->editor().getCaretPosition());
    closedTabsHistory_.push(std::move(snap));

    // If this tab is active, deactivate first
    if (activeIndex_ == index)
        activeIndex_ = -1;

    // Hide before removing from component hierarchy
    tab->setVisible(false);

    // Erase from vector — HathorTab destructor removes itself from parent
    tabs_.erase(tabs_.begin() + index);
    reorderModel_.resize(tabs_.size());

    // Adjust active index
    if (!tabs_.empty())
    {
        activeIndex_ = std::min(activeIndex_, static_cast<int>(tabs_.size()) - 1);
        activateTab(activeIndex_);
    }
    else
    {
        activeIndex_ = -1;
    }

    refreshTabBar();

    if (onTabCountChanged)
        onTabCountChanged();

    return true;
}

void EditorGroup::reopenLastClosedTab()
{
    auto snap = closedTabsHistory_.pop();
    if (!snap.has_value())
        return;

    int slot = nextFreeSlot(buildHathorTabPointers());
    if (slot < 0)
        return;

    auto tab = std::make_unique<HathorTab>(slot);

    if (!snap->fileName.empty())
        tab->setFilePath(juce::File(snap->fileName));
    if (!snap->label.empty())
        tab->setDisplayLabel(snap->label);

    tab->document().replaceAllContent(juce::String(snap->content));

    HathorTab* ptr = tab.get();
    tabs_.push_back(std::move(tab));
    addAndMakeVisible(*ptr);
    ptr->setVisible(false);

    reorderModel_.resize(tabs_.size());

    // Wire callbacks
    ptr->onUnsavedDotChanged = [this, ptr]() {
        if (ptr == activeTab())
            refreshTabBar();
    };

    // Wire LSP/Ghost if available
    if (lspClient_)
        ptr->installLspClient(lspClient_);
    if (ghostClient_)
        ptr->installGhostClient(ghostClient_);

    // Restore cursor position
    juce::CodeDocument::Position pos(ptr->document(), static_cast<int>(snap->cursorOffset));
    ptr->editor().moveCaretTo(pos, false);

    activateTab(static_cast<int>(tabs_.size()) - 1);

    if (onTabCountChanged)
        onTabCountChanged();
}

HathorTab* EditorGroup::activeTab() noexcept
{
    if (activeIndex_ < 0 || activeIndex_ >= static_cast<int>(tabs_.size()))
        return nullptr;
    return tabs_[activeIndex_].get();
}

const HathorTab* EditorGroup::activeTab() const noexcept
{
    return const_cast<EditorGroup*>(this)->activeTab();
}

void EditorGroup::activateTab(int index)
{
    if (index < 0 || index >= static_cast<int>(tabs_.size()))
        return;

    if (activeIndex_ == index)
        return;

    // Hide old active tab
    if (activeIndex_ >= 0 && activeIndex_ < static_cast<int>(tabs_.size()))
        tabs_[activeIndex_]->setVisible(false);

    activeIndex_ = index;

    // Show new active tab
    HathorTab* tab = tabs_[activeIndex_].get();
    tab->setVisible(true);
    tab->toBack();  // bring to front within this group
    tab->editor().grabKeyboardFocus();

    refreshTabBar();

    if (onActiveTabChanged)
        onActiveTabChanged(tab);
}

void EditorGroup::setTabPinned(int /*index*/, bool /*pinned*/)
{
    // Pinning is handled directly in the tab bar callback via reorderModel_.togglePin()
}

bool EditorGroup::isTabPinned(int index) const noexcept
{
    return index >= 0 && index < static_cast<int>(tabs_.size()) &&
           reorderModel_.isPinned(static_cast<size_t>(index));
}

void EditorGroup::handleKeyPress(const juce::KeyPress& key)
{
    if (HathorTab* tab = activeTab())
    {
        if (tab->handleLspKeyPress(key))
            return;
    }
}

void EditorGroup::setEditorErgonomicsEnabled(bool /*enabled*/) noexcept
{
    // Note: JUCE 8.0.4's CodeEditorComponent does not expose setCodeFoldingEnabled,
    // setBraceMatching, or setIndentOnEnter as public methods.
    // These features can be implemented via custom logic in HathorTab if needed.
    // For L-1, the editor ergonomics focus on tab management (pinning, drag,
    // recently-closed), which is handled by EnhancedTabBar + TabReorderModel.
}

// ---------------------------------------------------------------------------
// LSP / Ghost wiring
// ---------------------------------------------------------------------------

void EditorGroup::setLspClient(class HathorLspClient* client) noexcept
{
    lspClient_ = client;
    for (auto& tab : tabs_)
        tab->installLspClient(client);
}

void EditorGroup::setGhostClient(class GhostLlmClient* client) noexcept
{
    ghostClient_ = client;
    for (auto& tab : tabs_)
        tab->installGhostClient(client);
}

void EditorGroup::ghostTick()
{
    for (auto& tab : tabs_)
        tab->ghostTick();
}

void EditorGroup::syncSlotButtonStates()
{
    for (auto& tab : tabs_)
    {
        if (!tab->isChuckTab())
            tab->setSlotRunningVisual(audio_.isSlotRunning(tab->slotIndex()));
    }
}

void EditorGroup::updateNowPlayingHighlight(
    const std::vector<hathor::Event<hathor::ParamMap>>& events)
{
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
        if (ev.sourceOffset == 0)
            continue;
        latest[ev.slotId].valid = true;
        latest[ev.slotId].sourceOffset = ev.sourceOffset;
    }

    for (int i = 0; i < AudioEngine::kNumSlots; ++i)
    {
        if (!latest[i].valid)
        {
            for (const auto& t : tabs_)
            {
                if (t->slotIndex() == i && !t->isChuckTab())
                    t->clearNowPlayingHighlight();
            }
        }
        else
        {
            for (const auto& t : tabs_)
            {
                if (t->slotIndex() == i && !t->isChuckTab())
                {
                    juce::Rectangle<int> bounds = resolveGlyphBounds(*t, latest[i].sourceOffset);
                    if (!bounds.isEmpty())
                        t->setNowPlayingHighlight(latest[i].sourceOffset, bounds);
                    else
                        t->clearNowPlayingHighlight();
                }
            }
        }
    }
}

juce::Rectangle<int> EditorGroup::resolveGlyphBounds(HathorTab& tab, std::size_t sourceOffset)
{
    const juce::CodeDocument& doc = tab.document();
    const int docLen = doc.getNumCharacters();
    const int charIdx = static_cast<int>(sourceOffset);

    if (charIdx < 0 || charIdx > docLen)
        return {};

    juce::CodeDocument::Position docPos(tab.document(), charIdx);
    juce::Rectangle<int> bounds = tab.editor().getCharacterBounds(docPos);

    if (bounds.getWidth() <= 0 || bounds.getHeight() <= 0)
        return {};

    // Expand bounds to cover the full atom token
    const int lineNum = docPos.getLineNumber();
    if (lineNum >= 0 && lineNum < doc.getNumLines())
    {
        const juce::String lineText = doc.getLine(lineNum);
        const int col = docPos.getIndexInLine();

        // Scan forward from col to find the end of the current atom.
        int endCol = col;
        while (endCol < lineText.length())
        {
            char c = lineText[endCol];
            if (c == ' ' || c == ',' || c == ';' || c == '\n' || c == '\t')
                break;
            ++endCol;
        }

        juce::CodeDocument::Position endPos(doc, lineNum, endCol);
        juce::Rectangle<int> endBounds = tab.editor().getCharacterBounds(endPos);
        if (!endBounds.isEmpty())
            bounds = bounds.getUnion(endBounds);
    }

    return bounds;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void EditorGroup::resized()
{
    juce::Rectangle<int> area(getLocalBounds());

    tabBar_.setBounds(area.removeFromTop(EnhancedTabBar::kTabHeight));
    statusBar_.setBounds(area.removeFromBottom(22));

    // Active tab fills remaining space
    if (activeIndex_ >= 0 && activeIndex_ < static_cast<int>(tabs_.size()))
    {
        tabs_[activeIndex_]->setBounds(area);
    }
    else
    {
        for (auto& t : tabs_)
            t->setVisible(false);
    }
}

void EditorGroup::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black.withAlpha(0.1f));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void EditorGroup::refreshTabBar()
{
    tabBar_.rebuild(buildTabDisplayInfos(), activeIndex_, &reorderModel_);
}

std::vector<EnhancedTabBar::TabDisplayInfo> EditorGroup::buildTabDisplayInfos() const
{
    std::vector<EnhancedTabBar::TabDisplayInfo> infos;
    for (const auto& tab : tabs_)
    {
        EnhancedTabBar::TabDisplayInfo info;
        info.label = tab->tabLabel();
        info.unsavedDot = tab->hasUnsavedDot();
        info.pinned = reorderModel_.isPinned(infos.size());
        infos.push_back(info);
    }
    return infos;
}

std::vector<HathorTab*> EditorGroup::buildHathorTabPointers() const
{
    std::vector<HathorTab*> ptrs;
    for (const auto& tab : tabs_)
        ptrs.push_back(tab.get());
    return ptrs;
}

} // namespace hathor::ui
