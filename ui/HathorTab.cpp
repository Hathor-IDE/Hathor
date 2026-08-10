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

     addAndMakeVisible(highlightOverlay_);
     highlightOverlay_.setInterceptsMouseClicks(false, false);

     addAndMakeVisible(editor_);

     // AI-4: LSP language integration components
     if (!useChuckTokeniser_)
     {
         lspCompletionPopup_ = std::make_unique<LspCompletionPopup>(
              [this](const lsp::CompletionCandidate& c) { onCompletionSelected(c); },
              []() { /* popup dismissed */ });

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

      // AI-4: Ghost text overlay (llm-ls inline completion)
      // Only for .hathor tabs (not .ck)
      if (!useChuckTokeniser_)
      {
          ghostLogic_ = std::make_unique<lsp::GhostCompletionLogic>();
          ghostLogic_->setEnabled(lsp::GhostProviderResolver::isEnabled());
          ghostLogic_->setDebounceMs(300);
          ghostLogic_->setTimeoutMs(5000);

          ghostOverlay_ = std::make_unique<GhostTextOverlay>();
          ghostOverlay_->setInterceptsMouseClicks(false, false);
          addAndMakeVisible(*ghostOverlay_);
          ghostOverlay_->setVisible(false);
      }

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
    // AI-4: Clear ghost text on any edit — the ghost is context-sensitive
    // and must be recomputed for the new document state.
    if (ghostOverlay_)
        ghostOverlay_->clearGhost();
    notifyLspDidChange();
}

void HathorTab::codeDocumentTextDeleted(int /*startIndex*/,
                                         int /*endIndex*/)
{
    markUnsaved();
    // AI-4: Clear ghost text on any edit.
    if (ghostOverlay_)
        ghostOverlay_->clearGhost();
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
    if (!lspClient_ || useChuckTokeniser_)
        return;

    juce::String uri = lspDocumentUri();
    juce::String text = document_.getAllContent();
    lspClient_->didOpenDocument(uri.toStdString(), text.toStdString(), "hathor");
}

void HathorTab::notifyLspDidChange()
{
    if (!lspClient_ || useChuckTokeniser_)
        return;

    // Debounce: only send if text actually changed since last send
    static std::unordered_map<juce::Component*, std::string> lastText;
    juce::String currentText = document_.getAllContent();
    std::string currentStr = currentText.toStdString();

    if (lastText[this] == currentStr)
        return;

    lastText[this] = currentStr;

    juce::String uri = lspDocumentUri();
    static int changeVersion = 1;
    ++changeVersion;
    lspClient_->didChangeDocument(uri.toStdString(), changeVersion, currentStr);
}

void HathorTab::notifyLspDidClose()
{
    if (!lspClient_ || useChuckTokeniser_)
        return;

    juce::String uri = lspDocumentUri();
    lspClient_->didCloseDocument(uri.toStdString());
}

void HathorTab::requestLspCompletion()
{
    if (!lspClient_ || !lspCompletionPopup_ || useChuckTokeniser_)
        return;

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
    // AI-4: Ghost text — Ctrl+Space forces a ghost completion request
    // (overrides the normal debounce / idle trigger)
    if (!useChuckTokeniser_ && ghostLogic_ && ghostLogic_->isEnabled())
    {
        if (key == juce::KeyPress(' ', juce::ModifierKeys::ctrlModifier, 0))
        {
            // Force trigger — clear debounce and request immediately
            ghostLogic_->cancelPendingRequest();
            triggerGhostCompletion();
            return true;
        }
    }

    // Tab to accept ghost — if ghost is visible, the caller will handle
    // it. The CodeEditorComponent consumes Tab for indentation, so we
    // can't intercept it here. Instead, acceptance happens via:
    //   - EditorArea::acceptGhostOnActiveTab() (called by a keybinding)
    //   - Typing text that matches the ghost prefix (auto-accepts)
    if (!useChuckTokeniser_ && ghostLogic_ && ghostLogic_->isEnabled()
        && ghostOverlay_ && ghostOverlay_->hasGhost())
    {
        if (key.getModifiers().isCtrlDown() && key.getKeyCode() == '.')
        {
            acceptGhostCompletion();
            return true;
        }
    }

    // Ctrl+Space: trigger manual LSP completion
    if (key == juce::KeyPress(' ', juce::ModifierKeys::ctrlModifier, 0))
    {
        requestLspCompletion();
        return true;
    }

    // Escape: dismiss completion popup (works when popup doesn't have focus)
    if (key == juce::KeyPress::escapeKey &&
        lspCompletionPopup_ && lspCompletionPopup_->hasCandidates())
    {
        lspCompletionPopup_->dismiss();
        return true;
    }

    return false; // let other handlers process the key
}

void HathorTab::handleCursorMove()
{
    // AI-8: Notify listeners that the cursor position may have changed.
    if (onCursorMoved)
        onCursorMoved();

    if (!lspClient_ || !lspHoverHandler_ || useChuckTokeniser_)
        return;

    // AI-4: Trigger ghost text on cursor movement (debounced in GhostCompletionLogic)
    if (ghostLogic_ && ghostLogic_->isEnabled())
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
    // hasn't moved by the time the lambda runs
    const int line = cursorLine;
    const int col = cursorCol;
    juce::MessageManager::callAsync([this, line, col]() {
        if (hoverPendingLine_ == line && hoverPendingCol_ == col && hoverPending_)
        {
            hoverPending_ = false;
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

// ---------------------------------------------------------------------------
// AI-4: Ghost text (llm-ls inline completion)
// ---------------------------------------------------------------------------

void HathorTab::installGhostClient(GhostLlmClient* client) noexcept
{
    ghostClient_ = client;
}

void HathorTab::triggerGhostCompletion()
{
    if (!ghostLogic_ || !ghostLogic_->isEnabled())
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

    // Feed context to the logic layer — it handles debounce and returns
    // nullopt (the request is only sent when debounce expires via ghostTick).
    int64_t nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());
    ghostLogic_->onEditorChanged(ctx, nowMs);

    // Clear any existing ghost — a new request cycle has started
    if (ghostOverlay_)
        ghostOverlay_->clearGhost();
}

void HathorTab::acceptGhostCompletion()
{
    if (!ghostOverlay_ || !ghostLogic_)
        return;

    // Get the text to insert
    std::string text = ghostOverlay_->acceptGhost();
    if (text.empty())
        return;

    // Send accept notification to llm-ls
    auto acceptParams = ghostLogic_->onAccept();
    if (acceptParams.has_value() && ghostClient_)
        ghostClient_->sendAccept(*acceptParams);

    // Insert the text at the cursor position
    int caretAbs = editor_.getCaretPosition();
    document_.insertText(caretAbs, juce::String(text));
}

void HathorTab::dismissGhostCompletion()
{
    if (!ghostLogic_ || !ghostOverlay_)
        return;

    // Send reject notification
    auto rejectParams = ghostLogic_->onReject();
    if (rejectParams.has_value() && ghostClient_)
        ghostClient_->sendReject(*rejectParams);

    // Clear the ghost
    ghostOverlay_->clearGhost();
}

void HathorTab::ghostTick()
{
    if (!ghostLogic_ || !ghostLogic_->isEnabled() || !ghostClient_ || !ghostOverlay_)
        return;

    // Let the logic layer check debounce + timeout
    int64_t nowMs = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count());

    auto opt = ghostLogic_->onTimerTick(nowMs);
    if (!opt.has_value())
        return;

    // The logic returned a request + ID — send it via the client
    auto [req, requestId] = std::move(opt.value());

    // Resolve provider config from env
    auto config = lsp::GhostProviderResolver::resolve();
    if (!config.has_value())
    {
        ghostOverlay_->clearGhost();
        return;
    }

    // Rebuild the request with the resolved provider config
    const auto& ctx = ghostLogic_->currentContext();
    req = lsp::GhostCompletionLogic::buildRequest(ctx, *config);

    // Send the request via the llm-ls client
    ghostClient_->requestGhostCompletion(
        req,
        requestId,
        [this, requestId](const std::string& id,
                          const lsp::GhostCompletionResponse& resp)
        {
            if (!ghostLogic_ || !ghostOverlay_)
                return;

            int64_t responseTime = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                .count());
            auto result = ghostLogic_->onGhostResponse(id, resp, responseTime);
            if (!result.has_value() || result->isEmpty())
            {
                ghostOverlay_->clearGhost();
                return;
            }

            // Display the ghost text at the cursor position from the result
            auto ghostResult = result.value();

            juce::Rectangle<int> caretRect = editor_.getCaretRectangleForCharIndex(
                editor_.getCaretPosition());

            ghostOverlay_->setGhostText(
                ghostResult.text,
                ghostResult.cursorLine,
                ghostResult.character,
                0);

            ghostOverlay_->setTopLeftPosition(caretRect.getRight(), caretRect.getY());
            ghostOverlay_->toFront(false);
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
