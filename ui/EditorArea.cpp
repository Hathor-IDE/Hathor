// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * EditorArea.cpp — multi-tab code editor region implementation.
 *
 * Requirements: 22.1–22.3, 22.5–22.7, 23.1–23.7, 24.4
 */

#include "EditorArea.hpp"
#include "HathorFileParser.hpp"
#include "HathorLspClient.hpp"
#include "GhostLlmClient.hpp"
#include "GhostProviderConfig.hpp"
#include "EditorContextBridge.hpp"
#include "LspContextBridge.hpp"
#include "control/CompletionContextProvider.hpp"  // AI-G3: CompletionRequest
#include "hathor/LanguageMetadata.hpp"
#include "AudioEngine.hpp"

#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
#include <fstream>
#include <sstream>
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <optional>
#include <thread>
#include <filesystem>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// L-3: Conversion from lsp::Diagnostic to control::Diagnostic
// ---------------------------------------------------------------------------

static hathor::control::Diagnostic toControlDiag(
    const lsp::Diagnostic& lspDiag,
    hathor::control::DiagSource source)
{
    hathor::control::Diagnostic d;
    d.severity = static_cast<hathor::control::DiagSeverity>(
        lspDiag.severity.value_or(lsp::DiagnosticSeverity::Error));
    d.source  = source;
    d.sourceLabel = std::string(hathor::control::sourceLabel(source));
    d.code  = lspDiag.code.value_or("");
    d.message = lspDiag.message;
    d.uri = "";  // set by caller (the uri is the publish key)
    d.line = lspDiag.range.start.line + 1;    // LSP is 0-based, control is 1-based
    d.column = lspDiag.range.start.character + 1;
    d.relatedInfo = lspDiag.source.value_or("");
    return d;
}

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
     // AI-4 / AI-G7: Load language metadata for LSP fallback (AI-3) and
     // ChucK deterministic completion (AI-G7). Search multiple candidate paths
     // since the binary may be run from a build directory or bundled app.
     {
         const char* candidates[] = {
             "reference/language-metadata/HathorLanguageMetadata.json",
             "./reference/language-metadata/HathorLanguageMetadata.json",
             "../reference/language-metadata/HathorLanguageMetadata.json",
             "../../reference/language-metadata/HathorLanguageMetadata.json",
         };
         for (const char* p : candidates)
         {
             std::error_code ec;
             if (std::filesystem::exists(p, ec))
             {
                 auto result = hathor::language::loadAndValidate(p);
                 if (result.compatibility.compatible)
                 {
                     metadata_ = std::move(result.metadata);
                     metadataCompat_ = std::move(result.compatibility);
                     hathor::language::assignToConsumer(metadata_, "hathor-editor");
                     break;
                 }
                 else
                 {
                     // File was found but failed compatibility — log for debugging.
                     for (const auto& e : result.compatibility.errors)
                         std::fprintf(stderr, "AI-3 metadata compatibility error: %s\n", e.c_str());
                 }
             }
         }
         // If no compatible metadata was found after all candidates,
         // metadata_ stays empty and metadataCompat_ stays default (compatible=false).
     }

    // AI-4: Create the LSP client (manages the strudel-lsp-server process)
    // The server script is located at reference/strudel-lsp/strudel-lsp-server.cjs
    lspClient_ = std::make_unique<HathorLspClient>(
        "reference/strudel-lsp/strudel-lsp-server.cjs",
        "/usr/local/bin/node",
        &metadata_,
        &metadataCompat_);

    // Wire diagnostics callback — forward to: (1) the active tab for
    // squiggly underlines, (2) the AI-8 LSP context bridge, and (3) the
    // L-3 unified DiagnosticRegistry for the Problems panel / StatusRibbon.
    lspClient_->setDiagnosticsCallback([this](const std::string& uri,
                                               const std::vector<lsp::Diagnostic>& diags)
    {
        // L-3: Publish to the unified diagnostic store
        std::vector<hathor::control::Diagnostic> controlDiags;
        controlDiags.reserve(diags.size());
        for (const auto& ld : diags)
        {
            auto cd = toControlDiag(ld, hathor::control::DiagSource::StrudelLsp);
            cd.uri = uri;
            controlDiags.push_back(std::move(cd));
        }
        diagnosticRegistry_->setDiagnostics(hathor::control::DiagSource::StrudelLsp, uri, controlDiags);

        for (auto& tab : tabs_)
        {
            if (tab && !tab->isChuckTab())
            {
                juce::String tabUri = tab->lspDocumentUri();
                if (tabUri.toStdString() == uri)
                {
                    tab->notifyLspDiagnostics(uri, diags);
                    break;
                }
            }
        }
        // AI-8: Forward to the LSP context bridge so get-context can include
        // LSP-derived diagnostics in the authoring context payload.
        if (lspContextBridge_ != nullptr)
            lspContextBridge_->setLspDiagnostics(uri, diags);
    });

    lspClient_->start();

    // AI-4: Create the ghost-text client (manages the llm-ls process).
    // Only started if GHOST_ENABLED=true — the client checks this internally.
    ghostClient_ = std::make_unique<GhostLlmClient>("reference/llm-ls/llm-ls");
    ghostClient_->start();

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

    // -----------------------------------------------------------------------
    // L-1: Create editor ergonomics components
    // -----------------------------------------------------------------------
    actionRegistry_ = std::make_unique<ActionRegistry>();
    findReplacePanel_ = std::make_unique<FindReplacePanel>();
    commandPalette_ = std::make_unique<CommandPalette>();
    commandPalette_->setActionRegistry(actionRegistry_.get());
    breadcrumbsBar_ = std::make_unique<BreadcrumbsBar>();
    editorSplitSurface_ = std::make_unique<EditorSplitSurface>(audio_, ci_);

    // Add L-1 components to EditorArea's hierarchy (hidden by default)
    addChildComponent(findReplacePanel_.get());
    addChildComponent(commandPalette_.get());
    findReplacePanel_->setVisible(false);
    commandPalette_->setVisible(false);

    // -----------------------------------------------------------------------
    // L-2: Create navigation & workspace search components
    // -----------------------------------------------------------------------
    navigationHistory_ = std::make_unique<NavigationHistory>();
    workspaceSearchModel_ = std::make_unique<WorkspaceSearchModel>(workspaceRoot_);
    symbolSearchModel_ = std::make_unique<SymbolSearchModel>(&metadata_);

    quickOpenDialog_ = std::make_unique<QuickOpenDialog>(workspaceRoot_);
    quickOpenDialog_->onFileSelected = [this](const std::filesystem::path& file) {
        openFile(juce::File(file.string()));
    };
    quickOpenDialog_->onCancelled = [this]() {
        quickOpenDialog_->setVisible(false);
    };

    workspaceSearchPanel_ = std::make_unique<WorkspaceSearchPanel>(workspaceRoot_, workspaceSearchModel_.get());
    workspaceSearchPanel_->onNavigateToMatch = [this](const std::filesystem::path& file, int line, int column) {
        openFile(juce::File(file.string()));
        if (auto* tab = activeTab())
        {
            juce::CodeDocument::Position pos(tab->document(), line, column);
            tab->editor().moveCaretTo(pos, false);
        }
    };
    workspaceSearchPanel_->onClosePanel = [this]() {
        workspaceSearchPanel_->setVisible(false);
    };

    symbolSearchPanel_ = std::make_unique<SymbolSearchPanel>(symbolSearchModel_.get());
    symbolSearchPanel_->onSymbolSelected = [this](const SymbolSearchResult& result) {
        if (!result.isBuiltin && !result.filePath.empty())
        {
            openFile(juce::File(result.filePath.string()));
            if (auto* tab = activeTab())
            {
                juce::CodeDocument::Position pos(tab->document(), result.line, result.column);
                tab->editor().moveCaretTo(pos, false);
            }
        }
    };
    symbolSearchPanel_->onClosePanel = [this]() {
        symbolSearchPanel_->setVisible(false);
    };

    // =======================================================================
    // L-3: Unified Problems / Diagnostics surface
    // =======================================================================

    diagnosticRegistry_ = std::make_unique<hathor::control::DiagnosticRegistry>();

    problemsPanel_ = std::make_unique<ProblemsPanel>(diagnosticRegistry_.get());
    problemsPanel_->onDiagnosticSelected = [this](const std::string& uri, int line, int column) {
        // Navigate to the diagnostic's source location via the existing
        // L-2 editor navigation — open the file and move the caret.
        // Diagnostic line/column are 1-based; CodeDocument::Position is 0-based.
        std::string path = uri;
        const std::string prefix = "file://";
        if (path.substr(0, prefix.size()) == prefix)
            path = path.substr(prefix.size());

        juce::File file(path);
        openFile(file);

        if (auto* tab = activeTab())
        {
            juce::CodeDocument::Position pos(tab->document(), line - 1, column - 1);
            tab->editor().moveCaretTo(pos, false);

            // Record in navigation history (L-2 integration)
            navigationHistory_->navigateTo({uri, line - 1, column - 1});
        }
    };
    problemsPanel_->onClosePanel = [this]() {
        problemsPanel_->setVisible(false);
    };

    addChildComponent(quickOpenDialog_.get());
    addChildComponent(workspaceSearchPanel_.get());
    addChildComponent(symbolSearchPanel_.get());
    addChildComponent(problemsPanel_.get());
    quickOpenDialog_->setVisible(false);
    workspaceSearchPanel_->setVisible(false);
    symbolSearchPanel_->setVisible(false);
    problemsPanel_->setVisible(false);

    // L-4: Simple integrated terminal panel (bottom-docked, like ProblemsPanel).
    // Created with the workspace root (project dir) so tasks like build/test
    // resolve against the correct project. The task runner's buildDir is
    // "<projectDir>/build" by default.
    terminalPanel_ = std::make_unique<TerminalPanel>(
        workspaceRoot_.empty() ? "." : workspaceRoot_.string());
    addChildComponent(terminalPanel_.get());
    terminalPanel_->setVisible(false);

    // L-5: Source control panel (bottom-docked, like ProblemsPanel/TerminalPanel).
    // Initialized with the workspace root (project dir) so Git operations
    // execute against the correct repository.
    sourceControlPanel_ = std::make_unique<SourceControlPanel>(
        workspaceRoot_.empty() ? "." : workspaceRoot_.string());
    addChildComponent(sourceControlPanel_.get());
    sourceControlPanel_->setVisible(false);

    // L-6: Debug & Runtime Inspector panel (bottom-docked, two tabs:
    // "Debugger" for native C++ debugging, "Runtime" for Hathor runtime
    // inspection).  Reads deterministic state via the read-only facade and
    // the L-3 registry — opening it never mutates audio/ChucK state.
    debugPanel_ = std::make_unique<DebugPanel>(audio_, diagnosticRegistry_.get());
    debugPanel_->onClosePanel = [this]() {
        debugPanel_->setVisible(false);
    };
    // L-6 ↔ L-3: opening Problems from the runtime inspector reuses the
    // existing L-3 Problems surface (single diagnostics authority).
    debugPanel_->onOpenProblems = [this]() {
        hideTerminalPanel();
        hideSourceControlPanel();
        showProblemsPanel();
        resized();
    };
    addChildComponent(debugPanel_.get());
    debugPanel_->setVisible(false);

    // -----------------------------------------------------------------------
    // Phase G / D1: Petdex manifest service.
    // App-lifetime, but strictly lazy: the service performs no network or
    // cache work until SettingsComponent (opened only by explicit user
    // action) calls start(). Starting Hathor never downloads anything.
    // -----------------------------------------------------------------------
    petdexService_ = std::make_unique<PetdexManifestService>(
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Hathor/Petdex"));

    // StatusRibbon is mounted by MainWindow at the bottom of the window;
    // do NOT addChildComponent here — it stays parented to MainWindow.
}

EditorArea::~EditorArea()
{
    // Stop the ghost-text client before destroying tabs.
    if (ghostClient_)
        ghostClient_->stop();

    // Hide all tab components before deletion to avoid dangling paint calls.
    for (auto& t : tabs_)
        t->setVisible(false);

    delete statusClearTimer_;
}

// ---------------------------------------------------------------------------
// AI-8: Context bridge accessors
// ---------------------------------------------------------------------------

void EditorArea::setEditorContextBridge(EditorContextBridge* bridge) noexcept
{
    editorContextBridge_ = bridge;
    if (bridge != nullptr)
        bridge->refresh();
}

void EditorArea::setLspContextBridge(LspContextBridge* bridge) noexcept
{
    lspContextBridge_ = bridge;
    if (bridge != nullptr && lspClient_ != nullptr)
        bridge->setLspClient(lspClient_.get());
}

HathorLspClient* EditorArea::lspClient() const noexcept
{
    return lspClient_.get();
}

const hathor::language::LanguageMetadata& EditorArea::metadata() const noexcept
{
    return metadata_;
}

const hathor::language::MetadataCompatibility& EditorArea::metadataCompatibility() const noexcept
{
    return metadataCompat_;
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
    wireContextMenuCallbacks(*tab);
    installKeyListenerForTab(*tab);

     // AI-4: Install LSP client on the tab (for .hathor tabs)
     tab->installLspClient(lspClient_.get());
     tab->notifyLspDidOpen();

      // AI-4: Install ghost-text client on the tab (for both .hathor and .ck)
      tab->installGhostClient(ghostClient_.get());

       // AI-G3: Wire the Hathor-specific authoring-context callback for llm-ls FIM.
       // The callback assembles a compact, location-aware, bounded context
       // (AI-G3) reusing AI-8's providers + AI-3 metadata + AI-4 LSP, and injects
       // it as additional FIM context (fim.prefix) for the ghost completion.
       // Capture a raw pointer (tab is owned by tabs_, pushed below).
       HathorTab* tabPtr = tab.get();
       tab->getAuthoringContext = [this, tabPtr]() -> nlohmann::json {
           auto caretPos = tabPtr->editor().getCaretPos();
           hathor::control::CompletionRequest req;
           req.file = tabPtr->lspDocumentUri().toStdString();
           req.uri = tabPtr->lspDocumentUri().toStdString();
           req.line = caretPos.getLineNumber();
           req.character = caretPos.getIndexInLine();
           req.language = tabPtr->isChuckTab() ? "chuck" : "mininotation";
           req.documentText = tabPtr->document().getAllContent().toStdString();
           // Selection (selection-aware retrieval — AI-G3)
           const auto region = tabPtr->editor().getHighlightedRegion();
           if (!region.isEmpty())
           {
               const auto selStart = tabPtr->editor().getSelectionStart();
               const auto selEnd   = tabPtr->editor().getSelectionEnd();
               req.selection = hathor::control::CompletionRequest::Range{
                   selStart.getLineNumber(), selStart.getIndexInLine(),
                   selEnd.getLineNumber(),   selEnd.getIndexInLine()};
               req.selectedText = tabPtr->editor().getTextInRange(region).toStdString();
           }
           return ci_.assembleCompletionContext(req);
       };

      addAndMakeVisible(*tab);
      tabs_.push_back(std::move(tab));

     activateTab(static_cast<int>(tabs_.size()) - 1);

     // A5 — SongChuck eval wiring: auto-evaluate .ck files on open so that
     // a single Explorer click performs open + ckEval, mirroring .hathor's
     // Ctrl+Enter eval surface (same evalCkOnWorkerThread path).
     if (auto* newTab = tabs_.back().get(); newTab->isChuckTab())
         triggerChuckEval(newTab);

     return true;
 }

bool EditorArea::openFile(const juce::File& file)
{
    // Guard: verify the file exists before attempting to open it.
    if (!file.existsAsFile())
    {
        showStatus("Error: file not found: " + file.getFileName());
        return false;
    }

    // Focus existing tab if the file is already open.
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i)
    {
        const auto& fp = tabs_[static_cast<std::size_t>(i)]->filePath();
        if (fp.has_value() && *fp == file)
        {
            activateTab(i);
            // A5 — SongChuck eval wiring: re-evaluate .ck files on re-open.
            if (auto* t = tabs_[static_cast<std::size_t>(i)].get(); t->isChuckTab())
                triggerChuckEval(t);
            return true;
        }
    }

    // Parse the file to extract front matter + body.
    const juce::String contents = file.loadFileAsString();
    const auto parseResult = parseHathorFile(contents.toStdString());

    std::optional<std::string> frontLabel;
    std::optional<std::string> frontSlot;
    std::optional<FrontMatter> frontMatter;
    juce::String bodyText = contents;  // default: full contents if parse fails or no front matter

    if (const auto* hf = std::get_if<HathorFile>(&parseResult))
    {
        frontLabel = hf->front.label;
        frontSlot  = hf->front.slot;
        frontMatter = hf->front;
        bodyText = juce::String(hf->body);
    }
    else if (const auto* err = std::get_if<ParseFileError>(&parseResult)) {
        // Front matter is malformed — show a warning but still load the body
        // (parser returns the full contents as body on malformed front matter).
        showStatus("Warning: malformed front matter in " + file.getFileName()
                   + " at line " + juce::String(err->line) + ": " + err->message);
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
    if (frontMatter.has_value())
        tab->setFrontMatter(*frontMatter);

    // Populate the code document with ONLY the mini-notation body.
    // Front matter is stored separately on the tab, not in the editable document.
    tab->document().replaceAllContent(bodyText);

    // File was just loaded — clear the unsaved dot (it would have been set
    // by replaceAllContent triggering the CodeDocument listener).
    tab->clearUnsavedDot();

    wireUnsavedCallback(*tab);
    wirePlayStopCallback(*tab);
    installKeyListenerForTab(*tab);

     // AI-4: Install LSP client on the tab (for .hathor tabs)
     tab->installLspClient(lspClient_.get());
     tab->notifyLspDidOpen();

       // AI-4: Install ghost-text client on the tab (for both .hathor and .ck)
       tab->installGhostClient(ghostClient_.get());

       // AI-G3: Wire the Hathor-specific authoring-context callback for llm-ls FIM.
       // Capture a raw pointer (tab is owned by tabs_, pushed below).
       HathorTab* tabPtr = tab.get();
       tab->getAuthoringContext = [this, tabPtr]() -> nlohmann::json {
           auto caretPos = tabPtr->editor().getCaretPos();
           hathor::control::CompletionRequest req;
           req.file = tabPtr->lspDocumentUri().toStdString();
           req.uri = tabPtr->lspDocumentUri().toStdString();
           req.line = caretPos.getLineNumber();
           req.character = caretPos.getIndexInLine();
           req.language = tabPtr->isChuckTab() ? "chuck" : "mininotation";
           req.documentText = tabPtr->document().getAllContent().toStdString();
           const auto region = tabPtr->editor().getHighlightedRegion();
           if (!region.isEmpty())
           {
               const auto selStart = tabPtr->editor().getSelectionStart();
               const auto selEnd   = tabPtr->editor().getSelectionEnd();
               req.selection = hathor::control::CompletionRequest::Range{
                   selStart.getLineNumber(), selStart.getIndexInLine(),
                   selEnd.getLineNumber(),   selEnd.getIndexInLine()};
               req.selectedText = tabPtr->editor().getTextInRange(region).toStdString();
           }
           return ci_.assembleCompletionContext(req);
       };

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
                         const juce::File& f = *t->filePath();
                         if (ChuckTokeniser::isChuckFile(f))
                         {
                             // .ck files: write raw content (no front matter).
                             f.replaceWithText(t->document().getAllContent());
                         }
                         else
                         {
                             // .hathor files: serialize via serialiseHathorFile().
                             HathorFile hf;
                             if (t->frontMatter().has_value())
                                 hf.front = *t->frontMatter();
                             hf.body = t->document().getAllContent().toStdString();
                             const std::string serialized = serialiseHathorFile(hf);
                             f.replaceWithText(juce::String(serialized));
                         }
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
                                     if (ChuckTokeniser::isChuckFile(chosen))
                                     {
                                         chosen.replaceWithText(
                                             tabs_[static_cast<std::size_t>(index)]
                                                 ->document().getAllContent());
                                     }
                                     else
                                     {
                                         HathorTab* t = tabs_[static_cast<std::size_t>(index)].get();
                                         HathorFile hf;
                                         if (t->frontMatter().has_value())
                                             hf.front = *t->frontMatter();
                                         hf.body = t->document().getAllContent().toStdString();
                                         const std::string serialized = serialiseHathorFile(hf);
                                         chosen.replaceWithText(juce::String(serialized));
                                     }
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

    // Create the Settings tab (A2). The app-lifetime Petdex service is passed
    // so the Petdex section can browse/select the catalog (Phase G / D1).
    settingsTab_ = std::make_unique<SettingsComponent>(props, &audio_,
                                                       petdexService_.get());
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

bool EditorArea::isLspConnected() const noexcept
{
    return lspClient_ != nullptr && lspClient_->isRunning();
}

// ---------------------------------------------------------------------------
// juce::Component overrides
// ---------------------------------------------------------------------------

void EditorArea::resized()
{
    auto b = getLocalBounds();

    // L-1: Breadcrumbs bar at the very top
    if (breadcrumbsBar_)
        breadcrumbsBar_->setVisible(true);  // always visible
    if (breadcrumbsBar_)
    {
        auto crumbArea = b.removeFromTop(BreadcrumbsBar::kBarHeight);
        breadcrumbsBar_->setBounds(crumbArea);
    }

    // Tab bar below breadcrumbs
    tabBar_.setBounds(b.removeFromTop(kTabBarHeight));

    // Status bar at the bottom
    statusBar_.setBounds(b.removeFromBottom(kStatusBarHeight));

    // L-1: Find/replace panel at the bottom (above status bar)
    if (findReplacePanel_ && findReplacePanel_->isVisible())
    {
        auto findArea = b.removeFromBottom(FindReplacePanel::kPanelHeight);
        findReplacePanel_->setBounds(findArea);
    }

    // L-2: Workspace search panel at the bottom (if visible)
    if (workspaceSearchPanel_ && workspaceSearchPanel_->isVisible())
    {
        auto wsArea = b.removeFromBottom(WorkspaceSearchPanel::kPanelHeight);
        workspaceSearchPanel_->setBounds(wsArea);
    }

    // L-2: Symbol search panel at the bottom (if visible)
    if (symbolSearchPanel_ && symbolSearchPanel_->isVisible())
    {
        auto ssArea = b.removeFromBottom(SymbolSearchPanel::kPanelHeight);
        symbolSearchPanel_->setBounds(ssArea);
    }

    // L-3: Problems panel at the bottom (if visible)
    if (problemsPanel_ && problemsPanel_->isVisible())
    {
        auto probArea = b.removeFromBottom(ProblemsPanel::kPanelHeight);
        problemsPanel_->setBounds(probArea);
    }

    // L-4: Terminal panel at the bottom (if visible)
    if (terminalPanel_ && terminalPanel_->isVisible())
    {
        auto termArea = b.removeFromBottom(TerminalPanel::kPanelHeight);
        terminalPanel_->setBounds(termArea);
    }

    // L-5: Source control panel at the bottom (if visible)
    if (sourceControlPanel_ && sourceControlPanel_->isVisible())
    {
        auto gitArea = b.removeFromBottom(SourceControlPanel::kPanelHeight);
        sourceControlPanel_->setBounds(gitArea);
    }

    // L-6: Debug & Runtime Inspector panel at the bottom (if visible)
    if (debugPanel_ && debugPanel_->isVisible())
    {
        auto debugArea = b.removeFromBottom(DebugPanel::kPanelHeight);
        debugPanel_->setBounds(debugArea);
    }

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

     // AI-8: Refresh the editor context snapshot for the newly activated tab.
     if (editorContextBridge_ != nullptr)
         editorContextBridge_->refresh();

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

     // AI-4: Notify LSP/ghost that the document is closing.
     // Now handles both .hathor and .ck tabs (notifyLspDidClose
     // routes internally based on useChuckTokeniser_).
     HathorTab* closingTab = tabs_[static_cast<std::size_t>(index)].get();
     if (closingTab && lspClient_)
         closingTab->notifyLspDidClose();

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
        // AI-8: Refresh the editor context snapshot after any document edit.
        if (editorContextBridge_ != nullptr)
            editorContextBridge_->refresh();
    };

     // AI-8: Refresh the editor context snapshot when the cursor moves.
    tab.onCursorMoved = [this]()
    {
        if (editorContextBridge_ != nullptr)
            editorContextBridge_->refresh();
    };

    // Forward ChucK compiler diagnostics to: (1) the AI-8 LSP context bridge
    // for .ck file authoring context, and (2) the L-3 unified DiagnosticRegistry
    // for the Problems panel / StatusRibbon.
    tab.onChuckDiagnostics = [this](const std::string& uri,
                                    const std::vector<lsp::Diagnostic>& diags)
    {
        // L-3: Publish to the unified diagnostic store
        std::vector<hathor::control::Diagnostic> controlDiags;
        controlDiags.reserve(diags.size());
        for (const auto& ld : diags)
        {
            auto cd = toControlDiag(ld, hathor::control::DiagSource::ChuckCompiler);
            cd.uri = uri;
            controlDiags.push_back(std::move(cd));
        }
        diagnosticRegistry_->setDiagnostics(hathor::control::DiagSource::ChuckCompiler, uri, controlDiags);

        if (lspContextBridge_ != nullptr)
            lspContextBridge_->setLspDiagnostics(uri, diags);
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

void EditorArea::wireContextMenuCallbacks(HathorTab& tab)
{
    tab.onShowFindPanel = [this]() {
        showFindReplace();
    };
    tab.onShowReplacePanel = [this]() {
        showFindReplace();
    };
    tab.onGoToLine = [this]() {
        HathorTab* tab = activeTab();
        if (!tab)
            return;

        auto* ed = &tab->editor();
        const int currentLine = ed->getCaretPos().getLineNumber() + 1;
        const int totalLines = tab->document().getNumLines();
        const juce::String prompt = "Line number (1–" + juce::String(totalLines) + "):";

        auto alert = std::make_shared<juce::AlertWindow>(
            juce::MessageBoxIconType::QuestionIcon,
            "Go to Line",
            prompt);
        alert->addButton("OK", 1);
        alert->addButton("Cancel", 0);
        alert->addTextBox("lineNumber", juce::String(currentLine));

        alert->enterModalState(true,
            juce::ModalCallbackFunction::create([alert, ed, totalLines](int result) {
                if (result == 1)
                {
                    const int lineNum = juce::Colours::fromString;
                    const juce::String input = alert->getTextBoxInput("lineNumber");
                    const int n = input.getIntValue();
                    if (n >= 1 && n <= totalLines)
                    {
                        juce::CodeDocument::Position pos(ed->getDocument(), n - 1, 0);
                        ed->moveCaretTo(pos, false);
                        ed->scrollToKeepCaretVisible();
                    }
                }
                alert = nullptr;
            }),
            false);
    };
    tab.onToggleComment = [&tab]() {
        // Toggle line comment — use // prefix for .hathor and .ck
        auto& doc = tab.document();
        auto caret = tab.editor().getCaretPos();
        int lineNum = caret.getLineNumber();
        juce::String line = doc.getLine(lineNum);
        juce::String trimmed = line.trim();
        if (trimmed.startsWith("//"))
        {
            // Uncomment — remove "//" after leading whitespace
            int indent = 0;
            while (indent < line.length() && (line[indent] == ' ' || line[indent] == '\t'))
                ++indent;
            juce::CodeDocument::Position lineStartPos(doc, lineNum, 0);
            doc.deleteSection(lineStartPos.getPosition() + indent,
                              lineStartPos.getPosition() + indent + 2);
        }
        else
        {
            // Comment — insert "//" after leading whitespace
            int indent = 0;
            while (indent < line.length() && (line[indent] == ' ' || line[indent] == '\t'))
                ++indent;
            juce::CodeDocument::Position lineStartPos(doc, lineNum, 0);
            doc.insertText(lineStartPos.getPosition() + indent, juce::String("//"));
        }
    };
    tab.onDuplicateLine = [&tab]() {
        auto& ed = tab.editor();
        auto& doc = tab.document();
        auto caret = ed.getCaretPos();
        int lineNum = caret.getLineNumber();
        juce::CodeDocument::Position lineStart(doc, lineNum, 0);
        juce::CodeDocument::Position lineEnd(doc, lineNum + 1, 0);
        juce::String lineText = doc.getTextBetween(lineStart, lineEnd);
        doc.insertText(lineEnd.getPosition(), lineText + "\n");
    };
    tab.onEvalLine = [this, &tab]() {
        if (tab.isChuckTab())
            evalOnWorkerThread(&tab, "slot" + std::to_string(tab.slotIndex()), tab.document().getAllContent());
        else
            bakeActiveTab();
    };
    tab.onEvalBlock = [this, &tab]() {
        // Eval selected text (or current line if no selection)
        juce::String text = tab.editor().getTextInRange(tab.editor().getHighlightedRegion());
        if (text.isEmpty())
            text = tab.document().getLine(tab.editor().getCaretPos().getLineNumber());
        if (tab.isChuckTab())
            evalOnWorkerThread(&tab, "slot" + std::to_string(tab.slotIndex()), text);
        else
            bakeActiveTab();
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

void EditorArea::ghostTick()
{
    // Tick every HathorTab's ghost-text lifecycle so that debounce timers
    // and latency timeouts fire on schedule. Each tab early-returns if
    // it has no ghost client or ghost text is disabled.
    for (const auto& t : tabs_)
    {
        t->ghostTick();
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

// ---------------------------------------------------------------------------
// A5: triggerChuckEval — guard + dispatch for .ck auto-evaluation on open
// ---------------------------------------------------------------------------

void EditorArea::triggerChuckEval(HathorTab* tab)
{
    // Guard: the audio worker must be running before we can evaluate.
    if (!audio_.hasWorker())
    {
        showStatus("ChucK runtime unavailable — cannot evaluate .ck file.");
        tab->setCkEvalState(HathorTab::CkevalState::Error);
        return;
    }

    // Dispatch via the existing Ctrl+Enter path (same evalCkOnWorkerThread).
    const juce::String code = tab->document().getAllContent();
    evalCkOnWorkerThread(tab, code);
}

// ---------------------------------------------------------------------------
// J-6: Telemetry persistence
// ---------------------------------------------------------------------------

#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
bool EditorArea::saveTelemetry(const std::string& filePath) const
{
    nlohmann::json allData;
    allData["tabs"] = nlohmann::json::array();

    for (const auto& tab : tabs_)
    {
        auto* telemetry = tab->ghostTelemetry();
        if (telemetry == nullptr)
            continue;

        nlohmann::json tabData;
        tabData["uri"] = tab->lspDocumentUri().toStdString();
        tabData["languageId"] = tab->isChuckTab() ? "chuck" : "hathor";
        tabData["telemetryJson"] = telemetry->toJson();
        allData["tabs"].push_back(tabData);
    }

    std::ofstream file(filePath, std::ios::trunc | std::ios::out);
    if (!file.is_open())
        return false;

    file << allData.dump(2);
    return file.good();
}

void EditorArea::loadTelemetry(const std::string& filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open())
        return;

    std::stringstream ss;
    ss << file.rdbuf();

    try
    {
        auto allData = nlohmann::json::parse(ss.str());
        if (!allData.contains("tabs") || !allData["tabs"].is_array())
            return;

        // Build a lookup of tab URI → HathorTab for efficient matching.
        for (const auto& tabData : allData["tabs"])
        {
            if (!tabData.contains("uri") || !tabData.contains("telemetryJson"))
                continue;

            std::string uri = tabData["uri"].get<std::string>();

            // Find the matching tab by URI.
            for (auto& tab : tabs_)
            {
                if (tab->lspDocumentUri().toStdString() == uri)
                {
                    auto* telemetry = tab->ghostTelemetry();
                    if (telemetry)
                        telemetry->loadFromJson(tabData["telemetryJson"].get<std::string>());
                    break;
                }
            }
        }
    }
    catch (const nlohmann::json::exception&)
    {
        // Silently ignore malformed telemetry files.
    }
}
#endif

// ---------------------------------------------------------------------------
// L-1: Editor ergonomics — find/replace, split, breadcrumbs
// ---------------------------------------------------------------------------

void EditorArea::showFindReplace()
{
    if (!findReplacePanel_)
        findReplacePanel_ = std::make_unique<FindReplacePanel>();
    findReplacePanel_->setVisible(true);
    findReplacePanel_->toFront(true);
    HathorTab* tab = activeTab();
    if (tab)
        findReplacePanel_->setTargetEditor(&tab->editor(), &tab->document());
    resized();
}

void EditorArea::hideFindReplace()
{
    if (findReplacePanel_)
        findReplacePanel_->setVisible(false);
}

void EditorArea::findNextInActiveTab()
{
    if (!activeTab() || !findReplacePanel_)
        return;
    findReplacePanel_->setTargetEditor(&activeTab()->editor(), &activeTab()->document());
}

void EditorArea::findPrevInActiveTab()
{
    if (!activeTab() || !findReplacePanel_)
        return;
    findReplacePanel_->setTargetEditor(&activeTab()->editor(), &activeTab()->document());
}

void EditorArea::replaceInActiveTab()
{
    // Single replace is handled by the FindReplacePanel callback
}

void EditorArea::replaceAllInActiveTab()
{
    // Replace all is handled by the FindReplacePanel callback
}

void EditorArea::toggleSplit()
{
    if (!editorSplitSurface_)
    {
    editorSplitSurface_ = std::make_unique<EditorSplitSurface>(audio_, ci_);
        addChildComponent(editorSplitSurface_.get());
        // Hide the original EditorArea content area when split is active
    }
    else
    {
        editorSplitSurface_->setVisible(!editorSplitSurface_->isVisible());
    }
}

// ---------------------------------------------------------------------------
// L-2: Navigation & Workspace Search
// ---------------------------------------------------------------------------

void EditorArea::setWorkspaceRoot(const std::filesystem::path& root)
{
    workspaceRoot_ = root;
    if (workspaceSearchModel_)
        workspaceSearchModel_ = std::make_unique<WorkspaceSearchModel>(root);
    if (quickOpenDialog_)
    {
        // Rebuild the quick-open file list with the new root
        quickOpenDialog_.reset();
        quickOpenDialog_ = std::make_unique<QuickOpenDialog>(root);
        quickOpenDialog_->onFileSelected = [this](const std::filesystem::path& file) {
            openFile(juce::File(file.string()));
        };
        quickOpenDialog_->onCancelled = [this]() {
            quickOpenDialog_->setVisible(false);
        };
        addChildComponent(quickOpenDialog_.get());
        quickOpenDialog_->setVisible(false);
    }
}

void EditorArea::showQuickOpen()
{
    if (!quickOpenDialog_)
        return;

    juce::Component* topParent = getTopLevelComponent();
    if (!topParent)
        topParent = this;

    auto bounds = topParent->getBounds();
    quickOpenDialog_->setBounds(bounds);
    topParent->addAndMakeVisible(quickOpenDialog_.get());
    quickOpenDialog_->setVisible(true);
    quickOpenDialog_->setFilter({});
}

void EditorArea::gotoDefinition()
{
    HathorTab* tab = activeTab();
    if (!tab || !lspClient_ || tab->isChuckTab())
        return;

    auto cursorPos = tab->editor().getCaretPos();
    juce::String uri = tab->lspDocumentUri();
    int line = cursorPos.getLineNumber();
    int column = cursorPos.getIndexInLine();

    // Record current position in navigation history
    navigationHistory_->navigateTo({
        uri.toStdString(),
        line,
        column
    });

    lspClient_->requestDefinition(
        uri.toStdString(), line, column,
        [this](const lsp::NavigationResult& result) {
            if (!result.locations.empty())
            {
                const auto& loc = result.locations.front();
                std::string path = loc.uri;
                const std::string prefix = "file://";
                if (path.substr(0, prefix.size()) == prefix)
                    path = path.substr(prefix.size());

                juce::File file(path);
                openFile(file);

                if (auto* targetTab = activeTab())
                {
                    juce::CodeDocument::Position targetPos(targetTab->document(),
                        static_cast<int>(loc.range.start.line),
                        static_cast<int>(loc.range.start.character));
                    targetTab->editor().moveCaretTo(targetPos, false);

                    navigationHistory_->navigateTo({
                        loc.uri,
                        static_cast<int>(loc.range.start.line),
                        static_cast<int>(loc.range.start.character)
                    });
                }
            }
            else
            {
                showStatus("No definition found");
            }
        });
}

void EditorArea::gotoReferences()
{
    HathorTab* tab = activeTab();
    if (!tab || !lspClient_ || tab->isChuckTab())
        return;

    auto cursorPos = tab->editor().getCaretPos();
    juce::String uri = tab->lspDocumentUri();
    int line = cursorPos.getLineNumber();
    int column = cursorPos.getIndexInLine();

    navigationHistory_->navigateTo({uri.toStdString(), line, column});

    lspClient_->requestReferences(
        uri.toStdString(), line, column, true,
        [this](const lsp::NavigationResult& result) {
            if (result.locations.empty())
            {
                showStatus("No references found");
            }
            else
            {
                const auto& loc = result.locations.front();
                std::string path = loc.uri;
                const std::string prefix = "file://";
                if (path.substr(0, prefix.size()) == prefix)
                    path = path.substr(prefix.size());

                juce::File file(path);
                openFile(file);

                if (auto* targetTab = activeTab())
                {
                    juce::CodeDocument::Position targetPos(targetTab->document(),
                        static_cast<int>(loc.range.start.line),
                        static_cast<int>(loc.range.start.character));
                    targetTab->editor().moveCaretTo(targetPos, false);

                    navigationHistory_->navigateTo({
                        loc.uri,
                        static_cast<int>(loc.range.start.line),
                        static_cast<int>(loc.range.start.character)
                    });
                }
            }
        });
}

void EditorArea::peekDefinition()
{
    showStatus("Peek definition not yet implemented");
}

void EditorArea::navigateBack()
{
    auto entry = navigationHistory_->goBack();
    if (!entry.has_value())
    {
        showStatus("No previous location in history");
        return;
    }

    std::string path = entry->uri;
    const std::string prefix = "file://";
    if (path.substr(0, prefix.size()) == prefix)
        path = path.substr(prefix.size());

    juce::File file(path);
    openFile(file);

    if (auto* tab = activeTab())
    {
        juce::CodeDocument::Position pos(tab->document(), entry->line, entry->column);
        tab->editor().moveCaretTo(pos, false);
    }
}

void EditorArea::navigateForward()
{
    auto entry = navigationHistory_->goForward();
    if (!entry.has_value())
    {
        showStatus("No next location in history");
        return;
    }

    std::string path = entry->uri;
    const std::string prefix = "file://";
    if (path.substr(0, prefix.size()) == prefix)
        path = path.substr(prefix.size());

    juce::File file(path);
    openFile(file);

    if (auto* tab = activeTab())
    {
        juce::CodeDocument::Position pos(tab->document(), entry->line, entry->column);
        tab->editor().moveCaretTo(pos, false);
    }
}

void EditorArea::showWorkspaceSearch()
{
    if (!workspaceSearchPanel_)
        return;

    workspaceSearchPanel_->setVisible(true);
    workspaceSearchPanel_->toFront(true);
}

void EditorArea::showSymbolSearch()
{
    if (!symbolSearchPanel_)
        return;

    symbolSearchPanel_->setVisible(true);
    symbolSearchPanel_->toFront(true);
    symbolSearchPanel_->setQuery({});
}

void EditorArea::showSearchPanel()
{
    // Display the workspace search UI (Agent 2.4: Panel::Search wiring).
    // showWorkspaceSearch() brings the panel to the front and preserves the
    // current query/results state (no reset).
    showWorkspaceSearch();

    // Hide other bottom-docked panels, consistent with showProblemsPanel() and
    // showTerminalPanel(), so only one bottom panel is active at a time.
    if (symbolSearchPanel_)
        symbolSearchPanel_->setVisible(false);
    if (problemsPanel_)
        problemsPanel_->setVisible(false);
    if (terminalPanel_)
        terminalPanel_->setVisible(false);
    if (sourceControlPanel_)
        sourceControlPanel_->setVisible(false);
    if (debugPanel_)
        debugPanel_->setVisible(false);
}

void EditorArea::hideSearchPanel()
{
    if (workspaceSearchPanel_)
        workspaceSearchPanel_->setVisible(false);
    if (symbolSearchPanel_)
        symbolSearchPanel_->setVisible(false);
}

void EditorArea::showDocumentSymbols()
{
    HathorTab* tab = activeTab();
    if (!tab)
        return;

    // For .hathor tabs, use the LSP documentSymbol
    if (!tab->isChuckTab() && lspClient_)
    {
        juce::String uri = tab->lspDocumentUri();
        lspClient_->requestDocumentSymbols(
            uri.toStdString(),
            [this](const lsp::DocumentSymbolResult& result) {
                // Display symbols — for now, show count in status bar
                if (result.symbols.empty())
                {
                    showStatus("No symbols found in document");
                }
                else
                {
                    showStatus(juce::String(result.symbols.size()) + " symbols found");
                }
            });
    }
    else
    {
        showStatus("Document symbol search is only available for .hathor tabs");
    }
}

void EditorArea::navigateToNextDiagnostic()
{
    auto* tab = activeTab();
    if (!tab)
    {
        showStatus("No active editor");
        return;
    }

    const std::string uri = tab->lspDocumentUri().toStdString();
    auto diagnostics = diagnosticRegistry_->diagnosticsForUri(uri);
    if (diagnostics.empty())
    {
        showStatus("No diagnostics in this editor");
        return;
    }

    // Current cursor position (1-based)
    const int curLine = tab->editor().getCaretPos().getLineNumber() + 1;

    // Find the first diagnostic at or after the current line
    const hathor::control::Diagnostic* next = nullptr;
    for (const auto& d : diagnostics)
    {
        if (d.line >= curLine)
        {
            next = &d;
            break;
        }
    }
    // If none at or after, wrap to the first diagnostic
    if (!next)
    {
        next = &diagnostics.front();
    }

    juce::CodeDocument::Position pos(tab->document(), next->line - 1, next->column - 1);
    tab->editor().moveCaretTo(pos, false);
    navigationHistory_->navigateTo({uri, next->line - 1, next->column - 1});
    showStatus("Navigated to: " + juce::String(next->message));
}

void EditorArea::navigateToPrevDiagnostic()
{
    auto* tab = activeTab();
    if (!tab)
    {
        showStatus("No active editor");
        return;
    }

    const std::string uri = tab->lspDocumentUri().toStdString();
    auto diagnostics = diagnosticRegistry_->diagnosticsForUri(uri);
    if (diagnostics.empty())
    {
        showStatus("No diagnostics in this editor");
        return;
    }

    // Current cursor position (1-based)
    const int curLine = tab->editor().getCaretPos().getLineNumber() + 1;

    // Find the last diagnostic before the current line (reverse iterate)
    const hathor::control::Diagnostic* prev = nullptr;
    for (auto it = diagnostics.rbegin(); it != diagnostics.rend(); ++it)
    {
        if (it->line < curLine)
        {
            prev = &(*it);
            break;
        }
    }
    // If none before, wrap to the last diagnostic
    if (!prev)
    {
        prev = &diagnostics.back();
    }

    juce::CodeDocument::Position pos(tab->document(), prev->line - 1, prev->column - 1);
    tab->editor().moveCaretTo(pos, false);
    navigationHistory_->navigateTo({uri, prev->line - 1, prev->column - 1});
    showStatus("Navigated to: " + juce::String(prev->message));
}

// ---------------------------------------------------------------------------
// L-3: Panel visibility + StatusRibbon sync

void EditorArea::showProblemsPanel()
{
    if (problemsPanel_)
        problemsPanel_->setVisible(true);
    if (workspaceSearchPanel_)
        workspaceSearchPanel_->setVisible(false);
    if (symbolSearchPanel_)
        symbolSearchPanel_->setVisible(false);
}

void EditorArea::hideProblemsPanel()
{
    if (problemsPanel_)
        problemsPanel_->setVisible(false);
}

// ---------------------------------------------------------------------------
// L-4: Terminal panel visibility
// ---------------------------------------------------------------------------

void EditorArea::showTerminalPanel()
{
    if (terminalPanel_)
        terminalPanel_->setVisible(true);
    // Hide other bottom-docked panels when terminal is shown.
    if (problemsPanel_)
        problemsPanel_->setVisible(false);
    if (workspaceSearchPanel_)
        workspaceSearchPanel_->setVisible(false);
    if (symbolSearchPanel_)
        symbolSearchPanel_->setVisible(false);
}

void EditorArea::hideTerminalPanel()
{
    if (terminalPanel_)
        terminalPanel_->setVisible(false);
}

// ---------------------------------------------------------------------------
// L-5: Source control panel visibility
// ---------------------------------------------------------------------------

void EditorArea::showSourceControlPanel()
{
    if (sourceControlPanel_)
        sourceControlPanel_->setVisible(true);
    // Hide other bottom-docked panels when source control is shown.
    if (terminalPanel_)
        terminalPanel_->setVisible(false);
    if (problemsPanel_)
        problemsPanel_->setVisible(false);
    if (workspaceSearchPanel_)
        workspaceSearchPanel_->setVisible(false);
    if (symbolSearchPanel_)
        symbolSearchPanel_->setVisible(false);
}

void EditorArea::hideSourceControlPanel()
{
    if (sourceControlPanel_)
        sourceControlPanel_->setVisible(false);
}

// ---------------------------------------------------------------------------
// L-6: Debug & Runtime Inspector panel visibility
// ---------------------------------------------------------------------------

void EditorArea::showDebugPanel()
{
    if (debugPanel_)
        debugPanel_->setVisible(true);
    // Hide other bottom-docked panels when the debug panel is shown.
    if (terminalPanel_)
        terminalPanel_->setVisible(false);
    if (problemsPanel_)
        problemsPanel_->setVisible(false);
    if (sourceControlPanel_)
        sourceControlPanel_->setVisible(false);
    if (workspaceSearchPanel_)
        workspaceSearchPanel_->setVisible(false);
    if (symbolSearchPanel_)
        symbolSearchPanel_->setVisible(false);
}

void EditorArea::hideDebugPanel()
{
    if (debugPanel_)
        debugPanel_->setVisible(false);
}

// ---------------------------------------------------------------------------
void EditorArea::registerEditorActions()
{
    if (!actionRegistry_)
        actionRegistry_ = std::make_unique<ActionRegistry>();

    // Register L-1 editor actions with their key bindings
    actionRegistry_->registerAction("editor.find",              "Find…",              "Editor",     "Find in file");
    actionRegistry_->registerAction("editor.replace",           "Replace…",             "Editor",     "Replace in file");
    actionRegistry_->registerAction("editor.toggleSplit",       "Toggle Split",        "Window",     "Split editor");
    actionRegistry_->registerAction("editor.commandPalette",    "Command Palette…",    "General",    "Run a command");
    actionRegistry_->registerAction("editor.reopenClosed",      "Reopen Closed Tab",   "Editor",     "Reopen last closed tab");
    actionRegistry_->registerAction("editor.togglePin",         "Toggle Pin",          "Editor",     "Pin/unpin tab");
    actionRegistry_->registerAction("tab.close",                "Close Tab",           "Editor",     "Close active tab");
    actionRegistry_->registerAction("tab.new",                  "New Tab",             "Editor",     "Open new untitled tab");
    actionRegistry_->registerAction("tab.gotoDefinition",       "Go to Definition",   "Go",         "Jump to definition");
    actionRegistry_->registerAction("tab.peekDefinition",       "Peek Definition",    "Go",         "Peek definition");
    actionRegistry_->registerAction("file.save",                "Save",               "File",       "Save current file");
    actionRegistry_->registerAction("file.saveAs",              "Save As…",           "File",       "Save as…");
    actionRegistry_->registerAction("file.reload",              "Reload",             "File",       "Reload from disk");

    // L-1: editor.eval is registered in the AI-4 section above

    // Bind key shortcuts (macOS uses Cmd as primary modifier)
    if (auto k = parseKeyEquivalent("Cmd+F"))      actionRegistry_->bindKey(*k, "editor.find");
    if (auto k = parseKeyEquivalent("Cmd+Option+F"))  actionRegistry_->bindKey(*k, "editor.replace");
    if (auto k = parseKeyEquivalent("Cmd+\\"))    actionRegistry_->bindKey(*k, "editor.toggleSplit");
    if (auto k = parseKeyEquivalent("Cmd+Shift+P")) actionRegistry_->bindKey(*k, "editor.commandPalette");
    if (auto k = parseKeyEquivalent("Cmd+Shift+T")) actionRegistry_->bindKey(*k, "editor.reopenClosed");
    if (auto k = parseKeyEquivalent("Cmd+Enter")) actionRegistry_->bindKey(*k, "editor.eval");

    // L-2: Navigation & workspace search actions
    actionRegistry_->registerAction("editor.quickOpen",         "Quick Open…",      "Go",    "Open file by name");
    actionRegistry_->registerAction("editor.gotoDefinition",   "Go to Definition", "Go",    "Jump to symbol definition");
    actionRegistry_->registerAction("editor.peekDefinition",   "Peek Definition",  "Go",    "Peek definition in place");
    actionRegistry_->registerAction("editor.gotoReferences",   "Find References",  "Go",    "Find all references");
    actionRegistry_->registerAction("editor.navigateBack",     "Go Back",          "Go",    "Navigate backward");
    actionRegistry_->registerAction("editor.navigateForward",  "Go Forward",       "Go",    "Navigate forward");
    actionRegistry_->registerAction("editor.workspaceSearch",  "Search in Files…", "Go",    "Search across workspace");
    actionRegistry_->registerAction("editor.symbolSearch",     "Go to Symbol…",    "Go",    "Find symbol in workspace");
    actionRegistry_->registerAction("editor.docSymbols",       "Document Symbols", "Go",    "Symbols in current file");
    actionRegistry_->registerAction("editor.nextDiagnostic",   "Next Error",       "Go",    "Go to next diagnostic");
    actionRegistry_->registerAction("editor.prevDiagnostic",   "Previous Error",   "Go",    "Go to previous diagnostic");

    // L-2: Key bindings (macOS)
    if (auto k = parseKeyEquivalent("Cmd+P"))       actionRegistry_->bindKey(*k, "editor.quickOpen");
    if (auto k = parseKeyEquivalent("F12"))          actionRegistry_->bindKey(*k, "editor.gotoDefinition");
    if (auto k = parseKeyEquivalent("Shift+F12"))    actionRegistry_->bindKey(*k, "editor.gotoReferences");
    if (auto k = parseKeyEquivalent("Cmd+Option+Left"))  actionRegistry_->bindKey(*k, "editor.navigateBack");
    if (auto k = parseKeyEquivalent("Cmd+Option+Right")) actionRegistry_->bindKey(*k, "editor.navigateForward");
    if (auto k = parseKeyEquivalent("Cmd+Shift+F"))  actionRegistry_->bindKey(*k, "editor.workspaceSearch");
    if (auto k = parseKeyEquivalent("Cmd+T"))        actionRegistry_->bindKey(*k, "editor.symbolSearch");
    if (auto k = parseKeyEquivalent("F8"))            actionRegistry_->bindKey(*k, "editor.nextDiagnostic");
    if (auto k = parseKeyEquivalent("Shift+F8"))      actionRegistry_->bindKey(*k, "editor.prevDiagnostic");

    // L-2: Callbacks
    actionRegistry_->setCallback("editor.quickOpen",      [this]() { showQuickOpen(); });
    actionRegistry_->setCallback("editor.gotoDefinition", [this]() { gotoDefinition(); });
    actionRegistry_->setCallback("editor.peekDefinition", [this]() { peekDefinition(); });
    actionRegistry_->setCallback("editor.gotoReferences", [this]() { gotoReferences(); });
    actionRegistry_->setCallback("editor.navigateBack",   [this]() { navigateBack(); });
    actionRegistry_->setCallback("editor.navigateForward", [this]() { navigateForward(); });
    actionRegistry_->setCallback("editor.workspaceSearch", [this]() { showWorkspaceSearch(); });
    actionRegistry_->setCallback("editor.symbolSearch",   [this]() { showSymbolSearch(); });
    actionRegistry_->setCallback("editor.docSymbols",     [this]() { showDocumentSymbols(); });
    actionRegistry_->setCallback("editor.nextDiagnostic", [this]() { navigateToNextDiagnostic(); });
    actionRegistry_->setCallback("editor.prevDiagnostic", [this]() { navigateToPrevDiagnostic(); });

    // L-1: Install callbacks
    actionRegistry_->setCallback("editor.find", [this]() { showFindReplace(); });
    actionRegistry_->setCallback("editor.commandPalette", [this]() {
        if (commandPalette_)
        {
            juce::Component* topParent = getTopLevelComponent();
            commandPalette_->show(topParent);
        }
    });
    actionRegistry_->setCallback("editor.reopenClosed", []() {
        // Reopen last closed tab — handled by the active EditorGroup
    });
    actionRegistry_->setCallback("editor.toggleSplit", [this]() { toggleSplit(); });
    actionRegistry_->setCallback("tab.new", [this]() { openUntitledTab(); });
    actionRegistry_->setCallback("editor.togglePin", []() {
        // Pin toggle is handled at the EditorGroup/EnhancedTabBar level
    });

    // L-4: Terminal + task runner actions
    actionRegistry_->registerAction("terminal.toggle",      "Toggle Terminal",     "Terminal",    "Show/hide the integrated terminal");
    actionRegistry_->registerAction("terminal.runCommand",  "Run Command…",        "Terminal",    "Run a command in the terminal");
    actionRegistry_->registerAction("terminal.cancel",      "Cancel Process",      "Terminal",    "Cancel the running terminal process");

    if (auto k = parseKeyEquivalent("Cmd+Shift+`")) actionRegistry_->bindKey(*k, "terminal.toggle");

    actionRegistry_->setCallback("terminal.toggle", [this]() {
        if (terminalPanel_ && terminalPanel_->isVisible())
            hideTerminalPanel();
        else
            showTerminalPanel();
        resized();
    });
    actionRegistry_->setCallback("terminal.cancel", [this]() {
        if (terminalPanel_ && terminalPanel_->isRunning())
            terminalPanel_->cancelProcess();
    });

    // L-5: Git source control actions
    actionRegistry_->registerAction("git.toggle",          "Toggle Source Control",  "Git",    "Show/hide the Git source control panel");
    actionRegistry_->registerAction("git.commit",         "Commit",                 "Git",    "Commit staged changes");
    actionRegistry_->registerAction("git.push",           "Push",                   "Git",    "Push to remote");
    actionRegistry_->registerAction("git.pull",           "Pull",                   "Git",    "Pull from remote");
    actionRegistry_->registerAction("git.stageAll",       "Stage All",              "Git",    "Stage all changes");
    actionRegistry_->registerAction("git.createBranch",   "Create Branch…",         "Git",    "Create a new branch");
    actionRegistry_->registerAction("git.switchBranch",   "Switch Branch…",         "Git",    "Switch to another branch");

    if (auto k = parseKeyEquivalent("Cmd+Shift+G"))  actionRegistry_->bindKey(*k, "git.toggle");

    // L-6: Debug & Runtime Inspector actions
    actionRegistry_->registerAction("debug.toggle",   "Toggle Debug & Inspect", "Debug", "Show/hide the Debug & Runtime Inspector panel");

    if (auto k = parseKeyEquivalent("Cmd+Shift+D"))  actionRegistry_->bindKey(*k, "debug.toggle");

    actionRegistry_->setCallback("debug.toggle", [this]() {
        if (debugPanel_ && debugPanel_->isVisible())
            hideDebugPanel();
        else
            showDebugPanel();
        resized();
    });

    actionRegistry_->setCallback("git.toggle", [this]() {
        if (sourceControlPanel_ && sourceControlPanel_->isVisible())
        {
            hideSourceControlPanel();
        }
        else
        {
            showSourceControlPanel();
            if (sourceControlPanel_)
                sourceControlPanel_->refresh();
        }
        resized();
    });
    actionRegistry_->setCallback("git.commit", [this]() {
        if (sourceControlPanel_)
            sourceControlPanel_->commit();
    });
    actionRegistry_->setCallback("git.push", [this]() {
        if (sourceControlPanel_)
            sourceControlPanel_->push();
    });
    actionRegistry_->setCallback("git.pull", [this]() {
        if (sourceControlPanel_)
            sourceControlPanel_->pull();
    });
    actionRegistry_->setCallback("git.stageAll", [this]() {
        if (sourceControlPanel_)
            sourceControlPanel_->stageSelected();
    });
    actionRegistry_->setCallback("git.createBranch", [this]() {
        if (sourceControlPanel_)
            sourceControlPanel_->createBranch();
    });
     actionRegistry_->setCallback("git.switchBranch", [this]() {
         if (sourceControlPanel_)
             sourceControlPanel_->switchBranch();
     });
}

// ===========================================================================
// 20.7: Workspace session persistence (save / restore)
// ===========================================================================

void EditorArea::wireCommonTabIntegrations(HathorTab& tab)
{
    // AI-4: Install LSP + ghost-text clients on the tab.
    tab.installLspClient(lspClient_.get());
    tab.notifyLspDidOpen();
    tab.installGhostClient(ghostClient_.get());

    // AI-G3: Wire the authoring-context callback for ghost-text FIM (identical
    // for all tabs — openUntitledTab and openFile both install this).
    HathorTab* tabPtr = &tab;
    tab.getAuthoringContext = [this, tabPtr]() -> nlohmann::json {
        auto caretPos = tabPtr->editor().getCaretPos();
        hathor::control::CompletionRequest req;
        req.file = tabPtr->lspDocumentUri().toStdString();
        req.uri  = tabPtr->lspDocumentUri().toStdString();
        req.line = caretPos.getLineNumber();
        req.character = caretPos.getIndexInLine();
        req.language = tabPtr->isChuckTab() ? "chuck" : "mininotation";
        req.documentText = tabPtr->document().getAllContent().toStdString();
        const auto region = tabPtr->editor().getHighlightedRegion();
        if (!region.isEmpty())
        {
            const auto selStart = tabPtr->editor().getSelectionStart();
            const auto selEnd   = tabPtr->editor().getSelectionEnd();
            req.selection = hathor::control::CompletionRequest::Range{
                selStart.getLineNumber(), selStart.getIndexInLine(),
                selEnd.getLineNumber(),   selEnd.getIndexInLine()};
            req.selectedText = tabPtr->editor().getTextInRange(region).toStdString();
        }
        return ci_.assembleCompletionContext(req);
    };
}

HathorTab* EditorArea::createRestoredTab(const TabState& state)
{
    // Construct the tab with the persisted slot index (not re-derived) so the
    // tab→slot mapping is exactly preserved across sessions.
    juce::File file(state.filePath.empty() ? juce::File()
                                          : juce::File(state.filePath));
    auto tab = std::make_unique<HathorTab>(state.slotIndex, file);

    if (!state.filePath.empty())
        tab->setFilePath(file);

    if (state.frontMatter.has_value())
        tab->setFrontMatter(state.frontMatter.value());

    if (state.displayLabel.has_value())
        tab->setDisplayLabel(state.displayLabel.value());

    // Populate document content.
    if (!state.filePath.empty())
    {
        // File-backed tab: reload body from disk. If the file was deleted the
        // document stays empty (existing missing-file behaviour — no crash).
        if (file.existsAsFile())
        {
            const juce::String contents = file.loadFileAsString();
            const auto parseResult = parseHathorFile(contents.toStdString());
            if (const auto* hf = std::get_if<HathorFile>(&parseResult))
                tab->document().replaceAllContent(juce::String(hf->body));
            else
                tab->document().replaceAllContent(contents);  // no front matter
        }
        else
        {
            // File no longer exists — use persisted content as a fallback so
            // the user's work isn't silently lost, but the tab is still clean.
            if (!state.content.empty())
                tab->document().replaceAllContent(juce::String(state.content));
        }
    }
    else
    {
        // Untitled tab — restore persisted content.
        tab->document().replaceAllContent(juce::String(state.content));
    }

    // Clear the unsaved-dot flag: restored content IS the saved state, so the
    // tab must not appear dirty just because it was re-loaded.
    tab->clearUnsavedDot();

    // Wire all standard callbacks (same set as openUntitledTab/openFile).
    wireUnsavedCallback(*tab);
    wirePlayStopCallback(*tab);
    wireContextMenuCallbacks(*tab);
    installKeyListenerForTab(*tab);
    wireCommonTabIntegrations(*tab);

    HathorTab* tabPtr = tab.get();
    addAndMakeVisible(*tab);
    tabs_.push_back(std::move(tab));

    // Restore cursor position, clamped to the actual document length.
    if (state.cursorOffset >= 0)
    {
        const int docLen = tabPtr->document().getNumCharacters();
        const int clampedOffset = std::clamp(static_cast<int>(state.cursorOffset),
                                             0, docLen);
        juce::CodeDocument::Position pos(tabPtr->document(), clampedOffset);
        tabPtr->editor().moveCaretTo(pos, false);
    }

    return tabPtr;
}

// ---------------------------------------------------------------------------
// saveWorkspace — serialise current tab state into a WorkspaceSession
// ---------------------------------------------------------------------------

WorkspaceSession EditorArea::saveWorkspace() const
{
    WorkspaceSession session;
    session.schemaVersion  = WorkspaceSession::kSchemaVersion;

    for (const auto& tab : tabs_)
    {
        TabState state;
        state.slotIndex     = tab->slotIndex();
        state.isChuckTab    = tab->isChuckTab();

        if (const auto& fp = tab->filePath(); fp.has_value())
            state.filePath  = fp->getFullPathName().toStdString();

        // Persist the content for *all* tabs. For file-backed tabs this is a
        // safety net in case the file is deleted between sessions; for untitled
        // tabs it is the only source of truth.
        state.content = tab->document().getAllContent().toStdString();

        state.cursorOffset  = static_cast<int64_t>(
            tab->editor().getCaretPosition());

        if (const auto& fm = tab->frontMatter(); fm.has_value())
            state.frontMatter = *fm;

        if (const auto& label = tab->displayLabel(); label.has_value())
            state.displayLabel = *label;

        session.tabs.push_back(std::move(state));
    }

    if (settingsActive_)
    {
        session.activeIndex    = -1;
        session.settingsActive = true;
    }
    else
    {
        session.activeIndex    = activeIndex_;
        session.settingsActive = false;
    }

    return session;
}

// ---------------------------------------------------------------------------
// restoreWorkspace — recreate tabs from a WorkspaceSession
// ---------------------------------------------------------------------------

void EditorArea::restoreWorkspace(const WorkspaceSession& session,
                                  juce::ApplicationProperties* props)
{
    // Reject unknown/future schema versions — fail safe (no restore).
    if (session.schemaVersion != WorkspaceSession::kSchemaVersion)
        return;

    if (session.tabs.empty())
        return;

    // Pre-register slot names from front-matter so findOrAddSlot returns the
    // same indices as the original session. This guarantees deterministic
    // slot→tab mapping even when tabs are restored in a different order.
    for (const auto& tabState : session.tabs)
    {
        if (tabState.frontMatter.has_value()
            && tabState.frontMatter->slot.has_value())
        {
            audio_.findOrAddSlot(*tabState.frontMatter->slot);
        }
    }

    for (const auto& tabState : session.tabs)
        createRestoredTab(tabState);

    // Restore active tab.
    if (session.settingsActive && props != nullptr)
    {
        // Re-open the Settings tab (A2) when it was active at save time.
        openSettingsTab(props);
    }
    else
    {
        const int idx = session.activeIndex;
        if (idx >= 0 && idx < static_cast<int>(tabs_.size()))
            activateTab(idx);
        else if (!tabs_.empty())
            activateTab(0);
        else
            activeIndex_ = -1;
    }

    refreshTabBar();
    resized();
}

} // namespace hathor::ui
