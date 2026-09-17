// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * WorkspaceSearchPanel.hpp — JUCE workspace search/replace panel.
 *
 * A dockable panel at the bottom of the editor (like FindReplacePanel)
 * that wraps WorkspaceSearchModel to search across all workspace files.
 * Shows results grouped by file; clicking a result navigates to that
 * file:line:col.
 *
 * Requirement references: L-2 §1
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WorkspaceSearchModel.hpp"
#include "FindReplaceModel.hpp"

namespace hathor::ui {

class WorkspaceSearchPanel : public juce::Component,
                             public juce::TextEditor::Listener,
                             public juce::ListBoxModel
{
public:
    static constexpr int kPanelHeight = 250;

    WorkspaceSearchPanel(std::filesystem::path workspaceRoot,
                         WorkspaceSearchModel* model);
    ~WorkspaceSearchPanel() override;

    // Non-copyable
    WorkspaceSearchPanel(WorkspaceSearchPanel&&) = delete;
    WorkspaceSearchPanel& operator=(WorkspaceSearchPanel&&) = delete;

    /** Start a new search with the given query and flags. */
    void startSearch(const juce::String& query, const WorkspaceSearchFlags& flags);

    /** Cancel an in-flight background search. */
    void cancelSearch();

    /** Re-root file searches after a runtime workspace switch. */
    void setWorkspaceRoot(const std::filesystem::path& root)
    {
        workspaceRoot_ = root;
    }

    /** Toggle panel visibility. */
    void setVisible(bool visible) override;

    // juce::Component
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Callbacks — installed by EditorArea
    std::function<void(const std::filesystem::path& filePath, int line, int column)> onNavigateToMatch;
    std::function<void()> onClosePanel;
    /// Fired after Replace All writes files so the owner can reload clean
    /// open tabs that changed on disk.
    std::function<void(const std::vector<std::filesystem::path>&)> onFilesChanged;

private:
    // TextEditor::Listener
    void textEditorTextChanged(juce::TextEditor& editor) override;
    void textEditorEscapeKeyPressed(juce::TextEditor& editor) override;
    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;
    void textEditorFocusLost(juce::TextEditor&) override {}  // no-op — search runs on text change, not focus

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool isSelected) override;
    void selectedRowsChanged(int lastSelectedRow) override;

    /** Flatten results into a list of displayable items. */
    friend class WorkspaceSearchDoubleClickHandler;
    struct DisplayItem {
        std::filesystem::path filePath;
        std::string relativePath;
        const WorkspaceSearchMatch* match;
        const WorkspaceFileResult* fileResult;
    };

    std::vector<DisplayItem> displayItems_;
    int selectedIndex_ = 0;

    std::filesystem::path workspaceRoot_;
    WorkspaceSearchModel* model_;

    std::unique_ptr<juce::TextEditor> searchField_;
    std::unique_ptr<juce::TextEditor> replaceField_;
    std::unique_ptr<juce::TextButton> searchBtn_;
    std::unique_ptr<juce::TextButton> replaceAllBtn_;
    std::unique_ptr<juce::TextButton> closeBtn_;
    std::unique_ptr<juce::ToggleButton> regexCheckbox_;
    std::unique_ptr<juce::ToggleButton> caseSensitiveCheckbox_;
    std::unique_ptr<juce::ToggleButton> wholeWordCheckbox_;
    std::unique_ptr<juce::Label> hintLabel_;
    std::unique_ptr<juce::ListBox> listBox_;

    // Background search state: generation discards stale runs, cancel flag
    // stops the worker, running_ drives the Search/Stop button label.
    // searchMutex_ serializes the worker against result publication so a
    // newer run can't mutate results while an older publish reads them.
    std::atomic<uint64_t> searchGeneration_{ 0 };
    std::shared_ptr<std::atomic<bool>> searchCancel_;
    std::atomic<bool> searchRunning_{ false };
    std::mutex searchMutex_;

    void updateModelFlags();
    WorkspaceSearchFlags currentFlags() const;
    void setHint(const juce::String& text);
    void publishResults(int total);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WorkspaceSearchPanel)
};

} // namespace hathor::ui
