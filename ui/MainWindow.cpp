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

// The following headers are not yet created (sibling tasks 3.8, 5.1).
// Stub definitions below allow MainWindow to compile until those tasks land.
// Uncomment each #include as the corresponding task is completed:
//
// TODO: include ui/ChatSidebar.hpp when available       (task 5.1)
// TODO: include ui/VisualizerPanel.hpp when available   (task 3.8)

// UITimer (task 3.7) — real implementation is now available.
#include "UITimer.hpp"

#ifndef HATHOR_CHAT_SIDEBAR_DEFINED
#define HATHOR_CHAT_SIDEBAR_DEFINED
namespace hathor::ui {
/// Stub ChatSidebar — replaced by ChatSidebar.hpp (task 5.1).
class ChatSidebar : public juce::Component {
public:
    ChatSidebar(AudioEngine&, hathor::control::ControlInterface&) {}
};
} // namespace hathor::ui
#endif

#ifndef HATHOR_VISUALIZER_PANEL_DEFINED
#define HATHOR_VISUALIZER_PANEL_DEFINED
namespace hathor::ui {
/// Stub VisualizerPanel — replaced by VisualizerPanel.hpp (task 3.8).
class VisualizerPanel : public juce::Component {
public:
    explicit VisualizerPanel(AudioEngine&) {}
    void updateFrame(double, const std::vector<hathor::Event<hathor::ParamMap>>&) {}
};
} // namespace hathor::ui
#endif

// ==========================================================================
// HathorLookAndFeel
// ==========================================================================

HathorLookAndFeel::HathorLookAndFeel()
{
    // -----------------------------------------------------------------------
    // Window / component backgrounds
    // -----------------------------------------------------------------------
    setColour(juce::ResizableWindow::backgroundColourId,
              juce::Colour(kColourBackground));

    setColour(juce::DocumentWindow::backgroundColourId,
              juce::Colour(kColourBackground));

    // -----------------------------------------------------------------------
    // Generic component / label text
    // -----------------------------------------------------------------------
    setColour(juce::Label::textColourId,
              juce::Colour(kColourText));

    setColour(juce::Label::backgroundColourId,
              juce::Colour(kColourBackground));

    // -----------------------------------------------------------------------
    // TextEditor (chat input, search bar, etc.)
    // -----------------------------------------------------------------------
    setColour(juce::TextEditor::backgroundColourId,
              juce::Colour(kColourSurface));

    setColour(juce::TextEditor::textColourId,
              juce::Colour(kColourText));

    setColour(juce::TextEditor::outlineColourId,
              juce::Colour(0xff3c3c3c));

    setColour(juce::TextEditor::focusedOutlineColourId,
              juce::Colour(kColourAccent));

    setColour(juce::TextEditor::highlightColourId,
              juce::Colour(kColourAccent).withAlpha(0.4f));

    setColour(juce::TextEditor::highlightedTextColourId,
              juce::Colour(kColourText));

    setColour(juce::CaretComponent::caretColourId,
              juce::Colour(kColourAccent));

    // -----------------------------------------------------------------------
    // ListBox / TreeView (Explorer panel)
    // -----------------------------------------------------------------------
    setColour(juce::ListBox::backgroundColourId,
              juce::Colour(kColourBackground));

    setColour(juce::ListBox::outlineColourId,
              juce::Colour(0xff3c3c3c));

    setColour(juce::TreeView::backgroundColourId,
              juce::Colour(kColourBackground));

    setColour(juce::TreeView::linesColourId,
              juce::Colour(0xff555555));

    setColour(juce::TreeView::selectedItemBackgroundColourId,
              juce::Colour(kColourAccent).withAlpha(0.3f));

    setColour(juce::TreeView::dragAndDropIndicatorColourId,
              juce::Colour(kColourAccent));

    // -----------------------------------------------------------------------
    // CodeEditorComponent
    // -----------------------------------------------------------------------
    setColour(juce::CodeEditorComponent::backgroundColourId,
              juce::Colour(kColourBackground));

    setColour(juce::CodeEditorComponent::defaultTextColourId,
              juce::Colour(kColourText));

    setColour(juce::CodeEditorComponent::highlightColourId,
              juce::Colour(kColourAccent).withAlpha(0.3f));

    setColour(juce::CodeEditorComponent::lineNumberBackgroundId,
              juce::Colour(kColourSurface));

    setColour(juce::CodeEditorComponent::lineNumberTextId,
              juce::Colour(0xff858585));

    // -----------------------------------------------------------------------
    // TabbedComponent
    // -----------------------------------------------------------------------
    setColour(juce::TabbedComponent::backgroundColourId,
              juce::Colour(kColourBackground));

    setColour(juce::TabbedComponent::outlineColourId,
              juce::Colour(0xff3c3c3c));

    setColour(juce::TabbedButtonBar::tabTextColourId,
              juce::Colour(kColourText));

    setColour(juce::TabbedButtonBar::tabOutlineColourId,
              juce::Colour(0xff3c3c3c));

    setColour(juce::TabbedButtonBar::frontTextColourId,
              juce::Colour(0xffffffff));

    // -----------------------------------------------------------------------
    // ScrollBar
    // -----------------------------------------------------------------------
    setColour(juce::ScrollBar::backgroundColourId,
              juce::Colour(kColourBackground));

    setColour(juce::ScrollBar::thumbColourId,
              juce::Colour(0xff555555));

    setColour(juce::ScrollBar::trackColourId,
              juce::Colour(kColourSurface));

    // -----------------------------------------------------------------------
    // Slider (BPM, Master Gain in ChatSidebar)
    // -----------------------------------------------------------------------
    setColour(juce::Slider::backgroundColourId,
              juce::Colour(kColourSurface));

    setColour(juce::Slider::thumbColourId,
              juce::Colour(kColourAccent));

    setColour(juce::Slider::trackColourId,
              juce::Colour(0xff3c3c3c));

    setColour(juce::Slider::rotarySliderFillColourId,
              juce::Colour(kColourAccent));

    setColour(juce::Slider::rotarySliderOutlineColourId,
              juce::Colour(0xff3c3c3c));

    setColour(juce::Slider::textBoxTextColourId,
              juce::Colour(kColourText));

    setColour(juce::Slider::textBoxBackgroundColourId,
              juce::Colour(kColourSurface));

    setColour(juce::Slider::textBoxOutlineColourId,
              juce::Colour(0xff3c3c3c));

    setColour(juce::Slider::textBoxHighlightColourId,
              juce::Colour(kColourAccent).withAlpha(0.4f));

    // -----------------------------------------------------------------------
    // ComboBox
    // -----------------------------------------------------------------------
    setColour(juce::ComboBox::backgroundColourId,
              juce::Colour(kColourSurface));

    setColour(juce::ComboBox::textColourId,
              juce::Colour(kColourText));

    setColour(juce::ComboBox::outlineColourId,
              juce::Colour(0xff3c3c3c));

    setColour(juce::ComboBox::buttonColourId,
              juce::Colour(kColourSurface));

    setColour(juce::ComboBox::arrowColourId,
              juce::Colour(kColourText));

    // -----------------------------------------------------------------------
    // PopupMenu
    // -----------------------------------------------------------------------
    setColour(juce::PopupMenu::backgroundColourId,
              juce::Colour(kColourSurface));

    setColour(juce::PopupMenu::textColourId,
              juce::Colour(kColourText));

    setColour(juce::PopupMenu::highlightedBackgroundColourId,
              juce::Colour(kColourAccent).withAlpha(0.3f));

    setColour(juce::PopupMenu::highlightedTextColourId,
              juce::Colour(kColourText));

    setColour(juce::PopupMenu::headerTextColourId,
              juce::Colour(0xff858585));

    // -----------------------------------------------------------------------
    // AlertWindow / DialogWindow
    // -----------------------------------------------------------------------
    setColour(juce::AlertWindow::backgroundColourId,
              juce::Colour(kColourSurface));

    setColour(juce::AlertWindow::textColourId,
              juce::Colour(kColourText));

    setColour(juce::AlertWindow::outlineColourId,
              juce::Colour(0xff3c3c3c));

    // -----------------------------------------------------------------------
    // TextButton
    // -----------------------------------------------------------------------
    setColour(juce::TextButton::buttonColourId,
              juce::Colour(kColourSurface));

    setColour(juce::TextButton::buttonOnColourId,
              juce::Colour(kColourAccent));

    setColour(juce::TextButton::textColourOffId,
              juce::Colour(kColourText));

    setColour(juce::TextButton::textColourOnId,
              juce::Colour(0xffffffff));

    // -----------------------------------------------------------------------
    // TooltipWindow
    // -----------------------------------------------------------------------
    setColour(juce::TooltipWindow::backgroundColourId,
              juce::Colour(kColourSurface));

    setColour(juce::TooltipWindow::textColourId,
              juce::Colour(kColourText));

    setColour(juce::TooltipWindow::outlineColourId,
              juce::Colour(0xff3c3c3c));
}

// ==========================================================================
// MainWindow
// ==========================================================================

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

MainWindow::MainWindow(AudioEngine& audio,
                       hathor::control::ControlInterface& ci)
    : juce::DocumentWindow(
          "Hathor",
          juce::Colour(HathorLookAndFeel::kColourBackground),
          juce::DocumentWindow::allButtons)
    , audio_(audio)
    , ci_(ci)
{
    // Apply the dark theme to this window and all its children.
    setLookAndFeel(&lookAndFeel_);

    // -----------------------------------------------------------------------
    // Minimum window size (Req 20.5)
    // -----------------------------------------------------------------------
    setResizeLimits(1024, 768, 10000, 10000);

    // -----------------------------------------------------------------------
    // Instantiate child components (Req 20.1, 20.4)
    // These are JUCE native components — no embedded webview / Electron.
    //
    // ActivityRibbon and ExplorerPanel use their default constructors (task 3.2).
    // EditorArea, ChatSidebar, VisualizerPanel, UITimer use stub constructors
    // until their respective tasks are implemented.
    // -----------------------------------------------------------------------
    activityRibbon_  = std::make_unique<hathor::ui::ActivityRibbon>();
    editorArea_      = std::make_unique<hathor::ui::EditorArea>(audio_, ci_);
    chatSidebar_     = std::make_unique<hathor::ui::ChatSidebar>(audio_, ci_);
    visualizerPanel_ = std::make_unique<hathor::ui::VisualizerPanel>(audio_);

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
    // sliderPanelStub_ is a stub SliderPanel (task 3.9 not yet implemented).
    // When SliderPanel.hpp lands and ChatSidebar hosts the real panel,
    // pass the real reference here and remove sliderPanelStub_.
    // -----------------------------------------------------------------------
    uiTimer_ = std::make_unique<hathor::ui::UITimer>(
        audio_.visualizerBuffer(),
        *visualizerPanel_,
        sliderPanelStub_,
        audio_);
    uiTimer_->startTimerHz(60);

    setVisible(true);
}

MainWindow::~MainWindow()
{
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
