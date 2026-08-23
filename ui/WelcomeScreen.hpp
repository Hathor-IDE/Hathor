// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * WelcomeScreen.hpp — startup overlay shown when no workspace was persisted
 * (Agent 0.1 / audit P1–P4).
 *
 * Offers three entry points:
 *   - Open Folder…   : native directory chooser → onWorkspaceChosen
 *   - New Project…   : native save dialog for folder name + parent, then the
 *                      owner scaffolds the project directory on disk
 *   - Open Recent…   : popup menu of the persisted MRU list (may be empty)
 *
 * The component paints an opaque surface so it fully covers the IDE shell
 * until a workspace has been chosen.
 */

#include <juce_gui_basics/juce_gui_basics.h>

namespace hathor::ui {

class WelcomeScreen : public juce::Component
{
public:
    WelcomeScreen();

    /// Replace the MRU entries shown by "Open Recent…" (most-recent-first).
    void setRecentPaths(const juce::StringArray& paths);

    /// Called with the chosen (existing or freshly scaffolded) directory.
    /// Installed by MainWindow.
    std::function<void(juce::File)> onWorkspaceChosen;

    // juce::Component
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void openFolder();
    void newProject();
    void showRecent();

    juce::Label      titleLabel_;
    juce::Label      subtitleLabel_;
    juce::TextButton openButton_{"Open Folder..."};
    juce::TextButton newButton_{"New Project..."};
    juce::TextButton recentButton_{"Open Recent..."};

    juce::StringArray recentPaths_;

    /// Must outlive the async callback (JUCE idiom for modal choosers).
    std::unique_ptr<juce::FileChooser> chooser_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WelcomeScreen)
};

} // namespace hathor::ui
