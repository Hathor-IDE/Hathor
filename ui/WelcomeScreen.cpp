// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * WelcomeScreen.cpp — implementation of the startup workspace overlay (Agent 0.1).
 */

#include "WelcomeScreen.hpp"

#include "HathorLookAndFeel.hpp"

#include <filesystem>

namespace hathor::ui {

namespace {

/// Minimal starter .hathor file written by "New Project…" scaffolding.
constexpr const char* kStarterHathorFile =
    "[hathor]\n"
    "label = My First Song\n"
    "bpm = 120.0\n"
    "\n"
    "// Welcome to Hathor! Write your pattern below.\n";

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WelcomeScreen::WelcomeScreen()
{
    const auto& palette = HathorLookAndFeel::defaultPalette();

    titleLabel_.setText("Hathor", juce::dontSendNotification);
    titleLabel_.setFont(HathorLookAndFeel::fontBold(32.0f));
    titleLabel_.setColour(juce::Label::textColourId, palette.textPrimary);
    titleLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(titleLabel_);

    subtitleLabel_.setText("Open a folder to start live-coding, or create a new project.",
                           juce::dontSendNotification);
    subtitleLabel_.setFont(HathorLookAndFeel::fontRegular(15.0f));
    subtitleLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    subtitleLabel_.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(subtitleLabel_);

    for (auto* btn : { &openButton_, &newButton_, &recentButton_ })
    {
        btn->setColour(juce::TextButton::buttonColourId, palette.surfaceHigh);
        btn->setColour(juce::TextButton::textColourOffId, palette.textPrimary);
        addAndMakeVisible(btn);
    }

    openButton_.onClick   = [this]() { openFolder(); };
    newButton_.onClick    = [this]() { newProject(); };
    recentButton_.onClick = [this]() { showRecent(); };

    setOpaque(true);
}

// ---------------------------------------------------------------------------
// MRU list
// ---------------------------------------------------------------------------

void WelcomeScreen::setRecentPaths(const juce::StringArray& paths)
{
    recentPaths_ = paths;
    recentButton_.setEnabled(!recentPaths_.isEmpty());
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void WelcomeScreen::openFolder()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Open Project Folder",
        juce::File::getSpecialLocation(juce::File::userHomeDirectory));

    const int flags = juce::FileBrowserComponent::openMode
                    | juce::FileBrowserComponent::canSelectDirectories;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const juce::File dir = fc.getResult();
        chooser_.reset();
        if (dir.isDirectory() && onWorkspaceChosen)
            onWorkspaceChosen(dir);
    });
}

void WelcomeScreen::newProject()
{
    // A native "save" directory chooser collects both the project name and the
    // parent directory in one step.
    chooser_ = std::make_unique<juce::FileChooser>(
        "New Project — choose a name and location",
        juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile("NewProject"));

    const int flags = juce::FileBrowserComponent::saveMode
                    | juce::FileBrowserComponent::canSelectDirectories
                    | juce::FileBrowserComponent::doNotClearFileNameOnRootChange;

    chooser_->launchAsync(flags, [this](const juce::FileChooser& fc)
    {
        const juce::File dir = fc.getResult();
        chooser_.reset();
        if (!dir.isDirectory())
            return;

        // Scaffold the project: directory, starter .hathor file, samples/.
        if (!dir.isDirectory())
            dir.createDirectory();
        if (!dir.isDirectory())
            return;

        dir.getChildFile("samples").createDirectory();

        juce::File songFile = dir.getChildFile("song.hathor");
        if (!songFile.existsAsFile())
            songFile.replaceWithText(kStarterHathorFile);

        if (onWorkspaceChosen)
            onWorkspaceChosen(dir);
    });
}

void WelcomeScreen::showRecent()
{
    juce::PopupMenu menu;
    for (int i = 0; i < recentPaths_.size(); ++i)
        menu.addItem(i + 1, recentPaths_[i]);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&recentButton_),
                       [this](int result)
                       {
                           if (result <= 0 || result > recentPaths_.size())
                               return;
                           const juce::File dir(recentPaths_[result - 1]);
                           if (dir.isDirectory() && onWorkspaceChosen)
                               onWorkspaceChosen(dir);
                       });
}

// ---------------------------------------------------------------------------
// Painting / layout
// ---------------------------------------------------------------------------

void WelcomeScreen::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::defaultPalette();
    g.fillAll(palette.background);
    g.setColour(palette.surfaceHighest);
    g.drawRect(getLocalBounds());
}

void WelcomeScreen::resized()
{
    auto b = getLocalBounds();
    constexpr int kColumnWidth = 320;
    b.reduce(std::max(0, (b.getWidth() - kColumnWidth) / 2), 0);

    auto centre = juce::FlexBox{};
    centre.flexDirection = juce::FlexBox::Direction::column;
    centre.alignItems = juce::FlexBox::AlignItems::center;
    centre.justifyContent = juce::FlexBox::JustifyContent::center;
    centre.items.add(
        juce::FlexItem(titleLabel_).withHeight(44.0f),
        juce::FlexItem(subtitleLabel_).withHeight(24.0f).withMargin({ 0, 0, 16, 0 }),
        juce::FlexItem(openButton_).withHeight(36.0f).withWidth(240.0f).withMargin(6.0f),
        juce::FlexItem(newButton_).withHeight(36.0f).withWidth(240.0f).withMargin(6.0f),
        juce::FlexItem(recentButton_).withHeight(36.0f).withWidth(240.0f).withMargin(6.0f));

    juce::FlexBox root;
    root.items.add(juce::FlexItem(centre).withFlex(1.0f));
    root.performLayout(b);
}

} // namespace hathor::ui
