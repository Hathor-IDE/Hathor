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
#include "HathorFileParser.hpp"
#include "LspCompletionLogic.hpp"
#include "LspCompletionPopup.hpp"
#include "LspHoverHandler.hpp"
#include "LspDiagnosticsDisplay.hpp"

#include <optional>
#include <functional>
#include <memory>

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

     /// Store parsed front-matter metadata from a .hathor file.
     /// The editable document contains only the body, not the front matter.
     void setFrontMatter(const FrontMatter& fm);

     /// Retrieve the stored front-matter metadata for this tab.
     const std::optional<FrontMatter>& frontMatter() const noexcept { return frontMatter_; }

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
    // C1 — Editor now-playing highlight
    // -----------------------------------------------------------------------
    // Transient playback overlay: highlights the glyph box of the atom
    // currently sounding in this tab's slot.  Driven exclusively by the
    // B2 (slot, sourceOffset) data path via UITimer — never from the audio
    // thread or cursor state.  Does NOT modify the document, tokeniser,
    // AST, or static syntax highlighting.

    /// Set the now-playing highlight for this tab.
    /// Called by EditorArea from UITimer when the latest playback event
    /// targets this tab's slot.
    ///
    /// @param sourceOffset  Byte offset of the sounding atom in the document.
    /// @param glyphBounds  Pixel-rectangle of the atom's glyph box (resolved
    ///                     from sourceOffset by EditorArea).  Must be in
    ///                     editor component local coordinates.
    void setNowPlayingHighlight(std::size_t sourceOffset,
                                const juce::Rectangle<int>& glyphBounds) noexcept;

     /// Clear the now-playing highlight (slot stopped or no event).
     /// Repaints the previously highlighted region.
     void clearNowPlayingHighlight() noexcept;

     // -----------------------------------------------------------------------
     // AI-4: LSP language integration
     // -----------------------------------------------------------------------
     // Completion (Ctrl+Space), hover (debounced cursor move), diagnostics
     // (squiggly underlines + gutter markers), and signature help.
     // -----------------------------------------------------------------------

     /// Install the LSP client that provides completions, hover, and diagnostics
     /// for this tab. Called by EditorArea after creating the HathorLspClient.
     /// The client pointer is not owned by this tab.
     void installLspClient(class HathorLspClient* client) noexcept;

     /// Notify the LSP client that a document is open (didOpen).
     /// Called when the tab is activated or a file is opened.
     void notifyLspDidOpen();

     /// Notify the LSP client that the document has changed (didChange).
     /// Called from codeDocumentTextInserted / codeDocumentTextDeleted.
     void notifyLspDidChange();

     /// Notify the LSP client that the document is being closed (didClose).
     /// Called when the tab is destroyed or the file is closed.
     void notifyLspDidClose();

     /// Request completions from the LSP at the current cursor position.
     /// Shows the LspCompletionPopup.
     void requestLspCompletion();

     /// Request hover from the LSP at the given cursor position.
     /// Shows the LspHoverHandler tooltip.
     void requestLspHover(int cursorLine, int cursorCol);

      /// Request signature help from the LSP (when cursor is inside parens).
      void requestLspSignatureHelp();

      /// Handle a key press for LSP features (Ctrl+Space, Tab, Escape, Up/Down).
      /// Called by the LspKeyListener installed on the editor. Returns true
      /// if the key was consumed by LSP logic.
      bool handleLspKeyPress(const juce::KeyPress& key);

      /// Handle cursor movement for debounced hover requests.
      /// Called by EditorArea / UITimer when the cursor position changes.
      void handleCursorMove();

      /// Handle a completion selection — applies the insert text.
      void onCompletionSelected(const lsp::CompletionCandidate& candidate);

      /// Set/clear the LSP document diagnostics for this tab.
      void notifyLspDiagnostics(const std::string& uri,
                                 const std::vector<lsp::Diagnostic>& diagnostics);

      /// Return the document URI for LSP messages (file:// URI or synthetic).
      juce::String lspDocumentUri() const;

      /// Callback fired by the LspHoverHandler when it's dismissed
      /// (used to clear hover state).
      std::function<void()> onStatusMessage;

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
    // C1: Playback highlight overlay component
    // -----------------------------------------------------------------------
    // A lightweight transparency overlay child of HathorTab, painted on top
    // of editor_. Paints only the glyph-box highlight for the currently
    // sounding atom. This preserves the editor's own painting (including
    // static syntax highlighting) — the overlay is purely additive.
    // -----------------------------------------------------------------------
    class HighlightOverlay : public juce::Component
    {
    public:
        HighlightOverlay() { setInterceptsMouseClicks(false, false); }
        void paint(juce::Graphics& g) override;
        void setHighlight(const juce::Rectangle<int>& bounds) noexcept;
        void clearHighlight() noexcept;
    private:
        juce::Rectangle<int> highlightBounds_;
        bool active_ = false;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HighlightOverlay)
    };

    HighlightOverlay highlightOverlay_;

    // AI-4: Diagnostics overlay — draws squiggly underlines on top of the editor
    class DiagnosticsOverlay : public juce::Component
    {
    public:
        struct Squiggle
        {
            juce::Rectangle<int> bounds;
            juce::Colour colour;
        };

        DiagnosticsOverlay() { setInterceptsMouseClicks(false, false); }
        void paint(juce::Graphics& g) override;
        void setDiagnostics(const std::vector<Squiggle>& squiggles) noexcept;
        void clearDiagnostics() noexcept;
    private:
        std::vector<Squiggle> squiggles_;
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DiagnosticsOverlay)
    };

     DiagnosticsOverlay diagnosticsOverlay_;

     // -----------------------------------------------------------------------
     // AI-4: KeyListener for LSP hot-keys (Ctrl+Space, Tab, Escape, arrows)
     // -----------------------------------------------------------------------
     class LspKeyListener : public juce::KeyListener
     {
     public:
         LspKeyListener(HathorTab& owner) : owner_(owner) {}
         bool keyPressed(const juce::KeyPress& key,
                         juce::Component* /*source*/) override
         {
             return owner_.handleLspKeyPress(key);
         }
     private:
         HathorTab& owner_;
         JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LspKeyListener)
     };

     std::unique_ptr<LspKeyListener> lspKeyListener_;

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
    std::optional<FrontMatter> frontMatter_;  ///< parsed front-matter metadata
    bool                       unsavedDot_{ false };

    // Per-slot Play/Stop button visual state (B1).
    // This is NOT a source of truth — the engine's SlotState::running atomic
    // is. This flag is synced from the engine via setSlotRunningVisual().
    bool                      slotRunning_{ false };

    // B4-K7: Per-tab .ck eval state (compiling / running / error / idle).
    CkevalState               ckEvalState_{ CkevalState::Idle };

    // -----------------------------------------------------------------------
    // C1: Now-playing highlight state (transient, UI-only)
    // -----------------------------------------------------------------------
    // This is playback overlay state — NOT part of the document or tokeniser.
    // It is updated exclusively from the UITimer path (driven by B2's
    // (slotId, sourceOffset) data) and cleared when the slot stops playing.
    // -----------------------------------------------------------------------
    bool                      highlightActive_{ false };
    std::size_t               highlightOffset_{ 0 };   ///< source byte offset of current atom
    juce::Rectangle<int>      highlightBounds_;        ///< current glyph box (editor-local)
    juce::Rectangle<int>      highlightBoundsPrev_;    ///< previous box (for repaint)

     // Tokenisers for both file types.  Exactly one is active at a time;
     // the editor_ holds a non-owning pointer to whichever is active.
     // (juce::CodeEditorComponent does not own its tokeniser.)
     MiniNotationTokeniser       miniTokeniser_;
     ChuckTokeniser            chuckTokeniser_;
     bool                      useChuckTokeniser_{ false };

     // AI-4: LSP client (not owned — set by EditorArea)
     class HathorLspClient*    lspClient_{ nullptr };

     // AI-4: Completion popup, hover handler, and diagnostics display
     std::unique_ptr<LspCompletionPopup>  lspCompletionPopup_;
     std::unique_ptr<LspHoverHandler>     lspHoverHandler_;
     std::unique_ptr<LspDiagnosticsDisplay> lspDiagnostics_;

     // AI-4: Debounced hover timer
     int                                   hoverPendingLine_{ -1 };
     int                                   hoverPendingCol_{ -1 };
     bool                                  hoverPending_{ false };

    juce::CodeDocument          document_;
    juce::CodeEditorComponent   editor_;

    // Per-slot Play/Stop button (B1). Renders as a small icon button in the
    // editor header. Visual state reflects SlotState::running, not an
    // independent boolean.
    juce::TextButton            slotPlayButton_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorTab)
};

} // namespace hathor::ui
