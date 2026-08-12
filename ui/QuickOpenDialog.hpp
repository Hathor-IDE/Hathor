// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * QuickOpenDialog.hpp — JUCE modal dialog for quick file opening (Ctrl/Cmd+P).
 *
 * Shows a fuzzy-filterable list of workspace files. When a file is selected
 * (Enter or double-click), fires onFileSelected(file).
 *
 * Requirement references: L-2 §1
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <filesystem>
#include <string>
#include <vector>

#include "ExplorerFileTypes.hpp"

namespace hathor::ui {

/**
 * QuickOpenDialog
 *
 * A modal Component placed over the editor area. Contains a text editor for
 * fuzzy filtering and a ListBox showing matching files.
 *
 * The file list is populated at construction from the workspace root.
 * Only supported song file types (.hathor, .ck) and other source files
 * appear in the list.
 */
class QuickOpenDialog : public juce::Component,
                        public juce::TextEditor::Listener,
                        public juce::ListBoxModel
{
public:
    QuickOpenDialog(const std::filesystem::path& workspaceRoot);
    ~QuickOpenDialog() override;

    // Non-copyable
    QuickOpenDialog(QuickOpenDialog&&) = delete;
    QuickOpenDialog& operator=(QuickOpenDialog&&) = delete;

    /** Show the dialog centered over the parent component. */
    void showOver(juce::Component* parent);

    /** Hide the dialog. */
    void hide();

    /** True if currently visible. */
    bool isVisible() const noexcept { return juce::Component::isVisible(); }

    // juce::Component
    void resized() override;
    void paint(juce::Graphics& g) override;

    /** Filter the file list by the given query string (fuzzy match). */
    void setFilter(const juce::String& query);

    /** Get the currently selected file path, or empty if none. */
    std::filesystem::path selectedFile() const;

    /** Move selection up in the filtered list. */
    void selectUp();

    /** Move selection down in the filtered list. */
    void selectDown();

    /** Confirm the selected file (call the callback). */
    bool confirmSelection();

    /** Callback fired when a file is selected (Enter / double-click). */
    std::function<void(const std::filesystem::path&)> onFileSelected;

    /** Callback fired when the dialog is cancelled (Escape). */
    std::function<void()> onCancelled;

private:
    // TextEditor::Listener
    void textEditorTextChanged(juce::TextEditor& editor) override;
    void textEditorKeyPress(const juce::KeyPress& key) override;
    void textEditorFocusLost(juce::TextEditor&) override {}

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g,
                          int width, int height, bool isSelected) override;
    void selectedRowsChanged(int lastSelectedRow) override;
    void paintRowBackground(juce::Graphics& g, int width, int height,
                            bool isSelected, int row) override;

    /** Recursively collect supported files from the workspace root. */
    void collectFiles(const std::filesystem::path& root);

    /** Fuzzy-match a query against a file path string. */
    static bool fuzzyMatch(std::string_view query, std::string_view path);

    /** Refresh the filtered list after a query change. */
    void refreshFiltered();

    std::filesystem::path workspaceRoot_;
    std::vector<std::filesystem::path> allFiles_;
    std::vector<std::filesystem::path> filteredFiles_;
    int selectedIndex_ = 0;

    std::unique_ptr<juce::TextEditor> filterField_;
    std::unique_ptr<juce::ListBox> listBox_;
    std::unique_ptr<juce::Label> hintLabel_;

    std::unique_ptr<juce::TextEditor> getFocusOnShow_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(QuickOpenDialog)
};

} // namespace hathor::ui
