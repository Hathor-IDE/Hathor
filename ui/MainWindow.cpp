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
          juce::Colour(HathorLookAndFeel::Colours::background),
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
    editorArea_      = std::make_unique<hathor::ui::EditorArea>(audio_, ci_);
    chatSidebar_     = std::make_unique<hathor::ui::ChatSidebar>(audio_, ci_);
    visualizerPanel_ = std::make_unique<hathor::ui::VisualizerPanel>(audio_);

    // Task 3.9: Create real SliderPanel with ControlInterface for dispatching.
    sliderPanel_ = std::make_unique<hathor::ui::SliderPanel>(ci_);

    // -----------------------------------------------------------------------
    // Create AcpAgentSession and wire it to ChatSidebar (Req 32.1, 32.3)
    // -----------------------------------------------------------------------
    agentSession_ = std::make_unique<hathor::ui::AcpAgentSession>();

    // Determine the project directory (cwd at launch time).
    const std::string projectDir =
        juce::File::getCurrentWorkingDirectory().getFullPathName().toStdString();

    // Wire all callbacks before start() — setSession() registers the handlers.
    chatSidebar_->setSession(*agentSession_, agentExePath, projectDir, hathorMcpPath);

    // Start the session if a path was provided (Req 32.1).
    if (!agentExePath.empty())
        agentSession_->start(agentExePath, projectDir, hathorMcpPath);

    // Add child components to the content component (DocumentWindow wraps one
    // content component; we use a plain Component as the layout host).
    auto* content = new juce::Component();
    content->addAndMakeVisible(*activityRibbon_);
    content->addAndMakeVisible(*editorArea_);
    content->addAndMakeVisible(*chatSidebar_);
    content->addAndMakeVisible(*visualizerPanel_);

    setContentOwned(content, false);
    setUsingNativeTitleBar(true);
    setResizable(true, false);

    // -----------------------------------------------------------------------
    // Restore or compute initial window bounds (Req 20.5)
    // -----------------------------------------------------------------------
    appProperties_.setStorageParameters(makePropertiesOptions());
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

    // 2. Chat sidebar — fixed 320 px on the right (Req 20.1)
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
    // Persist current window bounds.
    if (auto* props = appProperties_.getUserSettings())
    {
        props->setValue("windowBounds",
                        getBounds().toString());
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
