// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * HathorTab.hpp — one code-editor tab in the Hathor multi-tab editor.
 *
 * Each HathorTab owns a juce::CodeDocument (the buffer), a
 * juce::CodeEditorComponent displaying that buffer, the assigned pattern
 * slot index, the optional file path, and the unsaved-dot flag.
 *
 * Requirements: 22.1, 22.2, 22.5, 22.7
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "MiniNotationTokeniser.hpp"
#include "ChuckTokeniser.hpp"
#include "HathorLookAndFeel.hpp"

#include <optional>
#include <functional>

namespace hathor::ui {

/**
 * HathorTab
 *
 * A juce::Component that wraps:
 *  - juce::CodeDocument         — the text buffer
 *  - juce::CodeEditorComponent  — the visible editor widget
 *  - int slotIndex              — the assigned AudioEngine pattern slot [0,15]
 *  - std::optional<juce::File> filePath — nullopt for untitled buffers
 *  - bool unsavedDot            — set on any edit, cleared on successful eval
 *
 * Tab display label:
 *   - Front-matter `label` field if present, else filename stem,
 *     else "untitled-<slot>".
 *
 * The unsavedDot flag is set automatically when the CodeDocument changes
 * (via juce::CodeDocument::Listener) and cleared by clearUnsavedDot().
 *
 * Requirements: 22.1, 22.2, 22.5, 22.7
 */
class HathorTab : public juce::Component,
                  private juce::CodeDocument::Listener
{
public:
    /**
      * Construct an untitled tab on the given slot.
      *
      * @param slotIndex  Pattern slot index in [0, AudioEngine::kNumSlots).
      * @param file       Optional file — if provided, the correct tokeniser
      *                   (mini-notation or ChuckTokeniser) is chosen at construction.
      */
    HathorTab(int slotIndex, const juce::File& file = juce::File());

    ~HathorTab() override;

    // Non-copyable / non-movable (owns a document + component).
    // Move is also deleted since juce::CodeDocument/CodeEditorComponent
    // are not movable. JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR at the
    // bottom of the class covers copy — delete move explicitly here.
    HathorTab(HathorTab&&)                 = delete;
    HathorTab& operator=(HathorTab&&)      = delete;

    // -----------------------------------------------------------------------
    // Accessors
    // -----------------------------------------------------------------------

    /// The pattern slot assigned to this tab.
    int slotIndex() const noexcept { return slotIndex_; }

    /// The file path, or nullopt for untitled buffers.
    const std::optional<juce::File>& filePath() const noexcept { return filePath_; }

    /// True when the buffer has been modified since the last successful eval.
    bool hasUnsavedDot() const noexcept { return unsavedDot_; }

    /// The underlying text buffer.
    juce::CodeDocument& document() noexcept { return document_; }
    const juce::CodeDocument& document() const noexcept { return document_; }

    /// The editor widget (for focus / cursor queries).
    juce::CodeEditorComponent& editor() noexcept { return editor_; }

    // -----------------------------------------------------------------------
    // Mutations
    // -----------------------------------------------------------------------

    /// Set the file path (called after Save-As or when opening a file).
    void setFilePath(const juce::File& f);

    /// Override the display label (usually taken from front-matter).
    void setDisplayLabel(const std::string& label);

    /// Return the effective tab label:
    ///   1. displayLabel_ if set
    ///   2. filename stem if filePath_ is set
    ///   3. "untitled-<slotIndex>"
    juce::String tabLabel() const;

    /// Clear the unsaved-dot flag (called after a successful `set-pattern` eval).
    void clearUnsavedDot();

    /// Switch the syntax tokeniser based on file extension.
    /// Called by setFilePath() and EditorArea when the file type is known.
    /// Defaults to MiniNotationTokeniser for .hathor, ChuckTokeniser for .ck.
    void setFileTypeFromPath(const juce::File& file);

    /// Return true if this tab is currently using the ChucK tokeniser.
    bool isChuckTab() const noexcept { return useChuckTokeniser_; }

    // -----------------------------------------------------------------------
    // B4-K7: Per-tab .ck eval state
    // -----------------------------------------------------------------------

    /// Eval state for .ck tabs (B4-K7).
    /// Tracks the compile→load→execute lifecycle of the current shred.
    enum class CkevalState : uint8_t {
        Idle,       ///< No shred loaded (never evaluated, or stopped)
        Compiling,  ///< Compile in progress (dispatching ck_compile)
        Running,    ///< Shred loaded and VM active
        Error,      ///< Last compile failed
    };

    /// Current .ck eval state.
    CkevalState ckEvalState() const noexcept { return ckEvalState_; }

    /// Set the .ck eval state and update the Play/Stop button visual.
    void setCkEvalState(CkevalState s) noexcept;

    // -----------------------------------------------------------------------
    // Callback — installed by EditorArea; fired when unsavedDot changes.
    // -----------------------------------------------------------------------
    std::function<void()> onUnsavedDotChanged;

    // -----------------------------------------------------------------------
    // Per-slot Play/Stop (B1)
    // -----------------------------------------------------------------------

    /// Return true if this tab's slot is currently armed/running.
    /// Called by EditorArea/UITimer to sync the button visual state.
    bool isSlotRunning() const noexcept { return slotRunning_; }

    /// Set the button visual state to reflect armed/running (Play vs Stop icon).
    /// Called by EditorArea/UITimer — NOT a source of truth; reflects the engine.
    void setSlotRunningVisual(bool running) noexcept;

    /// Callback installed by EditorArea — fired when the button is clicked.
    /// The callback receives the slot index and should dispatch
    /// slot-play/slot-stop via ControlInterface.
    std::function<void()> onPlayStopClicked;

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void resized() override;

    /// Re-apply palette-derived editor colours + syntax scheme on theme switch.
    /// JUCE's CodeEditorComponent::lookAndFeelChanged() does not refresh the
    /// syntax colour scheme, so the active tokeniser's scheme is rebuilt from
    /// the current palette here too (B1, B3).
    void lookAndFeelChanged() override;

private:
    // -----------------------------------------------------------------------
    // juce::CodeDocument::Listener
    // -----------------------------------------------------------------------
    void codeDocumentTextInserted(const juce::String& newText,
                                  int insertIndex) override;
    void codeDocumentTextDeleted(int startIndex,
                                 int endIndex) override;

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------
    void markUnsaved();

    /// Paint the Play/Stop button icon onto the editor header area.
    void paintSlotPlayButton(juce::Graphics& g);

    /// Handle a click on the Play/Stop button.
    void slotPlayButtonClicked();

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    int                        slotIndex_;
    std::optional<juce::File>  filePath_;
    std::optional<std::string> displayLabel_; ///< from front-matter `label`
    bool                       unsavedDot_{ false };

    // Per-slot Play/Stop button visual state (B1).
    // This is NOT a source of truth — the engine's SlotState::running atomic
    // is. This flag is synced from the engine via setSlotRunningVisual().
    bool                      slotRunning_{ false };

    // B4-K7: Per-tab .ck eval state (compiling / running / error / idle).
    CkevalState               ckEvalState_{ CkevalState::Idle };

    // Tokenisers for both file types.  Exactly one is active at a time;
    // the editor_ holds a non-owning pointer to whichever is active.
    // (juce::CodeEditorComponent does not own its tokeniser.)
    MiniNotationTokeniser       miniTokeniser_;
    ChuckTokeniser            chuckTokeniser_;
    bool                      useChuckTokeniser_{ false };

    juce::CodeDocument          document_;
    juce::CodeEditorComponent   editor_;

    // Per-slot Play/Stop button (B1). Renders as a small icon button in the
    // editor header. Visual state reflects SlotState::running, not an
    // independent boolean.
    juce::TextButton            slotPlayButton_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorTab)
};

} // namespace hathor::ui
