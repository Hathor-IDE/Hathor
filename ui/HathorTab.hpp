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

#include <nlohmann/json.hpp>

#include "MiniNotationTokeniser.hpp"
#include "ChuckTokeniser.hpp"
#include "HathorLookAndFeel.hpp"
#include "HathorFileParser.hpp"
#include "LspCompletionLogic.hpp"
#include "LspCompletionPopup.hpp"
#include "LspHoverHandler.hpp"
#include "LspDiagnosticsDisplay.hpp"
#include "GhostProtocol.hpp"
#include "GhostTextOverlay.hpp"
#include "GhostCompletionLogic.hpp"
#include "CompletionCoordinator.hpp"
#include "audio-worker/ChuckDiagnostics.hpp"

#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
#include "GhostCompletionTelemetry.hpp"
#endif

#include <chrono>
#include <optional>
#include <functional>
#include <memory>

namespace hathor::ui {

/**
 * GhostAwareEditor
 *
 * A thin CodeEditorComponent subclass that fires a callback whenever the
 * caret position changes (arrow keys, mouse clicks, programmatic moves).
 * In JUCE 8.0.4 CodeEditorComponent has no Listener interface; instead it
 * has a virtual caretPositionMoved() that must be overridden by a subclass.
 */
class GhostAwareEditor : public juce::CodeEditorComponent
{
public:
    GhostAwareEditor(juce::CodeDocument& doc, juce::CodeTokeniser* ts)
        : juce::CodeEditorComponent(doc, ts) {}

    std::function<void()> onCaretMoved;

    void caretPositionMoved() override
    {
        if (onCaretMoved)
            onCaretMoved();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GhostAwareEditor)
};

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
    // AI-G7: ChucK diagnostics callback (installed by EditorArea)
    // -----------------------------------------------------------------------
    // Fired when ChucK compiler diagnostics are produced for this tab.
    // EditorArea forwards these to the LspContextBridge for AI-8 context.
    std::function<void(const std::string& uri,
                       const std::vector<lsp::Diagnostic>&)> onChuckDiagnostics;

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

      /// AI-G7: Request deterministic ChucK completions from the versioned
      /// supported-surface metadata (no LSP server exists for ChucK).
      /// Uses the LanguageMetadata installed via installLspClient().
      void requestChuckCompletion();

      /// Request hover from the LSP at the given cursor position.
      /// Shows the LspHoverHandler tooltip.
      void requestLspHover(int cursorLine, int cursorCol);

      /// AI-G7: Request ChucK hover from the versioned metadata.
      /// Looks up the word under the cursor in LanguageMetadata::chuckApi.
      void requestChuckHover(int cursorLine, int cursorCol);

       /// Request signature help from the LSP (when cursor is inside parens).
      void requestLspSignatureHelp();

      /// Handle a key press for LSP + ghost features (Ctrl+Space, Ctrl+Shift+Space,
      /// Tab, Escape, Up/Down). Called by the LspKeyListener installed on the editor.
      /// Returns true if the key was consumed.
      bool handleLspKeyPress(const juce::KeyPress& key);

      /// Handle cursor movement for debounced hover requests.
      /// Called by EditorArea / UITimer when the cursor position changes.
      /// Also triggers ghost completion debounce cycle.
      void handleCursorMove();

      /// Handle a completion selection — applies the insert text.
      void onCompletionSelected(const lsp::CompletionCandidate& candidate);

       /// Set/clear the LSP document diagnostics for this tab.
      void notifyLspDiagnostics(const std::string& uri,
                                const std::vector<lsp::Diagnostic>& diagnostics);

      /// AI-G7: Notify ChucK compiler diagnostics for this tab.
      /// Converts audio_worker::ChuckDiagnostic to lsp::Diagnostic and
      /// displays via the diagnostics overlay (same visual path as LSP).
      void notifyChuckDiagnostics(const std::string& uri,
                                   const audio_worker::ChuckDiagnostic& diag);

      /// AI-G7: Schedule a debounced ChucK diagnostics check.
      /// Runs validateChuckSource() on a background thread and posts
      /// results back to the message thread via notifyChuckDiagnostics.
      void triggerChuckDiagnostics();

        /// Return the document URI for LSP messages (file:// URI or synthetic).
       juce::String lspDocumentUri() const;

      // -----------------------------------------------------------------------
      // AI-4: Ghost text (llm-ls inline completion)
      // -----------------------------------------------------------------------
      // Triggered on idle (debounced cursor movement on .hathor tabs).
      // Accepts Tab/Double-click, dismisses on any edit or Escape.
      // -----------------------------------------------------------------------

      /// Install the GhostLlmClient (non-owning, may be null).
      /// Called by EditorArea after creating the GhostLlmClient.
      void installGhostClient(class GhostLlmClient* client) noexcept;

      /// Trigger a ghost completion request at the current cursor position.
      /// Called on idle / debounced cursor movement.
      /// Supports both .hathor and .ck files — the languageId is determined
      /// by the active tokeniser (MiniNotationTokeniser → "hathor",
      /// ChuckTokeniser → "chuck").
      void triggerGhostCompletion();

      /// Accept the currently displayed ghost text (Tab key).
       void acceptGhostCompletion();

       // J-3: Partial acceptance — accept the next word/token of the ghost
       // text (Ctrl+→). The accepted prefix is inserted into the document as
       // a normal undoable edit; the remaining suffix stays as ghost text.
       // Does NOT issue a new LLM request. Integrates with J-2 candidate
       // cycling (acts on the currently selected candidate).
       void partialAcceptGhostCompletion();

       /// Dismiss the ghost text (Escape key or any edit).
       void dismissGhostCompletion();

      // J-2: Cycle ghost completion candidates (Alt+→ / Alt+←).
      // Cycling operates on cached candidates — no LLM request is issued.
      // Updates the overlay to show the newly selected candidate.
      /// Select the next ghost candidate (wraps). No LLM request.
      void cycleGhostNext();
      /// Select the previous ghost candidate (wraps). No LLM request.
      void cycleGhostPrev();

       /// Check for ghost text timeout / tick — called by EditorArea's
       /// UITimer.
       void ghostTick();

#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
       /// Get a human-readable per-language quality report (J-6).
       /// Returns an empty string if telemetry is disabled.
       juce::String ghostQualityReport() const noexcept;

       /// Access the telemetry object for direct event recording (J-6).
       /// Non-owning pointer — the telemetry object is owned by this tab.
       /// Returns nullptr if telemetry is not compiled in.
       lsp::GhostCompletionTelemetry* ghostTelemetry() const noexcept;
#endif

     /// Callback fired by the LspHoverHandler when it's dismissed
    /// (used to clear hover state).
    std::function<void(const juce::String&)> onStatusMessage;

     /// Callback fired when the cursor position changes (AI-8).
     /// Called from handleCursorMove() and on key events that move the caret.
     /// Installed by EditorArea to refresh the editor context snapshot.
     std::function<void()> onCursorMoved;

     // -----------------------------------------------------------------------
     // AI-8: Authoring context provider for ghost text (FIM)
     // -----------------------------------------------------------------------
     // Installed by EditorArea. Returns the dynamic authoring context JSON
     // (supported-surface from AI-3, diagnostics, editor state) to be
     // included as additional FIM context (fim.prefix) in the llm-ls request.
     // May be null if the AI-8 context provider is not wired.
     // AI-G2: This is the FIM extension point for AI-8 context injection.
     // -----------------------------------------------------------------------
     std::function<nlohmann::json()> getAuthoringContext;

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

      // AI-4: Ghost text (llm-ls inline completion)
      // -----------------------------------------------------------------------
      // GhostLlmClient is non-owning (set by EditorArea, like lspClient_).
      // CompletionCoordinator is owned by this tab (JUCE-free, testable).
      //   - Wraps GhostCompletionLogic for the ghost lifecycle.
      //   - Coordinates LSP + ghost coexistence (AI-G3): suppresses ghost
      //     when LSP popup is visible; cancels ghost on Ctrl+Space.
      // GhostTextOverlay is a child component for rendering.
      // -----------------------------------------------------------------------
        class GhostLlmClient*             ghostClient_{ nullptr };
        std::unique_ptr<CompletionCoordinator> coordinator_;
        std::unique_ptr<GhostTextOverlay>     ghostOverlay_;

#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
        /// Per-tab telemetry sink for ghost completion quality tracking (J-6).
        /// Installed on the coordinator so the JUCE-free logic layer can record
        /// display/accept/reject/stale events. The JUCE layer records the
        /// compile-result, diagnostic-added, immediate-deletion, and
        /// heavy-modification events.
        std::unique_ptr<lsp::GhostCompletionTelemetry> telemetry_;
#endif

        /// AI-G6: The active ghost result (if any) — stored so that
        // acceptGhostCompletion() can verify the cursor hasn't moved since
        // the ghost was generated. Stale ghost results (cursor moved,
        // document changed) are rejected before text is inserted.
        std::optional<lsp::GhostResult>      activeGhostResult_;

        // J-3: Flag set during partial-accept document insertion. While true,
        // codeDocumentTextInserted / caretPositionMoved skip ghost-clearing so
        // the remaining suffix survives the document edit. Cleared after the
        // overlay is re-displayed with the remaining text.
        bool partialAcceptInProgress_ = false;

       // J-6: Telemetry state for tracking accepted ghost text outcomes.
       // These are used to detect immediate deletion and heavy modification
       // of accepted ghost text — NOT stored for any other purpose, and NOT
       // the full source code (only the accepted ghost text snippet).
       std::string acceptedGhostText_;     ///< text inserted from last ghost accept
       int64_t     acceptedAtMs_{ 0 };     ///< steady-clock ms when ghost was accepted
       bool        ghostAccepted_{ false }; ///< true while accepted ghost text is being tracked

      // AI-G7: ChucK diagnostics debounce timestamp.
      int64_t chuckLastDiagTimeMs_{ 0 };

      juce::CodeDocument          document_;
      GhostAwareEditor           editor_;

    // Per-slot Play/Stop button (B1). Renders as a small icon button in the
    // editor header. Visual state reflects SlotState::running, not an
    // independent boolean.
    juce::TextButton            slotPlayButton_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorTab)
};

} // namespace hathor::ui
