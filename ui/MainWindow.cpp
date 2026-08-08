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

// UITimer (task 3.7) — real implementation is now available.
#include "UITimer.hpp"

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
    chatSidebar_     = std::make_unique<hathor::ui::ChatSidebar>(audio_, ci_);
    visualizerPanel_ = std::make_unique<hathor::ui::VisualizerPanel>(audio_);

    // Task 3.9: Create real SliderPanel with ControlInterface for dispatching.
    sliderPanel_ = std::make_unique<hathor::ui::SliderPanel>(ci_);

    // -----------------------------------------------------------------------
    // Create AcpAgentSession and wire it to ChatSidebar (Req 32.1, 32.3)
    // -----------------------------------------------------------------------
    agentSession_ = std::make_unique<hathor::ui::AcpAgentSession>();

    // -----------------------------------------------------------------------
    // Wire MCP/control commands received on the Unix socket to ControlInterface
    // (Phase 2.5 H0).  The handler is invoked on the socket accept-loop worker
    // thread; dispatchWithCallback() routes each command through the normal
    // command handlers and returns the JSON result via the response sink, which
    // forwards it back over the socket.
    // -----------------------------------------------------------------------
    agentSession_->setMcpCommandHandler(
        [this](std::string commandLine, std::function<void(std::string)> respond)
        {
            ci_.dispatchWithCallback(
                commandLine,
                [respond = std::move(respond)](nlohmann::json result)
                {
                    respond(result.dump());
                });
        });

    // Determine the project directory (cwd at launch time).
    const std::string projectDir =
        juce::File::getCurrentWorkingDirectory().getFullPathName().toStdString();

    // Wire all callbacks before start() — setSession() registers the handlers.
    chatSidebar_->setSession(*agentSession_, agentExePath, projectDir, hathorMcpPath);

    // Start the session if a path was provided (Req 32.1).
    if (!agentExePath.empty())
        agentSession_->start(agentExePath, projectDir, hathorMcpPath);

    // -----------------------------------------------------------------------
    // Wire ActivityRibbon panel toggles (H1: Explorer)
    // The callback toggles the explorer open/closed and syncs the ribbon's
    // active-button accent highlight via setActivePanel().
    // -----------------------------------------------------------------------
    activityRibbon_->onPanelToggled =
        [this](hathor::ui::Panel panel)
        {
            // Only the Explorer is wired in H1; other panels are no-ops for now.
            if (panel == hathor::ui::Panel::Explorer)
            {
                const bool wantsOpen = (activityRibbon_->activePanel() != hathor::ui::Panel::Explorer);
                explorerPanel_->setVisible(wantsOpen);
                activityRibbon_->setActivePanel(wantsOpen ? hathor::ui::Panel::Explorer : hathor::ui::Panel::None);
                resized(); // re-lay-out editor area
            }
             else if (panel == hathor::ui::Panel::None)
             {
                 // Settings button: open or focus the Settings tab (A2).
                 if (editorArea_)
                     editorArea_->openSettingsTab(&appProperties_);
             }
            // Other panels (Search, VersionControl, AIAgent) are not yet
            // implemented — do nothing, preserving active state.
        };

    // Wire ExplorerPanel file clicks → EditorArea::openFile (H1)
    // Clicking a song opens or recovers the corresponding tab.
    explorerPanel_->onFileClicked =
        [this](const juce::File& file)
        {
            if (editorArea_)
                editorArea_->openFile(file);
        };

    // Set up ApplicationProperties early so the ExplorerPanel can persist
    // and restore its last-used root directory (A4).
    appProperties_.setStorageParameters(makePropertiesOptions());
    explorerPanel_->setApplicationProperties(&appProperties_);

    // Restore the last-used root directory if one was persisted; otherwise
    // fall back to the project directory (cwd at launch).
    explorerPanel_->restoreLastDirectoryAndRefresh();
    if (explorerPanel_->directory() == juce::File())
        explorerPanel_->setDirectory(juce::File(projectDir));

    // Add child components to the content component (DocumentWindow wraps one
    // content component; we use a plain Component as the layout host).
    auto* content = new juce::Component();
    content->addAndMakeVisible(*activityRibbon_);
    content->addAndMakeVisible(*explorerPanel_);
    content->addAndMakeVisible(*editorArea_);
    content->addAndMakeVisible(*chatSidebar_);
    content->addAndMakeVisible(*visualizerPanel_);

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

    // -----------------------------------------------------------------------
    // Start UITimer at 60 Hz — audio device is open at this point (Req 28.5)
    // UITimer drains the visualizer ring buffer and syncs slider displays.
    //
    // Signature: UITimer(SpscRingBuffer<128>&, VisualizerPanel&,
    //                    SliderPanel&, AudioEngine&)
    //
    // sliderPanel_ is the real SliderPanel (task 3.9 now implemented).
    // -----------------------------------------------------------------------
    uiTimer_ = std::make_unique<hathor::ui::UITimer>(
        audio_.visualizerBuffer(),
        *visualizerPanel_,
        *sliderPanel_,
        audio_);
    uiTimer_->startTimerHz(60);

    setVisible(true);
}

MainWindow::~MainWindow()
{
    // Stop the agent session before destroying components it references (Req 32.8).
    if (agentSession_)
        agentSession_->stop();

    // Stop the timer before destroying components it references.
    if (uiTimer_)
        uiTimer_->stopTimer();

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
    if (chatSidebar_)
        chatSidebar_->setBounds(b.removeFromRight(320));

    // 3. Visualizer panel — max(height/4, 120) px at the bottom (Req 20.1, 20.3)
    //    Absorbs all vertical slack when the window grows taller.
    if (visualizerPanel_)
    {
        const int vizH = std::max(b.getHeight() / 4, 120);
        visualizerPanel_->setBounds(b.removeFromBottom(vizH));
    }

    // 4. Editor area — fills the remaining centre region (Req 20.1, 20.3)
    if (editorArea_)
        editorArea_->setBounds(b);
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
        // ExplorerPanel::saveLastDirectory is called on setDirectory(),
        // but call it again here to be certain the latest directory is persisted
        // even if setDirectory was never explicitly called.
        if (explorerPanel_)
            props->setValue("explorerLastDirectory",
                            explorerPanel_->directory().getFullPathName());
        props->saveIfNeeded();
    }

    juce::JUCEApplication::getInstance()->systemRequestedQuit();
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
