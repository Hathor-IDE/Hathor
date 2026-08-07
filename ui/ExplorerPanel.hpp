// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ExplorerPanel.hpp — sliding file-tree panel listing .hathor files.
 *
 * The panel is toggled open/closed by the Explorer button in ActivityRibbon.
 * When open it lists all .hathor files in the project directory (most-recently-
 * opened file's directory, or the application launch directory if no file has
 * been opened yet).
 *
 * Clicking a file calls onFileClicked(juce::File), which the owner (MainWindow /
 * EditorArea) uses to open the file in a new tab or focus an existing tab.
 *
 * Requirements: 21.3, 21.4, 24.1
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

namespace hathor::ui {

/**
 * ExplorerPanel
 *
 * A juce::Component that hosts a juce::ListBox listing .hathor files.
 * Owners should:
 *   1. Call setDirectory(juce::File) whenever the project directory changes.
 *   2. Install onFileClicked to respond to file selections.
 */
class ExplorerPanel : public juce::Component,
                      private juce::ListBoxModel
{
public:
    explicit ExplorerPanel();
    ~ExplorerPanel() override = default;

    //==========================================================================
    // Callback — installed by MainWindow / EditorArea.
    // Called on the JUCE message thread when the user clicks a .hathor file.
    std::function<void(juce::File)> onFileClicked;

    //==========================================================================
    // Directory management

    /// Set the directory to scan for .hathor files.
    /// Triggers an immediate refresh of the file list.
    void setDirectory(const juce::File& dir);

    juce::File directory() const noexcept { return directory_; }

    /// Re-scan the current directory and refresh the list.
    void refresh();

    //==========================================================================
    // juce::Component overrides
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    //==========================================================================
    // Colours
    static constexpr juce::uint32 kBgColour       = 0xff1e1e1eu; ///< dark bg
    static constexpr juce::uint32 kHeaderBgColour  = 0xff252526u; ///< slightly lighter header
    static constexpr juce::uint32 kHeaderTextColour = 0xffccccccu; ///< header label
    static constexpr juce::uint32 kItemTextColour   = 0xffd4d4d4u; ///< file name text
    static constexpr juce::uint32 kSelBgColour      = 0xff094771u; ///< selected row bg (VS Code blue-dark)
    static constexpr juce::uint32 kSelTextColour    = 0xffffffffu; ///< selected row text

    //==========================================================================
    // Layout
    static constexpr int kHeaderHeight = 28;
    static constexpr int kRowHeight    = 22;

    //==========================================================================
    // juce::ListBoxModel overrides
    int  getNumRows() override;
    void paintListBoxItem(int rowNumber, juce::Graphics& g,
                          int width, int height,
                          bool rowIsSelected) override;
    void listBoxItemClicked(int row, const juce::MouseEvent& e) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent& e) override;

    //==========================================================================
    // Data
    juce::File                 directory_;
    std::vector<juce::File>    files_;     ///< sorted list of .hathor files found

    juce::ListBox              listBox_;
    juce::Label                headerLabel_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ExplorerPanel)
};

} // namespace hathor::ui
