// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * HathorTab.cpp — implementation of HathorTab.
 *
 * Requirements: 22.1, 22.2, 22.5, 22.7
 */

#include "HathorTab.hpp"
#include "HathorLookAndFeel.hpp"
#include "HathorLspClient.hpp"
#include "GhostLlmClient.hpp"
#include "GhostJsonRpc.hpp"

#include <chrono>
#include <cctype>
#include <unordered_map>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Constructor / destructor
// ---------------------------------------------------------------------------

HathorTab::HathorTab(int slotIndex, const juce::File& file)
    : slotIndex_(slotIndex)
    , useChuckTokeniser_(ChuckTokeniser::isChuckFile(file))
    , editor_(document_, useChuckTokeniser_
                                  ? static_cast<juce::CodeTokeniser*>(&chuckTokeniser_)
                                  : static_cast<juce::CodeTokeniser*>(&miniTokeniser_))
{
    // -----------------------------------------------------------------------
    // Editor font: JetBrains Mono, 13 pt (code-default from mockup, Req 22.1)
    // -----------------------------------------------------------------------
    editor_.setFont(HathorLookAndFeel::fontRegular(
        HathorLookAndFeel::Typography::codeDefault));

    // -----------------------------------------------------------------------
    // Dark background colours (Req 22.1, 20.2) — sourced from design tokens.
    // These local overrides ensure correctness even if this component is
    // used outside of MainWindow's look-and-feel scope.
    // -----------------------------------------------------------------------
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    editor_.setColour(juce::CodeEditorComponent::backgroundColourId,
                      palette.surface);
    editor_.setColour(juce::CodeEditorComponent::defaultTextColourId,
                      palette.codeText);
    editor_.setColour(juce::CodeEditorComponent::highlightColourId,
                      palette.accent.withAlpha(0.3f));
    editor_.setColour(juce::CodeEditorComponent::lineNumberBackgroundId,
                      palette.surfaceLow);
    editor_.setColour(juce::CodeEditorComponent::lineNumberTextId,
                      palette.codeLineNum);

    // -----------------------------------------------------------------------
    // Register as a CodeDocument listener so we can detect edits (Req 22.5)
    // -----------------------------------------------------------------------
     document_.addListener(this);

     // AI-G6: Wire caret-moved callback so ghost text is invalidated
     // when the cursor moves (arrow keys, mouse clicks, etc.) without
     // the document changing — a core AI-G6 requirement.
     editor_.onCaretMoved = [this]() { handleCursorMove(); };

     addAndMakeVisible(highlightOverlay_);
     highlightOverlay_.setInterceptsMouseClicks(false, false);

     addAndMakeVisible(editor_);

     // AI-4: LSP language integration components
     if (!useChuckTokeniser_)
     {
          lspCompletionPopup_ = std::make_unique<LspCompletionPopup>(
               [this](const lsp::CompletionCandidate& c) { onCompletionSelected(c); },
               [this]() { if (coordinator_) coordinator_->onLspPopupDismissed(); });

         lspHoverHandler_ = std::make_unique<LspHoverHandler>(
             [this]() { hoverPending_ = false; });

         lspDiagnostics_ = std::make_unique<LspDiagnosticsDisplay>();

         addAndMakeVisible(lspCompletionPopup_.get());
         addAndMakeVisible(lspHoverHandler_.get());
         addAndMakeVisible(diagnosticsOverlay_);

          lspCompletionPopup_->setVisible(false);
          lspHoverHandler_->setVisible(false);
          diagnosticsOverlay_.setVisible(true);

          // Install LSP key listener on the editor (after TabKeyListener
          // is installed by EditorArea, so it handles keys first)
           lspKeyListener_ = std::make_unique<LspKeyListener>(*this);
           editor_.addKeyListener(lspKeyListener_.get());
      }

      // J-3: Install the LSP key listener for ALL file types (including .ck).
      // For .hathor files it handles Ctrl+Space / Tab / Escape / Alt+→ / Ctrl+→.
      // For .ck files, only Tab (ghost accept), Escape (ghost dismiss),
      // Alt+→/← (cycle), and Ctrl+→ (partial accept) are relevant — the LSP
      // completions are handled separately for .ck.
      if (useChuckTokeniser_ && !lspKeyListener_)
      {
          lspKeyListener_ = std::make_unique<LspKeyListener>(*this);
          editor_.addKeyListener(lspKeyListener_.get());
      }

      // AI-4: Ghost text overlay (llm-ls inline completion)
      // Supports both .hathor and .ck files — the languageId is set
      // from the active tokeniser in triggerGhostCompletion().
      coordinator_ = std::make_unique<CompletionCoordinator>();
      coordinator_->setGhostEnabled(lsp::GhostProviderResolver::isEnabled());
      coordinator_->setGhostDebounceMs(300);
      coordinator_->setGhostTimeoutMs(5000);

      ghostOverlay_ = std::make_unique<GhostTextOverlay>();
      ghostOverlay_->setInterceptsMouseClicks(false, false);
      addAndMakeVisible(*ghostOverlay_);
      ghostOverlay_->setVisible(false);

    // -----------------------------------------------------------------------
    // Per-slot Play/Stop button (B1)
    // -----------------------------------------------------------------------
    // Small icon-only button that toggles the tab's slot play/stop state.
    // It dispatches "slot-play <slot>" / "slot-stop <slot>" via the
    // onPlayStopClicked callback installed by EditorArea.
    slotPlayButton_.setButtonText("");
    slotPlayButton_.setTooltip("Play/Stop this tab's slot");
    slotPlayButton_.setLookAndFeel(&HathorLookAndFeel::fromComponent(*this));
    // Style as a flat icon button — no background fill, just the icon.
    slotPlayButton_.setColour(juce::TextButton::buttonColourId,
                              juce::Colours::transparentBlack);
    slotPlayButton_.onClick = [this]() { slotPlayButtonClicked(); };
    addAndMakeVisible(slotPlayButton_);

    // Initialize the button visual state to stopped (Play icon).
    // The authoritative state is the engine's SlotState::running atomic,
    // which UITimer syncs at 60 Hz via syncSlotButtonStates().
    setSlotRunningVisual(false);
}

HathorTab::~HathorTab()
{
    document_.removeListener(this);
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

void HathorTab::setFilePath(const juce::File& f)
{
    filePath_ = f;
    setFileTypeFromPath(f);
}

void HathorTab::setFileTypeFromPath(const juce::File& file)
{
    const bool isChuck = ChuckTokeniser::isChuckFile(file);

    if (isChuck != useChuckTokeniser_)
    {
        useChuckTokeniser_ = isChuck;
        // Note: CodeEditorComponent does not allow swapping the tokeniser
        // after construction in JUCE 8. The constructor picks the tokeniser
        // based on file type at tab-creation time. Here we only update the
        // colour scheme to match the active tokeniser's scheme.
        if (isChuck)
            editor_.setColourScheme(chuckTokeniser_.getDefaultColourScheme());
        else
            editor_.setColourScheme(miniTokeniser_.getDefaultColourScheme());
    }
}

void HathorTab::setDisplayLabel(const std::string& label)
{
    if (label.empty())
        displayLabel_.reset();
    else
        displayLabel_ = label;
}

void HathorTab::setFrontMatter(const FrontMatter& fm)
{
    frontMatter_ = fm;
    // Sync display label from front matter if present.
    if (fm.label && !fm.label->empty())
        displayLabel_ = *fm.label;
    else
        displayLabel_.reset();
}

juce::String HathorTab::tabLabel() const
{
    // 1. Front-matter label (Req 22.2)
    if (displayLabel_.has_value() && !displayLabel_->empty())
        return juce::String(*displayLabel_);

    // 2. Filename stem (Req 22.2)
    if (filePath_.has_value())
        return filePath_->getFileNameWithoutExtension();

    // 3. Generic untitled name
    return "untitled-" + juce::String(slotIndex_);
}

void HathorTab::clearUnsavedDot()
{
    if (unsavedDot_)
    {
        unsavedDot_ = false;
        if (onUnsavedDotChanged)
            onUnsavedDotChanged();
    }
}

// ---------------------------------------------------------------------------
// Per-slot Play/Stop (B1)
// ---------------------------------------------------------------------------

void HathorTab::setSlotRunningVisual(bool running) noexcept
{
    if (slotRunning_ == running)
        return;

    slotRunning_ = running;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    if (running)
    {
        // Stop icon (two squares).
        slotPlayButton_.setButtonText(juce::String::charToString(0x25A0) + " " +
                                      juce::String::charToString(0x25A0));
        slotPlayButton_.setColour(juce::TextButton::buttonColourId,
                                  juce::Colours::transparentBlack);
        slotPlayButton_.setColour(juce::TextButton::textColourOnId,
                                  palette.error);
        slotPlayButton_.setColour(juce::TextButton::textColourOffId,
                                  palette.error);
    }
    else
    {
        // Play icon (triangle).
        slotPlayButton_.setButtonText(juce::String::charToString(0x25B6));
        slotPlayButton_.setColour(juce::TextButton::buttonColourId,
                                  juce::Colours::transparentBlack);
        slotPlayButton_.setColour(juce::TextButton::textColourOnId,
                                  palette.textPrimary);
        slotPlayButton_.setColour(juce::TextButton::textColourOffId,
                                  palette.textPrimary);
    }

    slotPlayButton_.repaint();
}

void HathorTab::slotPlayButtonClicked()
{
    if (onPlayStopClicked)
        onPlayStopClicked();
}

// ---------------------------------------------------------------------------
// B4-K7: Per-tab .ck eval state
// ---------------------------------------------------------------------------

void HathorTab::setCkEvalState(CkevalState s) noexcept
{
    if (ckEvalState_ == s)
        return;

    ckEvalState_ = s;

    // For .ck tabs, sync the Play/Stop button visual to the eval state.
    // Running = Stop icon (red), Compiling = amber, Error = red (X or stop),
    // Idle = Play icon (default).
    if (useChuckTokeniser_)
    {
        setSlotRunningVisual(s == CkevalState::Running || s == CkevalState::Compiling);
    }

    // Repaint to update the button icon.
    slotPlayButton_.repaint();
}

// ---------------------------------------------------------------------------
// C1: Playback highlight overlay — HighlightOverlay implementation
// ---------------------------------------------------------------------------

void HathorTab::HighlightOverlay::paint(juce::Graphics& g)
{
    if (!active_ || highlightBounds_.isEmpty())
        return;

    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Draw a semi-transparent accent overlay with a thin outline.
    // Uses palette.accent with high alpha so it reads as "now playing"
    // without obscuring the syntax highlighting beneath.
    g.setColour(palette.accent.withAlpha(0.25f));
    g.fillRect(highlightBounds_);

    g.setColour(palette.accent.withAlpha(0.8f));
    g.drawRect(highlightBounds_, 1);
}

void HathorTab::HighlightOverlay::setHighlight(
    const juce::Rectangle<int>& bounds) noexcept
{
    // Repaint the old highlight region before changing it.
    if (active_)
    {
        const auto oldRect = highlightBounds_.toFloat().expanded(2);
        repaint(oldRect.toNearestInt());
    }

    highlightBounds_ = bounds;
    active_ = true;

    // Repaint the new region.
    const auto newRect = highlightBounds_.toFloat().expanded(2);
    repaint(newRect.toNearestInt());
}

void HathorTab::HighlightOverlay::clearHighlight() noexcept
{
    if (!active_)
        return;
    active_ = false;
    repaint();
}

// ---------------------------------------------------------------------------
// C1: setNowPlayingHighlight / clearNowPlayingHighlight
// ---------------------------------------------------------------------------

void HathorTab::setNowPlayingHighlight(std::size_t sourceOffset,
                                       const juce::Rectangle<int>& glyphBounds) noexcept
{
    highlightBoundsPrev_ = highlightBounds_;
    highlightOffset_     = sourceOffset;
    highlightBounds_     = glyphBounds;
    highlightActive_     = true;

    // The overlay is a child of HathorTab, positioned to cover the editor.
    // glyphBounds is in editor-local coordinates, so translate to the
    // overlay's coordinate space (which matches the editor's since they
    // share the same parent layout).
    highlightOverlay_.setHighlight(glyphBounds);
}

void HathorTab::clearNowPlayingHighlight() noexcept
{
    if (!highlightActive_)
        return;

    highlightActive_     = false;
    highlightBoundsPrev_ = highlightBounds_;
    highlightBounds_     = {};
    highlightOverlay_.clearHighlight();
}

void HathorTab::paintSlotPlayButton(juce::Graphics& /*g*/)
{
    // Button is a juce::TextButton — paint is handled by JUCE.
    // This method exists for potential custom icon drawing in the future.
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void HathorTab::resized()
{
    // Play/Stop button: small icon button at the top-right of the editor area.
    static constexpr int kButtonSize = 24;
    static constexpr int kButtonMargin = 6;

    slotPlayButton_.setBounds(
        getWidth() - kButtonSize - kButtonMargin,
        kButtonMargin,
        kButtonSize,
        kButtonSize);

    // Editor fills the remaining space, below the button.
    const int editorTop = kButtonSize + kButtonMargin * 2;
    editor_.setBounds(0, editorTop, getWidth(), getHeight() - editorTop);

    // Highlight overlay covers the same area as the editor so that glyph-box
    // coordinates (editor-local) map directly to overlay-local coordinates.
    highlightOverlay_.setBounds(0, editorTop, getWidth(), getHeight() - editorTop);

    // AI-4: LSP components cover the editor area too
    if (diagnosticsOverlay_.isVisible())
        diagnosticsOverlay_.setBounds(0, editorTop, getWidth(), getHeight() - editorTop);

    // AI-4: Ghost text overlay covers the editor area (same as highlight overlay)
    if (ghostOverlay_ && ghostOverlay_->isVisible())
        ghostOverlay_->setBounds(0, editorTop, getWidth(), getHeight() - editorTop);

    if (lspCompletionPopup_)
        lspCompletionPopup_->setBounds(0, editorTop, getWidth(),
                                        std::min(LspCompletionPopup::kPopupWidth, getWidth()));
    if (lspHoverHandler_)
        lspHoverHandler_->setBounds(0, editorTop, getWidth(), getHeight() - editorTop);
}

void HathorTab::lookAndFeelChanged()
{
    // Rebuild palette-derived colours + syntax colour scheme when the theme
    // switches (B3). The LookAndFeel has already been updated by the time this
    // is called (setPalette() → sendLookAndFeelChange()). JUCE's
    // CodeEditorComponent::lookAndFeelChanged() does not refresh its syntax
    // colour scheme, so the active tokeniser's scheme is re-applied here, all
    // sourced from the current HathorLookAndFeel palette.
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    editor_.setColour(juce::CodeEditorComponent::backgroundColourId,
                      palette.surface);
    editor_.setColour(juce::CodeEditorComponent::defaultTextColourId,
                      palette.codeText);
    editor_.setColour(juce::CodeEditorComponent::highlightColourId,
                      palette.accent.withAlpha(0.3f));
    editor_.setColour(juce::CodeEditorComponent::lineNumberBackgroundId,
                      palette.surfaceLow);
    editor_.setColour(juce::CodeEditorComponent::lineNumberTextId,
                      palette.codeLineNum);

    // Re-apply the active tokeniser's palette-derived colour scheme so syntax
    // highlighting tracks the new theme.
    if (useChuckTokeniser_)
        editor_.setColourScheme(chuckTokeniser_.getDefaultColourScheme());
    else
        editor_.setColourScheme(miniTokeniser_.getDefaultColourScheme());

    // Re-apply button visual state to sync colours with the new palette.
    setSlotRunningVisual(slotRunning_);

    // C1: repaint the highlight overlay so it picks up the new palette colours.
    highlightOverlay_.repaint();

     // AI-4: repaint diagnostics overlay to refresh squiggle colours.
    if (diagnosticsOverlay_.isVisible())
        diagnosticsOverlay_.repaint();

    // AI-4: repaint ghost overlay to refresh ghost text colour.
    if (ghostOverlay_ && ghostOverlay_->isVisible())
        ghostOverlay_->repaint();
}

// ---------------------------------------------------------------------------
// juce::CodeDocument::Listener
// ---------------------------------------------------------------------------

void HathorTab::codeDocumentTextInserted(const juce::String& /*newText*/,
                                         int /*insertIndex*/)
{
    markUnsaved();

    // J-3: During a partial-accept insertion, the ghost state has already been
    // updated to the remaining suffix. Don't clear it — just sync to LSP and
    // increment the coordinator revision so the ghost stays valid.
    if (partialAcceptInProgress_)
    {
        if (coordinator_)
            coordinator_->onPartialAcceptDocumentChange();
        if (useChuckTokeniser_)
            triggerChuckDiagnostics();
        else
            notifyLspDidChange();
        return;
    }

    // AI-G3: Document change invalidates ghost state. The coordinator
    // increments docRevision_, cancels pending ghost requests, and clears
    // any active ghost.
    if (coordinator_)
        coordinator_->onDocumentChanged();

    // AI-G6: Clear ghost text on any edit — the ghost is context-sensitive
    // and must be recomputed for the new document state. Also clear the
    // stored result so acceptance verification will fail if the cursor
    // hasn't moved but the document has changed.
     if (ghostOverlay_)
         ghostOverlay_->clearGhost();
     activeGhostResult_.reset();
     if (useChuckTokeniser_)
         triggerChuckDiagnostics();
     else
         notifyLspDidChange();
}

void HathorTab::codeDocumentTextDeleted(int /*startIndex*/,
                                        int /*endIndex*/)
{
    markUnsaved();

    // J-3: If a deletion happened during a partial accept (should not occur
    // in normal flow, but guard against it), treat it as a full document
    // change and clear the ghost.
    if (partialAcceptInProgress_)
    {
        if (coordinator_)
            coordinator_->onDocumentChanged();
        if (ghostOverlay_)
            ghostOverlay_->clearGhost();
        activeGhostResult_.reset();
        partialAcceptInProgress_ = false;
        if (useChuckTokeniser_)
            triggerChuckDiagnostics();
        else
            notifyLspDidChange();
        return;
    }

    // AI-G3: Document change invalidates ghost state.
    if (coordinator_)
        coordinator_->onDocumentChanged();

    // AI-4: Clear ghost text on any edit.
    if (ghostOverlay_)
        ghostOverlay_->clearGhost();
    activeGhostResult_.reset();
    if (useChuckTokeniser_)
        triggerChuckDiagnostics();
    else
        notifyLspDidChange();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void HathorTab::markUnsaved()
{
    if (!unsavedDot_)
    {
        unsavedDot_ = true;
        if (onUnsavedDotChanged)
            onUnsavedDotChanged();
    }
}

// ---------------------------------------------------------------------------
// AI-4: LSP language integration
// ---------------------------------------------------------------------------

void HathorTab::installLspClient(HathorLspClient* client) noexcept
{
    lspClient_ = client;
}

juce::String HathorTab::lspDocumentUri() const
{
    if (filePath_.has_value())
        return "file://" + filePath_->getFullPathName().replace("\\", "/");
    return "untitled://hathor-tab-" + juce::String(slotIndex_);
}

void HathorTab::notifyLspDidOpen()
{
    juce::String uri = lspDocumentUri();
    juce::String text = document_.getAllContent();

    if (useChuckTokeniser_)
    {
        // AI-G7: ChucK tabs don't use the Strudel LSP server.
        // Open the document on the ghost-text client (llm-ls) so that
        // FIM ghost writing works for .ck files.
        if (ghostClient_ && coordinator_ && coordinator_->isGhostEnabled())
            ghostClient_->didOpenDocument(uri.toStdString(), text.toStdString(), "chuck");

        // Trigger initial ChucK compiler diagnostics.
        triggerChuckDiagnostics();
        return;
    }

    if (!lspClient_)
        return;

    lspClient_->didOpenDocument(uri.toStdString(), text.toStdString(), "hathor");

    // AI-4: Mirror document open to the ghost-text (llm-ls) client so that
    // llm-ls can resolve completion context from the synced document.
    // Supports both .hathor and .ck files with their respective languageId.
    if (ghostClient_ && coordinator_ && coordinator_->isGhostEnabled())
    {
        std::string ghostLangId = useChuckTokeniser_ ? "chuck" : "hathor";
        ghostClient_->didOpenDocument(uri.toStdString(), text.toStdString(), ghostLangId);
    }
}

void HathorTab::notifyLspDidChange()
{
    // Debounce: only send if text actually changed since last send.
    static std::unordered_map<juce::Component*, std::string> lastText;
    juce::String currentText = document_.getAllContent();
    std::string currentStr = currentText.toStdString();

    if (lastText[this] == currentStr)
        return;

    lastText[this] = currentStr;

    juce::String uri = lspDocumentUri();
    static int changeVersion = 1;
    ++changeVersion;

    if (!useChuckTokeniser_)
    {
        // .hathor: send change to the Strudel LSP server.
        if (lspClient_)
            lspClient_->didChangeDocument(uri.toStdString(), changeVersion, currentStr);
    }

    // AI-4 / AI-G7: Mirror document change to the ghost-text (llm-ls) client.
    // Supports both .hathor and .ck files.
    if (ghostClient_ && coordinator_ && coordinator_->isGhostEnabled())
    {
        std::string ghostLangId = useChuckTokeniser_ ? "chuck" : "hathor";
        ghostClient_->didChangeDocument(uri.toStdString(), changeVersion, currentStr);
    }
}

void HathorTab::notifyLspDidClose()
{
    juce::String uri = lspDocumentUri();

    if (useChuckTokeniser_)
    {
        // AI-G7: Chuck tabs — just close the ghost document.
        if (ghostClient_ && coordinator_ && coordinator_->isGhostEnabled())
            ghostClient_->didCloseDocument(uri.toStdString());
        return;
    }

    if (!lspClient_)
        return;

    lspClient_->didCloseDocument(uri.toStdString());

    // AI-4: Mirror document close to the ghost-text (llm-ls) client.
    // Supports both .hathor and .ck files.
    if (ghostClient_ && coordinator_ && coordinator_->isGhostEnabled())
        ghostClient_->didCloseDocument(uri.toStdString());
}

void HathorTab::requestLspCompletion()
{
    if (!lspClient_ || !lspCompletionPopup_ || useChuckTokeniser_)
        return;

    // AI-G3: When LSP completion is requested, the coordinator cancels
    // any active ghost and suppresses further ghost display until the
    // popup is dismissed.
    if (coordinator_)
        coordinator_->requestLspCompletion();

    auto caretPos = editor_.getCaretPos();
    int cursorLine = caretPos.getLineNumber();
    int cursorCol = caretPos.getCharacter();

    juce::String uri = lspDocumentUri();

    lspClient_->requestCompletion(
        uri.toStdString(),
        cursorLine,
        cursorCol,
        [this](const lsp::CompletionResult& result)
        {
            if (!lspCompletionPopup_)
                return;

            std::vector<lsp::CompletionCandidate> candidates = result.items;
            if (!candidates.empty())
            {
                lspCompletionPopup_->setCandidates(candidates);

                // Position the popup at the cursor
                juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(
                    editor_.getCaretPosition());
                int popupX = caretRect.getX();
                int popupY = caretRect.getBottom() + 2;

                // Keep within editor bounds
                if (popupX < 0) popupX = 0;
                if (popupY + lspCompletionPopup_->getHeight() > this->getHeight())
                    popupY = std::max(0, this->getHeight() - lspCompletionPopup_->getHeight());

                lspCompletionPopup_->setTopLeftPosition(popupX, popupY);
                lspCompletionPopup_->setVisible(true);
                this->addAndMakeVisible(lspCompletionPopup_.get());
                lspCompletionPopup_->toFront(false);
            }
        });
}

void HathorTab::requestLspHover(int cursorLine, int cursorCol)
{
    if (!lspClient_ || !lspHoverHandler_ || useChuckTokeniser_)
        return;

    // Debounce: skip if same position as last request and pending
    if (hoverPendingLine_ == cursorLine && hoverPendingCol_ == cursorCol && hoverPending_)
        return;

    hoverPendingLine_ = cursorLine;
    hoverPendingCol_ = cursorCol;
    hoverPending_ = true;

    juce::String uri = lspDocumentUri();

    lspClient_->requestHover(
        uri.toStdString(),
        cursorLine,
        cursorCol,
        [this](const std::optional<lsp::Hover>& hover)
        {
            hoverPending_ = false;
            if (!lspHoverHandler_ || !hover.has_value())
                return;

            juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(
                editor_.getCaretPosition());
            lspHoverHandler_->showHover(hover.value(),
                                        {caretRect.getX(), caretRect.getBottom()});
         });
}

void HathorTab::requestChuckCompletion()
{
    // AI-G7: Deterministic ChucK completion from versioned metadata.
    // No ChucK LSP server exists — completion comes entirely from
    // LanguageMetadata::chuckApi + built-in ChucK keyword sets.
    if (!lspCompletionPopup_ || !lspClient_)
        return;

    // AI-G3: When completion is requested, the coordinator cancels
    // any active ghost and suppresses further ghost display until the
    // popup is dismissed.
    if (coordinator_)
        coordinator_->requestLspCompletion();

    // Gather metadata + compatibility from the installed LSP client.
    const auto* metadata = lspClient_->metadata();
    const auto* compat = lspClient_->compatibility();
    if (!metadata || !compat || !compat->compatible)
    {
        // Metadata not available — show empty popup.
        lspCompletionPopup_->setCandidates({});
        return;
    }

    auto caretPos = editor_.getCaretPos();
    int cursorLine = caretPos.getLineNumber();
    int cursorCol = caretPos.getCharacter();

    juce::String docText = document_.getAllContent();

    // Analyze context and produce ChucK-specific completions.
    auto ctx = lsp::analyzeContext(docText.toStdString(), cursorLine, cursorCol);
    auto candidates = lsp::chuckMetadataFallback(*metadata, *compat, ctx);

    // Sort by kind priority (functions/classes first, then alphabetically).
    std::sort(candidates.begin(), candidates.end(), [](const lsp::CompletionCandidate& a,
                                                        const lsp::CompletionCandidate& b) {
        auto kindPri = [](lsp::CompletionItemKind k) -> int {
            switch (k) {
                case lsp::CompletionItemKind::Class:   return 0;
                case lsp::CompletionItemKind::Module:  return 1;
                case lsp::CompletionItemKind::Function: return 2;
                case lsp::CompletionItemKind::Field:   return 3;
                case lsp::CompletionItemKind::Variable:return 4;
                case lsp::CompletionItemKind::Value:   return 5;
                case lsp::CompletionItemKind::Keyword: return 6;
                default: return 7;
            }
        };
        int pa = kindPri(a.kind);
        int pb = kindPri(b.kind);
        if (pa != pb) return pa < pb;
        // Within same kind, sort by label (case-sensitive for ChucK).
        std::string la = a.label;
        std::string lb = b.label;
        return la < lb;
    });

    if (!candidates.empty())
    {
        lspCompletionPopup_->setCandidates(candidates);

        // Position the popup at the cursor.
        juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(
            editor_.getCaretPosition());
        int popupX = caretRect.getX();
        int popupY = caretRect.getBottom() + 2;

        if (popupX < 0) popupX = 0;
        if (popupY + lspCompletionPopup_->getHeight() > this->getHeight())
            popupY = std::max(0, this->getHeight() - lspCompletionPopup_->getHeight());

        lspCompletionPopup_->setTopLeftPosition(popupX, popupY);
        lspCompletionPopup_->setVisible(true);
        this->addAndMakeVisible(lspCompletionPopup_.get());
        lspCompletionPopup_->toFront(false);
    }
    else
    {
        // No completions — dismiss popup.
        if (lspCompletionPopup_->hasCandidates())
            lspCompletionPopup_->dismiss();
    }
}

void HathorTab::requestChuckHover(int cursorLine, int cursorCol)
{
    // AI-G7: ChucK hover from versioned metadata (no LSP server for ChucK).
    if (!lspHoverHandler_ || !lspClient_)
        return;

    const auto* metadata = lspClient_->metadata();
    const auto* compat = lspClient_->compatibility();
    if (!metadata || !compat || !compat->compatible)
        return;

    // Debounce: skip if same position as last request and pending.
    if (hoverPendingLine_ == cursorLine && hoverPendingCol_ == cursorCol && hoverPending_)
        return;

    hoverPendingLine_ = cursorLine;
    hoverPendingCol_ = cursorCol;
    hoverPending_ = true;

    juce::String docText = document_.getAllContent();

    // Extract the word at the cursor position.
    int charIdx = editor_.getCaretPosition();
    int wordStart = charIdx;
    int wordEnd = charIdx;

    while (wordStart > 0)
    {
        char c = docText[wordStart - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            --wordStart;
        else
            break;
    }

    while (wordEnd < static_cast<int>(docText.length()))
    {
        char c = docText[wordEnd];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            ++wordEnd;
        else
            break;
    }

    juce::String word = docText.substring(wordStart, wordEnd - wordStart);
    if (word.isEmpty())
    {
        hoverPending_ = false;
        return;
    }

    // Look up the word in metadata::chuckApi.
    std::string wordStr = word.toStdString();
    for (const auto& api : metadata->chuckApi)
    {
        if (api.name == wordStr)
        {
            hoverPending_ = false;
            lsp::Hover h;
            h.contents.push_back({.kind = "markdown", .value = "**`" + api.name + "`** — " + api.kind});
            h.contents.push_back({.kind = "markdown", .value = api.description});
            if (!api.signature.empty())
                h.contents.push_back({.kind = "markdown", .value = "```chuck\n" + api.signature + "\n```"});
            if (api.example)
                h.contents.push_back({.kind = "markdown", .value = "Example:\n```chuck\n" + *api.example + "\n```"});

            juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(charIdx);
            lspHoverHandler_->showHover(h, {caretRect.getX(), caretRect.getBottom()});
            return;
        }
    }

    // Also check built-in keyword sets.
    if (chuckKeywordSet().count(wordStr) ||
        chuckTypeSet().count(wordStr) ||
        chuckConstantSet().count(wordStr) ||
        chuckModifierSet().count(wordStr) ||
        chuckTypeKeywordSet().count(wordStr) ||
        chuckVariableLanguageSet().count(wordStr) ||
        chuckUgenSet().count(wordStr) ||
        chuckLibrarySet().count(wordStr))
    {
        hoverPending_ = false;
        lsp::Hover h;
        h.contents.push_back({.kind = "markdown", .value = "**`" + wordStr + "`** — ChucK built-in"});
        juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(charIdx);
        lspHoverHandler_->showHover(h, {caretRect.getX(), caretRect.getBottom()});
        return;
    }

    hoverPending_ = false;
}

void HathorTab::requestLspSignatureHelp()
{
    if (!lspClient_ || useChuckTokeniser_)
        return;

    auto caretPos = editor_.getCaretPos();
    int cursorLine = caretPos.getLineNumber();
    int cursorCol = caretPos.getCharacter();
    juce::String uri = lspDocumentUri();

    lspClient_->requestSignatureHelp(
        uri.toStdString(),
        cursorLine,
        cursorCol,
        [this](const std::optional<lsp::SignatureHelp>& sig)
        {
            if (sig.has_value() && !sig->signatures.empty())
            {
                const auto& s = sig->signatures[0];
                // Show signature in status bar via callback
                if (onStatusMessage)
                    onStatusMessage(juce::String(s.label));
            }
        });
}

void HathorTab::onCompletionSelected(const lsp::CompletionCandidate& candidate)
{
    juce::String insertText(candidate.insertText.empty()
                                ? candidate.label
                                : candidate.insertText);

    // Get the word boundaries for the current word
    int caretAbs = editor_.getCaretPosition();
    juce::String docText = document_.getAllContent();

    int wordStart = caretAbs;
    int wordEnd = caretAbs;

    while (wordStart > 0)
    {
        char c = docText[wordStart - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            --wordStart;
        else
            break;
    }

    while (wordEnd < static_cast<int>(docText.length()))
    {
        char c = docText[wordEnd];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            ++wordEnd;
        else
            break;
    }

    if (wordEnd > wordStart)
        document_.deleteSection(wordStart, wordEnd);
    document_.insertText(wordStart, insertText);

    if (lspCompletionPopup_)
        lspCompletionPopup_->dismiss();
}

// ---------------------------------------------------------------------------
// AI-4: LSP key handling (KeyListener on editor_)
// ---------------------------------------------------------------------------
// Note: CodeEditorComponent::keyPressed() consumes Up/Down/Enter/Tab/Escape
// before our KeyListener is called. Only keys the editor doesn't handle
// (like Ctrl+Space) reach this listener. The popup handles its own
// navigation keys when it has focus.
// ---------------------------------------------------------------------------

bool HathorTab::handleLspKeyPress(const juce::KeyPress& key)
{
    // AI-G3: Ctrl+Space → deterministic completion.
    // For .hathor: LSP completion (Strudel LSP + metadata fallback).
    // For .ck: deterministic ChucK completion from versioned metadata (AI-G7).
    // The ghost trigger is moved to Ctrl+Shift+Space to eliminate the conflict.
    if (key == juce::KeyPress(' ', juce::ModifierKeys::ctrlModifier, 0))
    {
        if (useChuckTokeniser_)
            requestChuckCompletion();
        else
            requestLspCompletion();
        return true;
    }

    // Ctrl+Shift+Space: force a ghost completion request.
    // Clears any pending ghost request and restarts the debounce cycle.
    if (key == juce::KeyPress(' ', juce::ModifierKeys::ctrlModifier | juce::ModifierKeys::shiftModifier, 0))
    {
        if (coordinator_ && coordinator_->isGhostEnabled())
        {
            coordinator_->cancelPendingGhostRequest();
            triggerGhostCompletion();
            return true;
        }
    }

    // Accept ghost — if ghost is visible, accept on Ctrl+.
    if (coordinator_ && coordinator_->isGhostEnabled()
        && ghostOverlay_ && ghostOverlay_->hasGhost())
    {
        if (key.getModifiers().isCtrlDown() && key.getKeyCode() == '.')
        {
            acceptGhostCompletion();
            return true;
        }
    }

    // Tab: accept ghost (if visible). If no ghost, let it fall through
    // to the editor's default Tab handling (indentation).
    if (key == juce::KeyPress::tabKey)
    {
        if (coordinator_ && coordinator_->isGhostActive()
            && ghostOverlay_ && ghostOverlay_->hasGhost())
        {
            acceptGhostCompletion();
            return true;
        }
    }

    // Escape: dismiss popup or ghost
    if (key == juce::KeyPress::escapeKey)
    {
        if (lspCompletionPopup_ && lspCompletionPopup_->hasCandidates())
        {
            lspCompletionPopup_->dismiss();
            if (coordinator_)
                coordinator_->onLspPopupDismissed();
            return true;
        }
        if (coordinator_ && coordinator_->isGhostActive())
        {
            dismissGhostCompletion();
            return true;
        }
    }

    // J-2: Alt+→ / Alt+← cycle ghost candidates (if ghost active).
    // Inspects existing key handling first — arrows are only consumed when
    // ghost has multiple candidates; otherwise they fall through to the
    // editor's default cursor-movement behaviour.
    if (coordinator_ && coordinator_->isGhostEnabled()
        && ghostOverlay_ && ghostOverlay_->hasGhost()
        && ghostOverlay_->candidateCount() > 1)
    {
        if (key.getModifiers().isAltDown() && key.getKeyCode() == juce::KeyPress::rightKey)
        {
            cycleGhostNext();
            return true;
        }
         if (key.getModifiers().isAltDown() && key.getKeyCode() == juce::KeyPress::leftKey)
         {
             cycleGhostPrev();
             return true;
         }
     }

     // J-3: Ctrl+→ partially accepts the next word/token of the ghost text.
     // The accepted prefix is inserted into the document as a normal undoable
     // edit; the remaining suffix stays as ghost text for further acceptance
     // or dismissal. Does not issue a new LLM request.
     // Only consumed when a ghost is active; otherwise falls through to the
     // editor's default cursor-movement behaviour.
     if (coordinator_ && coordinator_->isGhostEnabled()
         && ghostOverlay_ && ghostOverlay_->hasGhost())
     {
         if (key.getModifiers().isCtrlDown()
             && key.getKeyCode() == juce::KeyPress::rightKey)
         {
             partialAcceptGhostCompletion();
             return true;
         }
     }

    // Up/Down: navigate LSP completion popup (if visible)
    if (lspCompletionPopup_ && lspCompletionPopup_->hasCandidates())
    {
        if (key == juce::KeyPress::upKey)
        {
            // Let the popup handle it
            return false;
        }
        if (key == juce::KeyPress::downKey)
        {
            return false;
        }
    }

    return false;
}

void HathorTab::handleCursorMove()
{
     // J-3: During partial-accept insertion, suppress ghost re-triggering.
    // The cursor moves as the accepted prefix is inserted, but the ghost
    // overlay is manually re-displayed with the remaining suffix after
    // insertText() returns.
    if (partialAcceptInProgress_)
        return;

    // AI-8: Notify listeners that the cursor position may have changed.
    if (onCursorMoved)
        onCursorMoved();

    if (!lspClient_ || !lspHoverHandler_)
        return;

    // AI-4 + AI-G3: Trigger ghost text on cursor movement.
    // The coordinator ensures ghost is suppressed when the LSP popup is
    // visible. The ghost logic handles debounce internally.
    if (coordinator_ && coordinator_->isGhostEnabled() && !coordinator_->isLspPopupActive())
    {
        triggerGhostCompletion();
    }

    auto caretPos = editor_.getCaretPos();
    int cursorLine = caretPos.getLineNumber();
    int cursorCol = caretPos.getCharacter();

    // Debounce: only request hover if position changed
    if (hoverPendingLine_ == cursorLine && hoverPendingCol_ == cursorCol)
        return;

    hoverPendingLine_ = cursorLine;
    hoverPendingCol_ = cursorCol;
    hoverPending_ = true;

    // Request hover via async callback with debounce — only fire if cursor
    // hasn't moved by the time the lambda runs.
    // AI-G7: For .ck tabs, use deterministic metadata hover (no LSP server).
    const int line = cursorLine;
    const int col = cursorCol;
    juce::MessageManager::callAsync([this, line, col]() {
        if (hoverPendingLine_ == line && hoverPendingCol_ == col && hoverPending_)
        {
            hoverPending_ = false;
            if (useChuckTokeniser_)
                requestChuckHover(line, col);
            else
                requestLspHover(line, col);
        }
    });
}

void HathorTab::notifyLspDiagnostics(const std::string& uri,
                                      const std::vector<lsp::Diagnostic>& diagnostics)
{
    if (!lspDiagnostics_)
        return;

    lspDiagnostics_->setDiagnostics(uri, diagnostics);

    // Convert diagnostics to pixel rectangles for the overlay
    std::vector<DiagnosticsOverlay::Squiggle> squiggles;
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    for (const auto& diag : diagnostics)
    {
        int startLine = diag.range.start.line;
        int startChar = diag.range.start.character;
        int endLine = diag.range.end.line;
        int endChar = diag.range.end.character;

        if (startLine != endLine)
            continue;

        juce::CodeDocument::Position startPos(document_, startLine, startChar);
        juce::CodeDocument::Position endPos(document_, endLine, endChar);

        int startAbs = startPos.getPosition();
        int endAbs = endPos.getPosition();

        if (startAbs >= endAbs)
            continue;

        // Get pixel bounds for the text range
        auto bounds = editor_.getTextBounds(juce::Range<int>(startAbs, endAbs));

        juce::Colour diagColor = palette.error;
        if (diag.severity.has_value())
        {
            if (diag.severity.value() == lsp::DiagnosticSeverity::Error)
                diagColor = palette.error;
            else if (diag.severity.value() == lsp::DiagnosticSeverity::Warning)
                diagColor = palette.warning;
            else
                diagColor = palette.textSecondary;
        }

        for (int i = 0; i < bounds.getNumRectangles(); ++i)
        {
            squiggles.push_back({bounds.getRectangle(i), diagColor});
        }
    }

     diagnosticsOverlay_.setDiagnostics(squiggles);
}

void HathorTab::notifyChuckDiagnostics(const std::string& uri,
                                       const audio_worker::ChuckDiagnostic& diag)
{
    // AI-G7: Convert the real libchuck/validateChuckSource diagnostic
    // to LSP Diagnostic format and display it via the same overlay as LSP.
    if (!lspDiagnostics_)
        return;

    lsp::ChuckCompileDiagnostic cd;
    cd.ok = diag.ok;
    cd.errorLine = diag.errorLine;
    cd.errorColumn = diag.errorColumn;
    cd.message = diag.message;

    const auto* metadata = lspClient_ ? lspClient_->metadata() : nullptr;
    const auto* compat = lspClient_ ? lspClient_->compatibility() : nullptr;

    juce::String docText = document_.getAllContent();
    auto diags = lsp::chuckDiagnostics(cd, metadata, compat,
                                      docText.toStdString());

    // Reuse the same display path as LSP diagnostics.
    notifyLspDiagnostics(uri, diags);

    // AI-8: Forward ChucK diagnostics to the LspContextBridge so they
    // can be included in the authoring context payload for .ck files.
    if (onChuckDiagnostics)
        onChuckDiagnostics(uri, diags);
}

void HathorTab::triggerChuckDiagnostics()
{
    // AI-G7: Debounced ChucK compiler diagnostics.
    // Runs validateChuckSource() on a background thread (serialized by
    // the global chuckCompileMutex inside ChuckDiagnostics.cpp) and posts
    // the result back to the message thread.
    if (!useChuckTokeniser_)
        return;

    // Simple time-based debounce: skip if we ran diagnostics less than
    // kDebounceMs ago.
    static constexpr int kDebounceMs = 500;
    const int64_t nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    if (nowMs - chuckLastDiagTimeMs_ < kDebounceMs)
        return;

    chuckLastDiagTimeMs_ = nowMs;

    // Capture the current document text and URI (stable snapshots).
    juce::String docText = document_.getAllContent();
    std::string sourceCopy = docText.toStdString();
    std::string uriCopy = lspDocumentUri().toStdString();

    // Capture raw pointer to self for the async callback.
    // HathorTab is owned by EditorArea's tabs_ vector; it will not be
    // destroyed while a diagnostic callback is in flight because the
    // callback is processed on the message thread and the tab is only
    // destroyed when the user closes it (which cancels async callbacks).
    HathorTab* self = this;

    // Run the (potentially slow) validation on a detached thread.
    std::thread([self, sourceCopy, uriCopy]() {
        auto diag = hathor::audio_worker::validateChuckSource(sourceCopy);

        // Post back to the message thread for display.
        juce::MessageManager::callAsync([self, uriCopy, diag]() {
            self->notifyChuckDiagnostics(uriCopy, diag);
        });
    }).detach();
}

// ---------------------------------------------------------------------------
// AI-4: Ghost text (llm-ls inline completion)
// ---------------------------------------------------------------------------

void HathorTab::installGhostClient(GhostLlmClient* client) noexcept
{
    ghostClient_ = client;
}

void HathorTab::triggerGhostCompletion()
{
    if (!coordinator_ || !coordinator_->isGhostEnabled())
        return;

    // AI-G3: The coordinator suppresses ghost when the LSP popup is visible.
    if (coordinator_->isLspPopupActive())
        return;

    // Build the current editor context
    auto caretPos = editor_.getCaretPos();
    int cursorLine = caretPos.getLineNumber();
    int cursorCol = caretPos.getCharacter();

    juce::String text = document_.getAllContent();

    lsp::GhostContext ctx;
    ctx.documentText = text.toStdString();
    ctx.uri = lspDocumentUri().toStdString();
    ctx.languageId = useChuckTokeniser_ ? "chuck" : "hathor";
    ctx.line = cursorLine;
    ctx.character = cursorCol;

    // J-4: Carry selection state through the existing authoring-context path
    // (same snapshot source as AI-8) so J-1's trigger policy can suppress
    // ghost completion when a non-empty selection is active — completion at
    // a cursor over a selection is not meaningful.
    const auto selRegion = editor_.getHighlightedRegion();
    ctx.hasSelection = !selRegion.isEmpty();
    if (ctx.hasSelection)
        ctx.selectedText = editor_.getTextInRange(selRegion).toStdString();

    // AI-8: Inject dynamic authoring context (supported-surface, diagnostics)
    // as additional FIM context. The callback is installed by EditorArea
    // and delegates to ControlInterface's AuthoringContext.
    if (getAuthoringContext)
        ctx.authoringContext = getAuthoringContext();

    // Feed context to the coordinator — it delegates to GhostCompletionLogic
    // which handles debounce. The request is only sent when debounce expires
    // via ghostTick().
    int64_t nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());
    coordinator_->triggerGhostCompletion(ctx, nowMs);

    // AI-G6: Clear any existing ghost — a new request cycle has started.
    // This clears the overlay AND the stored result so acceptance verification
    // will fail if a response arrives for a stale context.
    if (ghostOverlay_)
        ghostOverlay_->clearGhost();
    activeGhostResult_.reset();
}

void HathorTab::acceptGhostCompletion()
{
    if (!ghostOverlay_ || !coordinator_)
        return;

    // AI-G6: Verify the cursor hasn't moved since the ghost was generated.
    // If the cursor moved, dismiss instead of accepting — the ghost text
    // was generated for a different context and must not be inserted.
    if (activeGhostResult_.has_value())
    {
        const auto& gr = activeGhostResult_.value();
        const auto caretPos = editor_.getCaretPos();
        if (static_cast<int>(caretPos.getLineNumber()) != gr.cursorLine ||
            static_cast<int>(caretPos.getIndexInLine()) != gr.character)
        {
            // Cursor moved — dismiss, don't accept
            activeGhostResult_.reset();
            ghostOverlay_->clearGhost();
            return;
        }
    }

    // Get the text to insert
    std::string text = ghostOverlay_->acceptGhost();
    if (text.empty())
        return;

    // AI-G6: Clear the stored result BEFORE inserting — the ghost is now
    // being materialized into the document.
    activeGhostResult_.reset();

    // AI-G3: Accept through the coordinator — it handles mode transition
    // and returns the accept notification params.
    auto acceptParams = coordinator_->onGhostAccepted();
    if (acceptParams.has_value() && ghostClient_)
        ghostClient_->sendAccept(*acceptParams);

    // AI-G6: Insert the EXACT generated text at the current cursor position
    // using the editor's normal document-edit mechanism. This creates a
    // proper, undoable edit — the ghost text becomes a normal document edit.
    int caretAbs = editor_.getCaretPosition();
    document_.insertText(caretAbs, juce::String(text));
}

void HathorTab::partialAcceptGhostCompletion()
{
    if (!ghostOverlay_ || !coordinator_)
        return;

    // AI-G6: Verify the cursor hasn't moved since the ghost was generated.
    if (activeGhostResult_.has_value())
    {
        const auto& gr = activeGhostResult_.value();
        const auto caretPos = editor_.getCaretPos();
        if (static_cast<int>(caretPos.getLineNumber()) != gr.cursorLine ||
            static_cast<int>(caretPos.getIndexInLine()) != gr.character)
        {
            // Cursor moved — dismiss the stale ghost instead.
            coordinator_->onGhostRejected();
            ghostOverlay_->clearGhost();
            activeGhostResult_.reset();
            return;
        }
    }

    // Get the currently selected candidate text (J-2 integration).
    auto selected = coordinator_->selectedGhostResult();
    if (!selected.has_value() || selected->text.empty())
        return;

    // J-3: Find the next token boundary — accept up to and including the
    // first whitespace character (one word/token of the ghost text).
    size_t acceptLen = lsp::GhostCompletionLogic::findNextTokenBoundary(selected->text);

    // If the boundary is the entire text, do a full accept instead.
    if (acceptLen >= selected->text.size())
    {
        acceptGhostCompletion();
        return;
    }

    // J-3: Ask the coordinator to split the ghost — the accepted prefix is
    // returned for insertion; the remaining suffix stays as ghost state.
    // No LLM request is issued; no notification is sent to llm-ls.
    auto partialResult = coordinator_->onGhostPartialAccepted(acceptLen);
    if (!partialResult.has_value())
        return;

    // Flag: suppress ghost clearing in codeDocumentTextInserted and
    // handleCursorMove while we insert the accepted prefix.
    partialAcceptInProgress_ = true;

    // AI-G6: Insert the accepted prefix into the document using the normal
    // CodeDocument edit mechanism — this creates a proper, undoable edit.
    // The remaining suffix does NOT enter the document.
    int caretAbs = editor_.getCaretPosition();
    document_.insertText(caretAbs, juce::String(partialResult->acceptedText));

    // Document insert has triggered codeDocumentTextInserted (which, with the
    // flag, incremented the coordinator revision and did NOT clear the ghost)
    // and caretPositionMoved (which was suppressed by the flag).
    partialAcceptInProgress_ = false;

    // Re-display the ghost overlay with the remaining suffix at the new
    // cursor position.
    auto remaining = coordinator_->selectedGhostResult();
    if (remaining.has_value())
    {
        juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(
            editor_.getCaretPosition());

        ghostOverlay_->setGhostText(remaining->text, caretRect, 0);
        ghostOverlay_->setCandidateIndicator(
            coordinator_->ghostCandidateCount(),
            coordinator_->ghostSelectedCandidateIndex());

        // Update the stored result for cursor-verification on the next
        // accept / partial-accept.
        activeGhostResult_ = *remaining;

        if (!coordinator_->isLspPopupActive())
            ghostOverlay_->showGhost();
        else
            ghostOverlay_->hideGhost();
    }
    else
    {
        // Should not happen, but handle gracefully.
        ghostOverlay_->clearGhost();
        activeGhostResult_.reset();
    }
}

void HathorTab::dismissGhostCompletion()
{
    if (!coordinator_ || !ghostOverlay_)
        return;

    // AI-G3: Reject through the coordinator
    auto rejectParams = coordinator_->onGhostRejected();
    if (rejectParams.has_value() && ghostClient_)
        ghostClient_->sendReject(*rejectParams);

    // Clear the ghost overlay and stored result
    ghostOverlay_->clearGhost();
    activeGhostResult_.reset();
}

// ---------------------------------------------------------------------------
// J-2: Candidate cycling (Alt+→ / Alt+←)
// ---------------------------------------------------------------------------
// Cycling operates entirely on the cached candidate set in GhostCompletionLogic
// — no LLM request is issued. The overlay's displayed text is updated to show
// the newly selected candidate. The document is never modified during cycling.
// ---------------------------------------------------------------------------

void HathorTab::cycleGhostNext()
{
    if (!coordinator_ || !ghostOverlay_)
        return;

    // Delegate to the coordinator (which delegates to GhostCompletionLogic).
    // This changes the selectedIndex of the cached active ghost.
    coordinator_->selectNextGhostCandidate();

    // Get the newly selected candidate and update the overlay display.
    auto selected = coordinator_->selectedGhostResult();
    if (!selected.has_value())
        return;

    // Re-resolve the caret pixel rectangle — the cursor hasn't moved
    // but we recompute in case of scroll or resize.
    juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(
        editor_.getCaretPosition());

    ghostOverlay_->setGhostText(selected->text, caretRect, 0);
    ghostOverlay_->setCandidateIndicator(
        coordinator_->ghostCandidateCount(),
        coordinator_->ghostSelectedCandidateIndex());

    if (!coordinator_->isLspPopupActive())
        ghostOverlay_->showGhost();
    else
        ghostOverlay_->hideGhost();
}

void HathorTab::cycleGhostPrev()
{
    if (!coordinator_ || !ghostOverlay_)
        return;

    coordinator_->selectPreviousGhostCandidate();

    auto selected = coordinator_->selectedGhostResult();
    if (!selected.has_value())
        return;

    juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(
        editor_.getCaretPosition());

    ghostOverlay_->setGhostText(selected->text, caretRect, 0);
    ghostOverlay_->setCandidateIndicator(
        coordinator_->ghostCandidateCount(),
        coordinator_->ghostSelectedCandidateIndex());

    if (!coordinator_->isLspPopupActive())
        ghostOverlay_->showGhost();
    else
        ghostOverlay_->hideGhost();
}

void HathorTab::ghostTick()
{
    if (!coordinator_ || !coordinator_->isGhostEnabled()
        || !ghostClient_ || !ghostOverlay_)
        return;

    // AI-G3: The coordinator suppresses ghost ticks when the LSP popup
    // is visible.
    if (coordinator_->isLspPopupActive())
        return;

    // Let the coordinator check debounce + timeout
    int64_t nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());

    auto opt = coordinator_->onGhostTick(nowMs);
    if (!opt.has_value())
        return;

    // The coordinator returned a request + ID — send it via the client
    auto [req, requestId] = std::move(opt.value());

    // Resolve provider config from env
    auto config = lsp::GhostProviderResolver::resolve();
    if (!config.has_value())
    {
        ghostOverlay_->clearGhost();
        return;
    }

    // Rebuild the request with the resolved provider config
    const auto& ctx = coordinator_->ghostLogic().currentContext();
    req = lsp::GhostCompletionLogic::buildRequest(ctx, *config);

    // J-2: Preserve maxCandidates from the coordinator's ghost logic config.
    req.maxCandidates = coordinator_->ghostLogic().maxCandidates();

    // Send the request via the llm-ls client
    ghostClient_->requestGhostCompletion(
        req,
        requestId,
        [this, requestId](const std::string& id,
                           const lsp::GhostCompletionResponse& resp)
        {
            if (!coordinator_ || !ghostOverlay_)
                return;

            int64_t responseTime = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                .count());

            // AI-G3: The coordinator handles LSP coexistence — it
            // suppresses the ghost response if the LSP popup is visible.
            auto result = coordinator_->onGhostResponse(id, resp, responseTime);

            if (!result.has_value() || result->isEmpty())
            {
                ghostOverlay_->clearGhost();
                return;
            }

             // AI-G6: Verify the cursor hasn't moved since the response was
             // generated. The coordinator's revision check already ensures
             // the document hasn't changed, but we also check that the cursor
             // is still at the expected position. If the cursor moved, the
             // ghost was already cleared by codeEditorCaretMoved → but
             // defense-in-depth: reject if positions don't match.
             if (result.has_value())
             {
                 const auto& gr = result.value();
                 const auto caretPos = editor_.getCaretPos();
                 if (static_cast<int>(caretPos.getLineNumber()) != gr.cursorLine ||
                     static_cast<int>(caretPos.getIndexInLine()) != gr.character)
                 {
                     // Cursor moved — discard the stale ghost
                     ghostOverlay_->clearGhost();
                     activeGhostResult_.reset();
                     return;
                 }
             }

             // Display the ghost text at the cursor position from the result
             auto ghostResult = result.value();

             // AI-G6: Resolve the caret pixel rectangle in editor-local
             // coordinates. Since GhostTextOverlay shares the same bounds
             // as the editor (set in HathorTab::resized), editor-local
             // coordinates map directly to overlay-local coordinates.
             // This follows the same pattern as HighlightOverlay — no
             // setTopLeftPosition() which would break alignment.
             juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(
                 editor_.getCaretPosition());

              ghostOverlay_->setGhostText(ghostResult.text, caretRect, 0);

              // J-2: Set the candidate indicator badge on the overlay
              ghostOverlay_->setCandidateIndicator(
                  coordinator_->ghostCandidateCount(),
                  coordinator_->ghostSelectedCandidateIndex());

              // Store the result for cursor verification on accept
              activeGhostResult_ = ghostResult;

             // AI-G3: Only show the ghost overlay if the coordinator
             // hasn't entered LspPopupActive mode (no late ghost-behind-popup).
             if (!coordinator_->isLspPopupActive())
             {
                 ghostOverlay_->showGhost();
             }
             else
             {
                 // LSP popup took over while the response was in flight —
                 // hide the ghost overlay.
                 ghostOverlay_->hideGhost();
             }
        });
}

// ---------------------------------------------------------------------------
// DiagnosticsOverlay implementation
// ---------------------------------------------------------------------------

void HathorTab::DiagnosticsOverlay::paint(juce::Graphics& g)
{
    if (squiggles_.empty())
        return;

    for (const auto& sq : squiggles_)
    {
        juce::Path path;
        int x = sq.bounds.getX();
        int endX = sq.bounds.getRight();
        int y = sq.bounds.getBottom() + 1;

        path.startNewSubPath(static_cast<float>(x), static_cast<float>(y));

        while (x < endX)
        {
            x += 2;
            path.lineTo(static_cast<float>(x), static_cast<float>(y + 2));
            x += 2;
            if (x < endX)
                path.lineTo(static_cast<float>(x), static_cast<float>(y));
        }

        g.setColour(sq.colour);
        g.strokePath(path, juce::PathStrokeType(1.0f));
    }
}

void HathorTab::DiagnosticsOverlay::setDiagnostics(const std::vector<Squiggle>& squiggles) noexcept
{
    squiggles_ = squiggles;
    repaint();
}

void HathorTab::DiagnosticsOverlay::clearDiagnostics() noexcept
{
    squiggles_.clear();
    repaint();
}

} // namespace hathor::ui
