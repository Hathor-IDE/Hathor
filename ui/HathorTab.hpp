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
     */
    explicit HathorTab(int slotIndex);

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
    // Callback — installed by EditorArea; fired when unsavedDot changes.
    // -----------------------------------------------------------------------
    std::function<void()> onUnsavedDotChanged;

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void resized() override;

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

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    int                        slotIndex_;
    std::optional<juce::File>  filePath_;
    std::optional<std::string> displayLabel_; ///< from front-matter `label`
    bool                       unsavedDot_{ false };

    // Tokenisers for both file types.  Exactly one is active at a time;
    // the editor_ holds a non-owning pointer to whichever is active.
    // (juce::CodeEditorComponent does not own its tokeniser.)
    MiniNotationTokeniser       miniTokeniser_;
    ChuckTokeniser            chuckTokeniser_;
    bool                      useChuckTokeniser_{ false };

    juce::CodeDocument          document_;
    juce::CodeEditorComponent   editor_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorTab)
};

} // namespace hathor::ui
