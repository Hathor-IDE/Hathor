// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * MainWindow.cpp — top-level JUCE DocumentWindow implementation.
 *
 * Requirements: 20.1, 20.2, 20.3, 20.4, 20.5
 */

#include "MainWindow.hpp"

// juce_gui_extra is needed for juce::CodeEditorComponent colour IDs used in
// HathorLookAndFeel (CodeEditorComponent lives in juce_gui_extra, not juce_gui_basics).
#include <juce_gui_extra/juce_gui_extra.h>

// Phase G — Petdex D4 attribution gate (applied at selection/restore time)
#include "PetdexAttribution.hpp"

#include <cctype>
#include <filesystem>
#include <string>

// ---------------------------------------------------------------------------
// AI-8: Context bridges
// ---------------------------------------------------------------------------
#include "EditorContextBridge.hpp"
#include "LspContextBridge.hpp"

// ---------------------------------------------------------------------------
// Real child component headers (include when available)
// ---------------------------------------------------------------------------
//
// The following headers exist and are included directly:
#include "ActivityRibbon.hpp"   // task 3.2 — implemented
#include "ExplorerPanel.hpp"    // task 3.2 — implemented

// Task 3.4: EditorArea is now implemented — include the real header.
#include "EditorArea.hpp"

// Task 3.8: VisualizerPanel is now implemented — include the real header.
#include "VisualizerPanel.hpp"

// Task 3.9: SliderPanel is now implemented — include the real header.
// (SliderPanel.hpp is already included transitively via ChatSidebar.hpp)

// ChatSidebar and AcpAgentSession are now fully implemented (task 5.1).
#include "ChatSidebar.hpp"
#include "AcpAgentSession.hpp"
#include "AgentRegistry.hpp"

// UITimer (task 3.7) — real implementation is now available.
#include "UITimer.hpp"

// Agent 0.1: startup welcome overlay
#include "WelcomeScreen.hpp"

// HathorLookAndFeel is defined in HathorLookAndFeel.hpp/.cpp (design system)

// ==========================================================================
// MainWindow
// ==========================================================================

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MainWindow::MainWindow(AudioEngine& audio,
                       hathor::control::ControlInterface& ci,
                       std::string agentExePath,
                       std::string hathorMcpPath)
    : juce::DocumentWindow(
           "Hathor",
          juce::Colour(Palette::defaultPalette().background),
           juce::DocumentWindow::allButtons)
    , agentExePath_(agentExePath)
    , audio_(audio)
    , ci_(ci)
{
    // Apply the dark theme to this window and all its children (Req 20.2).
    setLookAndFeel(&lookAndFeel_);

    // -----------------------------------------------------------------------
    // Minimum window size (Req 20.5)
    // -----------------------------------------------------------------------
    setResizeLimits(1024, 768, 10000, 10000);

    // -----------------------------------------------------------------------
    // Instantiate child components (Req 20.1, 20.4)
    // These are JUCE native components — no embedded webview / Electron.
    // -----------------------------------------------------------------------
    activityRibbon_  = std::make_unique<hathor::ui::ActivityRibbon>();
    explorerPanel_   = std::make_unique<hathor::ui::ExplorerPanel>();
    editorArea_      = std::make_unique<hathor::ui::EditorArea>(audio_, ci_);

     // AI-8: Create and wire the editor + LSP context bridges.
     // EditorContextBridge captures editor state (file, cursor, selection)
     // from the EditorArea on the JUCE message thread and snapshots it for
     // thread-safe access from the MCP accept-loop.
     editorContextBridge_ = std::make_unique<hathor::ui::EditorContextBridge>(*editorArea_);
     editorArea_->setEditorContextBridge(editorContextBridge_.get());

     // LspContextBridge captures LSP diagnostics and status from the HathorLspClient.
     lspContextBridge_ = std::make_unique<hathor::ui::LspContextBridge>(editorArea_->lspClient());
     editorArea_->setLspContextBridge(lspContextBridge_.get());

     // Inject all AI-8 context providers into ControlInterface so that the
     // get-context MCP command can assemble dynamic context.
     ci_.setEditorContextProvider(editorContextBridge_.get());
     ci_.setLspContextProvider(lspContextBridge_.get());
     ci_.setLanguageMetadata(&editorArea_->metadata(),
                             &editorArea_->metadataCompatibility());
    visualizerPanel_ = std::make_unique<hathor::ui::VisualizerPanel>(audio_);

    // L-3: Create StatusRibbon, wired to the shared DiagnosticRegistry
    // (owned by EditorArea) so the ribbon and Problems panel stay in sync.
    statusRibbon_ = std::make_unique<hathor::ui::StatusRibbon>(
        editorArea_->diagnosticRegistry());

    // L-3: Clicking the Problems indicator in the StatusRibbon opens the Problems panel.
    statusRibbon_->onErrorsClicked = [this]()
    {
        if (editorArea_)
        {
            editorArea_->showProblemsPanel();
            activityRibbon_->setActivePanel(hathor::ui::Panel::Problems);
            resized();
        }
    };

    // L-6: Clicking the worker indicator in the StatusRibbon opens the
    // Debug & Runtime Inspector on the Runtime tab (worker liveness/restart
    // state, per-tab ChucK VMs, playback).
    statusRibbon_->onWorkerClicked = [this]()
    {
        if (editorArea_)
        {
            // Close other bottom-docked panels.
            editorArea_->hideProblemsPanel();
            editorArea_->hideTerminalPanel();
            editorArea_->hideSourceControlPanel();

            editorArea_->showDebugPanel();
            if (auto* panel = editorArea_->debugPanel())
                panel->showRuntimeTab();
            activityRibbon_->setActivePanel(hathor::ui::Panel::Debug);
            resized();
        }
    };

    // L-5: Clicking the Git indicator in the StatusRibbon opens the
    // source-control panel.
    statusRibbon_->onGitClicked = [this]()
    {
        if (editorArea_)
        {
            // Close other bottom-docked panels.
            editorArea_->hideProblemsPanel();
            editorArea_->hideTerminalPanel();

            const bool wantsOpen = !editorArea_->sourceControlPanel()->isVisible();
            if (wantsOpen)
            {
                editorArea_->showSourceControlPanel();
                editorArea_->sourceControlPanel()->refresh();
            }
            else
            {
                editorArea_->hideSourceControlPanel();
            }
            activityRibbon_->setActivePanel(
                wantsOpen ? hathor::ui::Panel::VersionControl : hathor::ui::Panel::None);
            editorArea_->resized();
        }
    };

    // -----------------------------------------------------------------------
    // Create and wire chat sidebar (B6: multi-thread tabs, C2: per-thread reconnect)
    // -----------------------------------------------------------------------
     chatSidebar_     = std::make_unique<hathor::ui::ChatSidebar>(audio_, ci_);

     // A2: Install the known-agent registry so the sidebar's header picker is
     // live. The registry is JUCE-free; load() reads the platform config dir
     // (agent-presets.json) and falls back to compiled-in defaults if absent.
     agentRegistry_ = std::make_unique<hathor::ui::AgentRegistry>();
     agentRegistry_->load();
     chatSidebar_->setAgentRegistry(agentRegistry_.get());

      // -------------------------------------------------------------------
      // Agent 0.1: Resolve the workspace root from persisted state instead
      // of the process CWD (audit P1). Prefers "lastWorkspacePath"; falls
      // back to the older "explorerLastDirectory" key. If neither yields a
      // valid directory the welcome screen is shown after construction.
      // -------------------------------------------------------------------
      appProperties_.setStorageParameters(makePropertiesOptions());

      std::string projectDir;
      if (const auto* props = appProperties_.getUserSettings())
      {
          const juce::File last(props->getValue("lastWorkspacePath"));
          if (last.isDirectory())
              projectDir = last.getFullPathName().toStdString();
          else
          {
              const juce::File legacy(props->getValue("explorerLastDirectory"));
              if (legacy.isDirectory())
                  projectDir = legacy.getFullPathName().toStdString();
          }
      }
      if (!projectDir.empty())
      {
          workspaceDir_ = projectDir;

          // 0.2: seed the recent-projects MRU with the startup workspace.
          pushRecentProject(projectDir);

          // L-2: Set the workspace root for navigation & search components.
          if (editorArea_)
              editorArea_->setWorkspaceRoot(std::filesystem::path(projectDir));
      }

      // Chat threads need *some* working directory even before a workspace
      // has been chosen — use the user's home folder, never the process CWD.
      const std::string threadWorkingDir =
          !workspaceDir_.empty()
              ? workspaceDir_
              : juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                    .getFullPathName().toStdString();

     // Store MCP path for thread creation and Settings updates (A2).
     hathorMcpPath_ = hathorMcpPath;

     // Wire MCP/control commands received on the Unix socket to ControlInterface
     // (Phase 2.5 H0).  The handler is invoked on the socket accept-loop worker
     // thread; dispatchWithCallback() routes each command through the normal
     // command handlers and returns the JSON result via the response sink, which
     // forwards it back over the socket.
     // This handler is installed on every chat thread's session (B6).
     chatSidebar_->setMcpCommandHandler(
         [this](std::string commandLine, std::function<void(std::string)> respond)
         {
             ci_.dispatchWithCallback(
                 commandLine,
                 [respond = std::move(respond)](nlohmann::json result)
                 {
                     respond(result.dump());
                 });
         });

      // B6: Restore persisted chat threads (if any) so closed tabs are not
      // resurrected.  Falls back to a single thread when no persisted state
      // exists (preserving the original startup behaviour).
      chatSidebar_->setApplicationProperties(&appProperties_);
      if (!agentExePath.empty())
      {
          chatSidebar_->restoreChatThreads(agentExePath, threadWorkingDir, hathorMcpPath);
          // If no threads were restored (first launch or all were closed),
          // create a default single thread — same as the original behaviour.
          if (chatSidebar_->threadCount() == 0)
              chatSidebar_->addThread(agentExePath, threadWorkingDir, hathorMcpPath);
      }

    // -----------------------------------------------------------------------
    // Wire ActivityRibbon panel toggles (H1: Explorer)
    // The callback toggles the explorer open/closed and syncs the ribbon's
    // active-button accent highlight via setActivePanel().
    // -----------------------------------------------------------------------
    activityRibbon_->onContextMenu =
        [this](const juce::Point<int>&)
        {
            // 0.2: ribbon context menu — Open Folder… + Open Recent entries.
            juce::PopupMenu menu;
            menu.addItem(1, "Open Folder…");

            const std::vector<std::string> recent = loadRecentProjects();
            if (!recent.empty())
            {
                juce::PopupMenu recentMenu;
                for (std::size_t i = 0; i < recent.size(); ++i)
                    recentMenu.addItem(static_cast<int>(100 + i),
                                       "Open Recent: " + juce::String(recent[i]));
                menu.addSubMenu("Open Recent", recentMenu);
            }

            menu.showMenuAsync(
                juce::PopupMenu::Options(),
                [this](int result)
                {
                    if (result == 1)
                        openFolderChooser();
                    else if (result >= 100)
                    {
                        const std::vector<std::string> recents = loadRecentProjects();
                        const int idx = result - 100;
                        if (juce::isPositiveAndBelow(idx, recents.size()))
                            switchWorkspace(juce::File(recents[static_cast<std::size_t>(idx)]));
                    }
                });
        };

    activityRibbon_->onPanelToggled =
        [this](hathor::ui::Panel panel)
        {
            // H1–L-6: All ActivityRibbon panels are wired (Explorer, Search,
            // AIAgent, Terminal, Problems, VersionControl, Debug). Each block
            // below toggles its panel and syncs the ribbon accent.
            if (panel == hathor::ui::Panel::Explorer)
            {
                // Close Problems panel when switching to another panel
                if (editorArea_)
                    editorArea_->hideProblemsPanel();
                const bool wantsOpen = (activityRibbon_->activePanel() != hathor::ui::Panel::Explorer);
                explorerPanel_->setVisible(wantsOpen);
                activityRibbon_->setActivePanel(wantsOpen ? hathor::ui::Panel::Explorer : hathor::ui::Panel::None);
                resized(); // re-lay-out editor area
            }
            else if (panel == hathor::ui::Panel::None)
            {
                 // Settings button: open or focus the Settings tab (A2).
                 if (editorArea_)
                 {
                     auto* settings = editorArea_->openSettingsTab(&appProperties_);
                     if (settings != nullptr)
                     {
                          settings->onSettingsApplied = [this]()
                          {
                              // Restart the agent session with the persisted path (A2).
                              const auto* props = appProperties_.getUserSettings();
                              if (props == nullptr)
                                  return;

                              const std::string newAgentPath =
                                  props->getValue("settings.agentExePath").toStdString();

                              if (newAgentPath != agentExePath_)
                              {
                                  agentExePath_ = newAgentPath;
                                  const std::string threadWorkingDir =
                                      !workspaceDir_.empty()
                                          ? workspaceDir_
                                          : juce::File::getSpecialLocation(
                                                juce::File::userHomeDirectory)
                                                .getFullPathName().toStdString();
                                  chatSidebar_->restartAllThreads(
                                      agentExePath_, threadWorkingDir, hathorMcpPath_);
                              }
                           };

                           // A2: React to the agent preset selection from the
                           // Settings tab — restart all chat threads with the
                           // resolved command string (preset exe + args, or
                           // custom Browse path + args). This is the primary
                           // entry point for switching agents from Settings.
                           settings->onAgentSelectionApplied = [this](const std::string& agentCommand)
                           {
                               if (agentCommand != agentExePath_)
                               {
                                   agentExePath_ = agentCommand;
                                   const std::string threadWorkingDir =
                                       !workspaceDir_.empty()
                                           ? workspaceDir_
                                           : juce::File::getSpecialLocation(
                                                 juce::File::userHomeDirectory)
                                                 .getFullPathName().toStdString();
                                   chatSidebar_->restartAllThreads(
                                       agentExePath_, threadWorkingDir, hathorMcpPath_);
                               }
                           };

                           // Phase G (D2–D4): react to an applied Petdex
                          // selection. Fires on Apply with the committed slug
                          // (empty string = explicit "no mascot").
                          settings->onPetSelectionApplied = [this](const std::string& slug)
                          {
                              applySelectedPet(slug);
                          };
                     }
                 }
             }
            // L-2: Search panel toggles the bottom-docked workspace search
            // panel (Agent 2.4: Panel::Search wiring). Reuses the existing
            // WorkspaceSearchPanel owned by EditorArea — mirrors the
            // Problems/Terminal/VersionControl/Debug toggle pattern.
            if (panel == hathor::ui::Panel::Search)
            {
                const bool wantsOpen = (activityRibbon_->activePanel() != hathor::ui::Panel::Search);
                if (wantsOpen)
                    editorArea_->showSearchPanel();
                else
                    editorArea_->hideSearchPanel();
                activityRibbon_->setActivePanel(wantsOpen ? hathor::ui::Panel::Search : hathor::ui::Panel::None);
                editorArea_->resized(); // re-lay-out editor area
            }

            // H1: AIAgent panel toggles the right-hand ChatSidebar (the AI
            // agent UI). Mirrors the Explorer left-sidebar toggle (Agent 2.4:
            // Panel::AIAgent wiring). ChatSidebar owns its thread/session state
            // (ChatThread/AcpAgentSession) as a persistent member, so toggling
            // visibility does not destroy or recreate any conversation state.
            if (panel == hathor::ui::Panel::AIAgent)
            {
                const bool wantsOpen = (activityRibbon_->activePanel() != hathor::ui::Panel::AIAgent);
                if (chatSidebar_)
                    chatSidebar_->setVisible(wantsOpen);
                activityRibbon_->setActivePanel(wantsOpen ? hathor::ui::Panel::AIAgent : hathor::ui::Panel::None);
                resized(); // re-lay-out content areas (editor fills freed space)
            }

            // L-4: Terminal panel toggles the bottom-docked terminal panel.
            if (panel == hathor::ui::Panel::Terminal)
            {
                const bool wantsOpen = (activityRibbon_->activePanel() != hathor::ui::Panel::Terminal);
                if (wantsOpen)
                {
                    editorArea_->showTerminalPanel();
                    editorArea_->terminalPanel()->openShell();
                }
                else
                    editorArea_->hideTerminalPanel();
                activityRibbon_->setActivePanel(wantsOpen ? hathor::ui::Panel::Terminal : hathor::ui::Panel::None);
                editorArea_->resized(); // re-lay-out editor area
            }

            // L-3: Problems panel toggles the bottom-docked problems panel.
            if (panel == hathor::ui::Panel::Problems)
            {
                const bool wantsOpen = (activityRibbon_->activePanel() != hathor::ui::Panel::Problems);
                if (wantsOpen)
                    editorArea_->showProblemsPanel();
                else
                    editorArea_->hideProblemsPanel();
                activityRibbon_->setActivePanel(wantsOpen ? hathor::ui::Panel::Problems : hathor::ui::Panel::None);
                editorArea_->resized(); // re-lay-out editor area
            }

            // L-5: Version Control panel toggles the bottom-docked source
            // control panel (Changes/Commit + History with Git graph).
            if (panel == hathor::ui::Panel::VersionControl)
            {
                const bool wantsOpen = (activityRibbon_->activePanel() != hathor::ui::Panel::VersionControl);
                if (wantsOpen)
                {
                    editorArea_->showSourceControlPanel();
                    editorArea_->sourceControlPanel()->refresh();
                }
                else
                    editorArea_->hideSourceControlPanel();
                activityRibbon_->setActivePanel(wantsOpen ? hathor::ui::Panel::VersionControl : hathor::ui::Panel::None);
                editorArea_->resized(); // re-lay-out editor area
            }

            // L-6: Debug & Runtime Inspector panel toggles the bottom-docked
            // debug panel (Debugger + Runtime tabs).
            if (panel == hathor::ui::Panel::Debug)
            {
                const bool wantsOpen = (activityRibbon_->activePanel() != hathor::ui::Panel::Debug);
                if (wantsOpen)
                    editorArea_->showDebugPanel();
                else
                    editorArea_->hideDebugPanel();
                activityRibbon_->setActivePanel(wantsOpen ? hathor::ui::Panel::Debug : hathor::ui::Panel::None);
                editorArea_->resized(); // re-lay-out editor area
            }
        };

    // Wire ExplorerPanel file clicks → EditorArea::openFile (H1)
    // Clicking a song opens or recovers the corresponding tab.
    explorerPanel_->onFileClicked =
        [this](const juce::File& file)
        {
            if (editorArea_)
                editorArea_->openFile(file);
        };

    // -----------------------------------------------------------------------
    // L-1: Register editor actions (creates ergonomics components in EditorArea)
    // -----------------------------------------------------------------------
    if (editorArea_)
        editorArea_->registerEditorActions();

    // 0.2: Register workspace-lifecycle actions (Open Folder…).
    // Recent-project entries are refreshed dynamically right before the
    // palette is shown (see onCommandPaletteClicked below).
    if (auto* reg = editorArea_->actionRegistry())
    {
        reg->registerAction("workspace.openFolder", "Open Folder…", "File",
                            "Switch the workspace to another directory");
        reg->setCallback("workspace.openFolder", [this]() { openFolderChooser(); });
        refreshRecentActions();
    }

    // Wire breadcrumbs callbacks (accessed via EditorArea)
    editorArea_->breadcrumbsBar()->onCommandPaletteClicked = [this]() {
        refreshRecentActions();  // 0.2: keep MRU entries current in the palette
        editorArea_->commandPalette()->show(getContentComponent());
    };
    editorArea_->breadcrumbsBar()->onFindClicked = [this]() {
        editorArea_->showFindReplace();
    };
    editorArea_->breadcrumbsBar()->onSplitClicked = [this]() {
        editorArea_->toggleSplit();
    };
    editorArea_->breadcrumbsBar()->onBreadcrumbClicked = [this](const juce::File& file) {
        editorArea_->openFile(file);
    };

    // Wire find/replace panel callbacks
    editorArea_->findReplacePanel()->onFindNext = [this]() {
        editorArea_->findNextInActiveTab();
    };
    editorArea_->findReplacePanel()->onFindPrev = [this]() {
        editorArea_->findPrevInActiveTab();
    };
    editorArea_->findReplacePanel()->onReplace = [this]() {
        editorArea_->replaceInActiveTab();
    };
    editorArea_->findReplacePanel()->onReplaceAll = [this]() {
        editorArea_->replaceAllInActiveTab();
    };
    editorArea_->findReplacePanel()->onClosePanel = [this]() {
        editorArea_->hideFindReplace();
    };

    // ApplicationProperties storage was configured above; hand the properties
    // to the ExplorerPanel so it can persist/restore its last-used root (A4).
    explorerPanel_->setApplicationProperties(&appProperties_);

    // Restore the persisted theme on startup (B3). SettingsComponent persists
    // the theme as "settings.theme" (a ThemeId enum index). If absent or
    // unknown, loadSettings() clamps to Dark (the default).
    if (const auto* props = appProperties_.getUserSettings())
    {
        const int themeIdx = props->getIntValue("settings.theme",
                                                 static_cast<int>(ThemeId::Dark));
        static constexpr int kMinTheme = static_cast<int>(ThemeId::Dark);
        static constexpr int kMaxTheme = static_cast<int>(ThemeId::Light);
        const ThemeId appliedTheme =
            static_cast<ThemeId>(juce::jlimit(kMinTheme, kMaxTheme, themeIdx));
        lookAndFeel_.setPalette(paletteForTheme(appliedTheme));

        // B7-K3: Restore the persisted EQ preset on startup.
        // Loads from "settings.eqPreset" (stable identifier key).
        // Calls AudioEngine::setMasterEqPreset() which performs the atomic
        // swap into the master bus chain.  If the audio device isn't open yet,
        // AudioEngine::audioDeviceAboutToStart() will re-apply the current
        // preset once the sample rate is known.
        const juce::String eqPresetStr = props->getValue("settings.eqPreset", "flat");
        const hathor::EqPreset startupPreset =
            (eqPresetStr == "bass-boost") ? hathor::EqPreset::BassBoost
            : (eqPresetStr == "vocal")     ? hathor::EqPreset::Vocal
            : (eqPresetStr == "bright")    ? hathor::EqPreset::Bright
            :                                hathor::EqPreset::Flat;
        audio_.setMasterEqPreset(startupPreset);
    }

    // -----------------------------------------------------------------------
    // Phase G / D2–D4: Petdex mascot — resource service + D4-gated widget.
    // The resource service is app-lifetime and shares the manifest cache dir
    // (<userApplicationDataDirectory>/Hathor/Petdex). It does NO network work
    // until the user applies a selection, or a previously selected pet is
    // restored below (disk cache when present — offline launches stay
    // network-free).
    // -----------------------------------------------------------------------
    petdexResourceService_ = std::make_unique<hathor::ui::PetdexResourceService>(
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Hathor/Petdex"));

    petWidget_ = std::make_unique<hathor::ui::PetWidget>();
    petWidget_->setVisible(false);   // NoPet until a selection exists

    // D3 app-state reactivity: any actively playing pattern slot maps to the
    // "running" state; otherwise the mascot idles. The probe runs from the
    // widget's message-thread animation timer and never touches the audio
    // callback (AudioEngine::isSlotRunning is a plain atomic check).
    petWidget_->setActivityProbe([this]() -> bool
    {
        if (!audio_.isRunning())
            return false;
        for (int i = 0; i < AudioEngine::kNumSlots; ++i)
            if (audio_.isSlotRunning(i))
                return true;
        return false;
    });

    petWidget_->onStatusChanged = [this](hathor::ui::PetWidget::Status status)
    {
        if (petWidget_)
        {
            petWidget_->setVisible(status != hathor::ui::PetWidget::Status::NoPet);
            resized();
        }
    };

    // Sprite results arrive on the message thread via callAsync; the
    // SafePointer guard makes the delivery a no-op if the widget is ever
    // destroyed first (same convention as the D1 manifest service callbacks).
    petdexResourceService_->setResultCallback(
        [safeWidget = juce::Component::SafePointer<hathor::ui::PetWidget>(
             petWidget_.get())](const hathor::ui::PetdexSpriteResult& result)
        {
            if (safeWidget)
                safeWidget->onSpriteResult(result);
        });

    // Restore a persisted selection ("settings.petSelection"). The D4 gate
    // re-runs from the persisted attribution snapshot before anything is
    // drawn; the sprite loads from disk when cached (offline launch) or
    // completes the explicitly-requested download in the background.
    restorePetSelection();

    // Restore the last-used root directory if one was persisted. With no
    // workspace resolved (fresh launch) leave the explorer empty — the
    // welcome screen will drive the first selection (Agent 0.1).
    explorerPanel_->restoreLastDirectoryAndRefresh();
    if (explorerPanel_->directory() == juce::File() && !workspaceDir_.empty())
        explorerPanel_->setDirectory(juce::File(workspaceDir_));

     // J-6: Load persisted ghost completion telemetry (quality metrics) from disk.
     // Restores per-tab event history so metrics accumulate across sessions.
#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
     if (editorArea_)
     {
         juce::File appDataDir = juce::File::getSpecialLocation(
             juce::File::userApplicationDataDirectory);
         juce::File telemetryFile = appDataDir.getChildFile("Hathor/ghost-telemetry.json");
         editorArea_->loadTelemetry(telemetryFile.getFullPathName().toStdString());
     }
#endif

     // -----------------------------------------------------------------------
     // 20.7: Restore persisted editor workspace (tabs, cursors, slot state).
     // Must run after EditorArea is fully initialised (LSP/ghost clients,
     // context bridges wired) but before the content component is laid out.
     // Malformed or version-mismatched data is silently ignored.
     // -----------------------------------------------------------------------
     restoreWorkspace();

     // Add child components to the content component (DocumentWindow wraps one
    // content component; we use a plain Component as the layout host).
    auto* content = new juce::Component();
    content->addAndMakeVisible(*activityRibbon_);
    content->addAndMakeVisible(*explorerPanel_);
    content->addAndMakeVisible(*editorArea_);
    content->addAndMakeVisible(*chatSidebar_);
    content->addAndMakeVisible(*visualizerPanel_);

    // L-3: StatusRibbon — mounted at MainWindow level, below VisualizerPanel.
    content->addAndMakeVisible(*statusRibbon_);

    // Phase G (D2–D4): mascot overlay (bottom-right of the editor area).
    // Hidden until a pet is selected; setVisible is driven by status changes.
    content->addAndMakeVisible(*petWidget_);
    if (petWidget_->status() == hathor::ui::PetWidget::Status::NoPet)
        petWidget_->setVisible(false);

    // L-4: TerminalPanel is owned by EditorArea (bottom-docked, like ProblemsPanel).
    // MainWindow toggles its visibility via the ActivityRibbon Terminal button.

    // Explorer starts hidden; opens when the user clicks the Explorer button
    // in the ActivityRibbon (H1).
    explorerPanel_->setVisible(false);

    setContentOwned(content, false);
    setUsingNativeTitleBar(true);
    setResizable(true, false);

    // -----------------------------------------------------------------------
    // Restore or compute initial window bounds (Req 20.5)
    // -----------------------------------------------------------------------
    const juce::Rectangle<int> bounds = resolveInitialBounds();
    setBounds(bounds);

    // Agent 0.1: fresh launch with no persisted workspace → show the welcome
    // overlay on top of the shell until a folder is opened or created.
    if (workspaceDir_.empty())
        showWelcomeScreen();

    // -----------------------------------------------------------------------
    // Start UITimer at 60 Hz — audio device is open at this point (Req 28.5)
    // UITimer drains the visualizer ring buffer and syncs slider displays.
    //
    // Signature: UITimer(SpscRingBuffer<128>&, VisualizerPanel&,
    //                    SliderPanel&, AudioEngine&)
    //
    // sliderPanel comes from ChatSidebar (0.5/S4 — single BPM/gain surface).
    // -----------------------------------------------------------------------
    uiTimer_ = std::make_unique<hathor::ui::UITimer>(
        audio_.visualizerBuffer(),
        *visualizerPanel_,
        chatSidebar_->getSliderPanel(),
        audio_);
    uiTimer_->startTimerHz(60);

    // B1: Sync per-tab Play/Stop button visuals to the engine's slot state.
    // This runs at 60 Hz so the UI reflects slot state changes from any path
    // (button clicks, slot-play/slot-stop commands, etc.) — not just clicks.
      uiTimer_->onSyncSlotButtons = [this]()
      {
          if (editorArea_)
              editorArea_->syncSlotButtonStates();
      };

      // AI-4: Ghost-text tick — drive debounce + timeout logic on all tabs.
      uiTimer_->onGhostTick = [this]()
      {
          if (editorArea_)
              editorArea_->ghostTick();
      };

     // C1: Now-playing highlight — route drained events to the editor area
     // so it can resolve sourceOffset → glyph bounds and paint the overlay.
      uiTimer_->onUpdateNowPlaying = [this](
          const std::vector<hathor::Event<hathor::ParamMap>>& events)
      {
          if (editorArea_)
              editorArea_->updateNowPlayingHighlight(events);
      };

      // L-3: StatusRibbon — sync transport/worker/LSP state at 60 Hz
      uiTimer_->onSyncStatusRibbon = [this]()
      {
          if (statusRibbon_)
          {
              statusRibbon_->setTransportRunning(audio_.isRunning());
              statusRibbon_->setBpm(audio_.getBpm());
              statusRibbon_->setWorkerAlive(audio_.hasWorker());
              statusRibbon_->setLspConnected(editorArea_->isLspConnected());

              // L-5: Git status from the SourceControlPanel's repository model.
              if (editorArea_->sourceControlPanel())
              {
                  auto* repo = editorArea_->sourceControlPanel()->repository();
                  if (repo && repo->hasRepository())
                  {
                      auto entries = repo->getStatusEntries();
                      int staged = 0, unstaged = 0;
                      for (const auto& e : entries)
                      {
                          if (e.staged == hathor::ui::GitStaged::Yes) ++staged;
                          else ++unstaged;
                      }
                      statusRibbon_->setGitStatus(
                          repo->getCurrentBranch(), staged, unstaged);
                  }
                  else
                  {
                      statusRibbon_->setGitStatus("", 0, 0);
                  }
              }
          }
      };

      setVisible(true);
}

MainWindow::~MainWindow()
{
    // Stop the timer before destroying components it references.
    if (uiTimer_)
        uiTimer_->stopTimer();

    // chatSidebar_ (which owns all AcpAgentSession instances) is destroyed
    // after MainWindow's other members. Its destructor stops all sessions.

    // Remove look-and-feel reference before it is destroyed.
    setLookAndFeel(nullptr);
}

// ---------------------------------------------------------------------------
// resized() — four-zone layout (Req 20.1, 20.3)
// ---------------------------------------------------------------------------

void MainWindow::resized()
{
    juce::DocumentWindow::resized();

    auto* content = getContentComponent();
    if (content == nullptr)
        return;

    content->setBounds(getLocalBounds());

    auto b = content->getLocalBounds();

    // 1. Activity ribbon — fixed 48 px on the left (Req 20.1)
    if (activityRibbon_)
        activityRibbon_->setBounds(b.removeFromLeft(48));

    // 2. Explorer panel — fixed 240 px, immediately right of the ribbon,
    //    only when visible (H1). When closed, the editor fills this space.
    if (explorerPanel_)
    {
        if (explorerPanel_->isVisible())
            explorerPanel_->setBounds(b.removeFromLeft(kExplorerWidth));
        else
            explorerPanel_->setBounds(juce::Rectangle<int>());
    }

    // 3. Chat sidebar — fixed 320 px on the right (Req 20.1)
    //    Visibility is gated so the editor area fills the freed space when
    //    the ribbon AIAgent button hides it (Agent 2.4), mirroring how the
    //    Explorer panel is gated above.
    if (chatSidebar_)
    {
        if (chatSidebar_->isVisible())
            chatSidebar_->setBounds(b.removeFromRight(320));
        else
            chatSidebar_->setBounds(juce::Rectangle<int>());
    }

    // 3. Visualizer panel — max(height/4, 120) px at the bottom (Req 20.1, 20.3)
    //    Absorbs all vertical slack when the window grows taller.
    if (visualizerPanel_)
    {
        const int vizH = std::max(b.getHeight() / 4, 120);
        visualizerPanel_->setBounds(b.removeFromBottom(vizH));
    }

    // L-3: StatusRibbon — 28 px strip at the very bottom
    if (statusRibbon_)
        statusRibbon_->setBounds(b.removeFromBottom(hathor::ui::StatusRibbon::kRibbonHeight));

    // 4. Editor area — fills the remaining centre region (Req 20.1, 20.3)
    if (editorArea_)
        editorArea_->setBounds(b);

    // Phase G (D2–D4): mascot overlay — bottom-right corner of the editor
    // region. Compact display scale with an 8 px margin; the widget hides
    // itself entirely while NoPet.
    if (petWidget_)
    {
        const int w = hathor::ui::PetWidget::kPetWidth;
        const int h = hathor::ui::PetWidget::kPetHeight;
        petWidget_->setBounds(b.getRight() - w - 8, b.getBottom() - h - 8, w, h);
    }

    // Agent 0.1: welcome overlay covers the entire content area while shown.
    if (welcomeScreen_ != nullptr && welcomeScreen_->isVisible())
        welcomeScreen_->setBounds(content->getLocalBounds());
}

// ---------------------------------------------------------------------------
// closeButtonPressed() — persist bounds and quit (Req 20.5)
// ---------------------------------------------------------------------------

void MainWindow::closeButtonPressed()
{
    // Persist current window bounds and explorer last-directory (A4).
    if (auto* props = appProperties_.getUserSettings())
    {
        props->setValue("windowBounds",
                        getBounds().toString());
        // Agent 0.1: persist the workspace root so relaunch restores it
        // directly (no welcome screen).
        if (!workspaceDir_.empty())
            props->setValue("lastWorkspacePath", juce::String(workspaceDir_));
        // ExplorerPanel::saveLastDirectory is called on setDirectory(),
        // but call it again here to be certain the latest directory is persisted
        // even if setDirectory was never explicitly called.
        if (explorerPanel_)
            props->setValue("explorerLastDirectory",
                            explorerPanel_->directory().getFullPathName());
        props->saveIfNeeded();
    }

    // J-6: Persist ghost completion telemetry (quality metrics) across sessions.
#ifdef HATHOR_ENABLE_GHOST_TELEMETRY
    if (editorArea_)
    {
        juce::File appDataDir = juce::File::getSpecialLocation(
            juce::File::userApplicationDataDirectory);
        juce::File telemetryFile = appDataDir.getChildFile("Hathor/ghost-telemetry.json");
        editorArea_->saveTelemetry(telemetryFile.getFullPathName().toStdString());
    }
#endif

     // 20.7: Persist the editor workspace (open tabs, cursors, slot state).
     saveWorkspace();

     // B6: Persist the chat thread list (so closed tabs survive a restart).
     if (chatSidebar_)
         chatSidebar_->saveChatState();

    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

// ---------------------------------------------------------------------------
// Phase G / D2–D4 — Petdex selection lifecycle
// ---------------------------------------------------------------------------

void MainWindow::applySelectedPet(const std::string& slug)
{
    if (petdexResourceService_ == nullptr || petWidget_ == nullptr)
        return;

    // "No mascot" is an explicit, valid state (D1 requirement 10).
    if (slug.empty())
    {
        petWidget_->clearPet();
        return;
    }

    // Resolve the pet from the currently loaded catalog (the Settings picker
    // only lists catalog pets, so this is normally found).
    hathor::ui::PetdexAttributionSnapshot snapshot;
    bool resolved = false;
    if (editorArea_ != nullptr)
    {
        if (auto* svc = editorArea_->petdexManifestService())
        {
            const auto current = svc->current();
            for (const auto& pet : current.manifest.pets)
            {
                if (pet.slug == slug)
                {
                    snapshot  = hathor::ui::PetdexAttribution::buildSnapshot(pet);
                    resolved  = true;
                    break;
                }
            }
        }
    }

    if (!resolved)
    {
        // D4: attribution cannot be established — the pet must not be drawn.
        // Surface an explicit blocked state instead of silently rendering.
        snapshot.slug       = slug;
        snapshot.canDisplay = false;
        snapshot.notice     = hathor::ui::PetdexAttribution::kMissingAttributionNotice;
    }

    // Persist the D4 record beside the pet's cached resources so the gate
    // re-runs identically on later launches, without the manifest or network.
    petdexResourceService_->writeAttribution(slug, snapshot);

    petWidget_->setSelectedPet(snapshot);

    if (snapshot.canDisplay && snapshot.spritesheetUrl.rfind("http", 0) == 0)
        petdexResourceService_->loadPet(slug, snapshot.spritesheetUrl);
}

void MainWindow::restorePetSelection()
{
    if (petdexResourceService_ == nullptr || petWidget_ == nullptr)
        return;

    const auto* props = appProperties_.getUserSettings();
    if (props == nullptr)
        return;

    const std::string savedPet =
        props->getValue("settings.petSelection").toStdString();
    if (savedPet.empty())
        return;

    hathor::ui::PetdexAttributionSnapshot snap;
    const bool haveSnap = petdexResourceService_->readAttribution(savedPet, snap);
    if (haveSnap && snap.canDisplay
        && snap.spritesheetUrl.rfind("http", 0) == 0)
    {
        // D4 gate passed from the persisted record: display the pet, then
        // load the sprite — from disk when cached (offline launch, no
        // network), or by completing the explicitly-selected download in the
        // background (never a manifest/catalog fetch).
        petWidget_->setSelectedPet(snap);
        petdexResourceService_->loadPet(savedPet, snap.spritesheetUrl);
    }
    else
    {
        // D4 blocks display. Distinguish an unreadable record (missing/
        // corrupt snapshot file) from a record whose attribution could not be
        // established, so the notice explains the actual reason.
        hathor::ui::PetdexAttributionSnapshot blocked;
        blocked.slug       = savedPet;
        blocked.canDisplay = false;
        blocked.notice     = haveSnap
            ? hathor::ui::PetdexAttribution::kMissingAttributionNotice
            : "This pet cannot be displayed: its saved attribution record "
              "could not be read, so attribution cannot be established.";
        petWidget_->setSelectedPet(blocked);
    }
}

// ---------------------------------------------------------------------------
// makePropertiesOptions() (Req 20.5)
// ---------------------------------------------------------------------------

juce::PropertiesFile::Options MainWindow::makePropertiesOptions()
{
    juce::PropertiesFile::Options opts;
    opts.applicationName     = "Hathor";
    opts.filenameSuffix      = ".props";
    opts.folderName          = "Hathor";
    opts.storageFormat       = juce::PropertiesFile::storeAsXML;
    opts.commonToAllUsers    = false;
    opts.ignoreCaseOfKeyNames = false;
    return opts;
}

// ---------------------------------------------------------------------------
// resolveInitialBounds() (Req 20.5)
// ---------------------------------------------------------------------------

juce::Rectangle<int> MainWindow::resolveInitialBounds()
{
    // Default: centred 1024×768.
    auto defaultBounds = []() -> juce::Rectangle<int>
    {
        return juce::Desktop::getInstance()
                   .getDisplays()
                   .getPrimaryDisplay()
                   ->userArea
                   .withSizeKeepingCentre(1024, 768);
    };

    const auto* props = appProperties_.getUserSettings();
    if (props == nullptr)
        return defaultBounds();

    const juce::String stored = props->getValue("windowBounds");
    if (stored.isEmpty())
        return defaultBounds();

    // juce::Rectangle<int>::fromString parses "x y w h" format.
    const juce::Rectangle<int> r =
        juce::Rectangle<int>::fromString(stored);

    if (r.isEmpty())
        return defaultBounds();

    // Only use stored bounds if they are actually visible on a display (Req 20.5).
    if (!boundsIntersectsDisplays(r))
        return defaultBounds();

    // Clamp to minimum size in case the stored size is smaller (e.g. from an
    // older version or manual property edit).
    return r.withSize(std::max(r.getWidth(),  1024),
                      std::max(r.getHeight(), 768));
}

// ---------------------------------------------------------------------------
// boundsIntersectsDisplays() (Req 20.5)
// ---------------------------------------------------------------------------

/*static*/
bool MainWindow::boundsIntersectsDisplays(const juce::Rectangle<int>& bounds)
{
    const auto& displays = juce::Desktop::getInstance().getDisplays();

    for (const auto& d : displays.displays)
    {
        // Check the stored bounds against each display's total area
        // (including any menu bar / taskbar region) to avoid falsely
        // treating a window just off the bottom of the dock as off-screen.
        if (d.totalArea.intersects(bounds))
            return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// keyPressed — L-1 global keyboard shortcuts (Req §5)
// ---------------------------------------------------------------------------

bool MainWindow::keyPressed(const juce::KeyPress& key)
{
    if (!editorArea_)
        return false;

    // Try the action registry first (L-1: keyboard shortcut/action registry)
    if (auto* reg = editorArea_->actionRegistry())
    {
        // Convert JUCE KeyPress to our portable KeyEquivalent
        hathor::ui::KeyEquivalent ke;
        if (key.getModifiers().isCtrlDown())
            ke.modifiers = ke.modifiers | hathor::ui::ModFlag::Ctrl;
        if (key.getModifiers().isCommandDown())
            ke.modifiers = ke.modifiers | hathor::ui::ModFlag::Cmd;
        if (key.getModifiers().isAltDown())
            ke.modifiers = ke.modifiers | hathor::ui::ModFlag::Alt;
        if (key.getModifiers().isShiftDown())
            ke.modifiers = ke.modifiers | hathor::ui::ModFlag::Shift;

        // Map JUCE key codes to our key strings
        if (key.getKeyCode() >= 'A' && key.getKeyCode() <= 'Z')
        {
            char c = static_cast<char>(key.getKeyCode());
            if (key.getModifiers().isShiftDown())
                ke.key = std::string(1, c);  // uppercase letter
            else
                ke.key = std::string(1, static_cast<char>(std::tolower(c)));
        }
        else if (key.getKeyCode() >= 'a' && key.getKeyCode() <= 'z')
        {
            ke.key = std::string(1, static_cast<char>(key.getKeyCode()));
        }
        else if (key.getKeyCode() >= 0xF700 && key.getKeyCode() <= 0xF70B)  // F1-F12
        {
            ke.key = "F" + std::to_string(key.getKeyCode() - 0xF700 + 1);
        }
        else
        {
            int kc = key.getKeyCode();
            if (kc == juce::KeyPress::returnKey)    ke.key = "Enter";
            else if (kc == juce::KeyPress::tabKey)  ke.key = "Tab";
            else if (kc == juce::KeyPress::escapeKey) ke.key = "Escape";
            else if (kc == juce::KeyPress::backspaceKey) ke.key = "Backspace";
            else if (kc == juce::KeyPress::deleteKey) ke.key = "Delete";
            else if (kc == juce::KeyPress::upKey)   ke.key = "Up";
            else if (kc == juce::KeyPress::downKey) ke.key = "Down";
            else if (kc == juce::KeyPress::leftKey) ke.key = "Left";
            else if (kc == juce::KeyPress::rightKey) ke.key = "Right";
            else if (kc == juce::KeyPress::spaceKey) ke.key = "Space";
            else return false;
        }

        if (reg->dispatchKey(ke))
            return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// 0.2 — Workspace lifecycle (Open Folder + recent-projects MRU)
// ---------------------------------------------------------------------------

void MainWindow::openFolderChooser()
{
    folderChooser_ = std::make_unique<juce::FileChooser>(
        "Open Folder…",
        juce::File(workspaceDir_.empty()
                       ? juce::File::getSpecialLocation(
                             juce::File::userHomeDirectory).getFullPathName()
                       : juce::String(workspaceDir_)));

    folderChooser_->launchAsync(
        juce::FileBrowserComponent::openMode |
            juce::FileBrowserComponent::canSelectDirectories,
        [this](const juce::FileChooser& fc)
        {
            const juce::File dir = fc.getResult();
            if (dir.isDirectory())
                switchWorkspace(dir);
            folderChooser_.reset();
        });
}

void MainWindow::switchWorkspace(const juce::File& dir)
{
    if (!dir.isDirectory())
        return;

    const std::string newDir = dir.getFullPathName().toStdString();

    pushRecentProject(newDir);

    if (newDir == workspaceDir_)
        return;

    // Close tabs that belong to the old workspace first — dirty buffers go
    // through the existing Save/Discard/Cancel prompt (Req 22.7).
    const std::string oldDir = workspaceDir_;
    if (editorArea_ && !oldDir.empty())
        editorArea_->closeTabsUnderRoot(std::filesystem::path(oldDir));

    // Re-root the explorer (persists explorerLastDirectory) and the editor
    // area (workspace search + quick open).
    if (explorerPanel_)
        explorerPanel_->setDirectory(dir);
    if (editorArea_)
        editorArea_->setWorkspaceRoot(std::filesystem::path(newDir));

    workspaceDir_ = newDir;

    // Agent 0.1: persist the chosen root so relaunch restores it directly.
    if (auto* props = appProperties_.getUserSettings())
    {
        props->setValue("lastWorkspacePath", juce::String(newDir));
        props->saveIfNeeded();
    }

    // A workspace now exists — dismiss the welcome overlay if it was up.
    if (welcomeScreen_ != nullptr && welcomeScreen_->isVisible())
    {
        welcomeScreen_->setVisible(false);
        resized();
    }
}

std::vector<std::string> MainWindow::loadRecentProjects()
{
    std::vector<std::string> out;
    auto* props = appProperties_.getUserSettings();
    if (props == nullptr)
        return out;

    juce::StringArray entries;
    entries.addLines(props->getValue("recent.projects"));
    for (const auto& line : entries)
    {
        std::string p = line.toStdString();
        while (!p.empty() && (p.back() == '\r'))
            p.pop_back();
        if (!p.empty() && juce::File(p).isDirectory())
            out.push_back(p);
    }
    return out;
}

void MainWindow::pushRecentProject(const std::string& path)
{
    if (auto* props = appProperties_.getUserSettings())
    {
        // Dedup, most-recent-first, capped at 10.
        std::vector<std::string> mru;
        mru.push_back(path);
        for (const auto& p : loadRecentProjects())
        {
            if (p != path && mru.size() < 10)
                mru.push_back(p);
        }

        juce::String joined;
        for (const auto& p : mru)
        {
            if (joined.isNotEmpty())
                joined += "\n";
            joined += juce::String(p);
        }
        props->setValue("recent.projects", joined);
        props->saveIfNeeded();
    }
}

// ---------------------------------------------------------------------------
// Agent 0.1 — Welcome screen (no persisted workspace at launch)
// ---------------------------------------------------------------------------

void MainWindow::showWelcomeScreen()
{
    if (welcomeScreen_ == nullptr)
    {
        welcomeScreen_ = std::make_unique<hathor::ui::WelcomeScreen>();

        welcomeScreen_->onWorkspaceChosen =
            [this](juce::File dir)
            {
                switchWorkspace(dir);

                // Chat threads started before a workspace existed should
                // follow the newly chosen root.
                if (chatSidebar_ && !agentExePath_.empty())
                    chatSidebar_->restartAllThreads(
                        agentExePath_, workspaceDir_, hathorMcpPath_);
            };
    }

    juce::StringArray recent;
    for (const auto& p : loadRecentProjects())
        recent.add(juce::String(p));
    welcomeScreen_->setRecentPaths(recent);

    if (auto* content = getContentComponent())
        content->addAndMakeVisible(welcomeScreen_.get());
    welcomeScreen_->setVisible(true);
    resized();
}

void MainWindow::refreshRecentActions()
{
    auto* reg = editorArea_ != nullptr ? editorArea_->actionRegistry() : nullptr;
    if (reg == nullptr)
        return;

    const std::vector<std::string> recent = loadRecentProjects();
    for (std::size_t i = 0; i < recent.size(); ++i)
    {
        const std::string id = "workspace.openRecent." + std::to_string(i);
        reg->registerAction(id, "Open Recent: " + recent[i], "File",
                            "Reopen this project as the workspace");
        reg->setCallback(id, [this, path = recent[i]]()
        {
            switchWorkspace(juce::File(path));
        });
    }
}

// ---------------------------------------------------------------------------
// 20.7 — Workspace persistence (save / restore)
// ---------------------------------------------------------------------------

void MainWindow::saveWorkspace()
{
    if (!editorArea_)
        return;

    hathor::ui::WorkspaceSession session = editorArea_->saveWorkspace();
    std::string json = session.toJson();

    if (auto* props = appProperties_.getUserSettings())
    {
        props->setValue("workspaceData",
                        juce::String(json));
        props->setValue("workspaceSchemaVersion",
                        hathor::ui::WorkspaceSession::kSchemaVersion);
        props->saveIfNeeded();
    }
}

void MainWindow::restoreWorkspace()
{
    if (!editorArea_)
        return;

    if (auto* props = appProperties_.getUserSettings())
    {
        juce::String json = props->getValue("workspaceData");
        if (!json.isEmpty())
        {
            if (auto session = hathor::ui::WorkspaceSession::fromJson(json.toStdString()))
                editorArea_->restoreWorkspace(*session, &appProperties_);
        }
    }
}
