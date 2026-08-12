// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SourceControlPanel.hpp — L-5: native JUCE Git source-control panel.
 *
 * This is the main Git UI panel. It has two tabs:
 *
 *   1. Changes (default) — the working view:
 *      - Changes list (unstaged, with status icons)
 *      - Staged Changes list
 *      - Stage / Unstage buttons
 *      - Discard button (with confirmation)
 *      - Commit message editor
 *      - Commit button
 *      - Push / Pull / Fetch buttons
 *      - Branch create / switch / merge
 *
 *   2. History — commit history with a visual Git graph:
 *      - GitGraph visualization
 *      - Commit list (newest first)
 *      - Commit details on selection
 *      - Open diff for a selected commit
 *
 * The panel is bottom-docked within EditorArea (like ProblemsPanel and
 * TerminalPanel). It can also be opened as an editor tab (GitGraph).
 *
 * Threading boundary:
 *   - GitRepository runs all git commands on worker threads.
 *   - This component polls GitRepository status and retrieves cached results
 *     on a timer (30 Hz), so the JUCE message thread never blocks on Git.
 *   - The audio thread is never touched by this component.
 *
 * Requirement references: L-5 §Source Control Panel, L-5 §History,
 *   L-5 §Git Graph, L-5 §Diff View
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "GitRepository.hpp"
#include "GitGraph.hpp"
#include "GitDiffView.hpp"
#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

/// Visual state for the source control panel.
enum class GitPanelTab
{
    Changes,   ///< Changes/Commit view (default)
    History,   ///< History view with Git graph
};

/// A single row in the changes/staged list.
struct GitChangeRow
{
    std::string path;
    std::string displayName;
    GitFileStatus status;
    GitStaged staged;
    bool isConflicted = false;
    std::string statusText() const;
    juce::Colour statusColor(const Palette& palette) const;
};

/**
 * SourceControlPanel
 *
 * The main Git UI panel with Changes/Commit (default) and History tabs.
 * Provides staging, committing, branching, merging, push/pull/fetch,
 * history browsing, visual graph, and diff viewing — all as native JUCE
 * components with async Git operations.
 */
class SourceControlPanel : public juce::Component,
                          private juce::Timer,
                          private juce::TextEditor::Listener,
                          private juce::ListBoxModel
{
public:
    static constexpr int kPanelHeight = 320;

    explicit SourceControlPanel(const std::string& projectDir = {});
    ~SourceControlPanel() override;

    // -----------------------------------------------------------------------
    // Visibility
    // -----------------------------------------------------------------------

    void setVisible(bool visible) override;

    // -----------------------------------------------------------------------
    // Panel management
    // -----------------------------------------------------------------------

    /** Switch between Changes and History tabs. */
    void setActiveTab(GitPanelTab tab);

    /** Get the currently active tab. */
    GitPanelTab getActiveTab() const noexcept { return activeTab_; }

    /** Get the GitRepository model (non-owning). */
    GitRepository* repository() noexcept { return repository_.get(); }

    /** Refresh all Git state (status + history). */
    void refresh();

    // -----------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------

    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // -----------------------------------------------------------------------
    // juce::TextEditor::Listener (commit message editor)
    // -----------------------------------------------------------------------

    void textEditorTextChanged(juce::TextEditor&) override {}

    // -----------------------------------------------------------------------
    // juce::ListBoxModel (changes list)
    // -----------------------------------------------------------------------

    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool isSelected) override;
    void selectedRowsChanged(int lastSelectedRow) override;

    // -----------------------------------------------------------------------
    // Callbacks — installed by EditorArea / MainWindow
    // -----------------------------------------------------------------------

    /** Fired when the panel is closed. */
    std::function<void()> onClosePanel;

    /** Fired when a file in the changes list is double-clicked (open diff). */
    std::function<void(const std::string& path)> onFileDiffRequested;

    /** Fired when a commit in history is selected (open diff). */
    std::function<void(const std::string& sha)> onCommitDiffRequested;

    /** Fired when the Git Graph should open as an editor tab. */
    std::function<void()> onGraphTabRequested;

    /** Fired when the panel becomes busy (show spinner). */
    std::function<void(bool busy)> onBusyChanged;

    // -----------------------------------------------------------------------
    // Public actions (can be called programmatically)
    // -----------------------------------------------------------------------

    void stageSelected();
    void unstageSelected();
    void discardSelected();
    void commit();
    void push();
    void pull();
    void fetch();
    void createBranch();
    void switchBranch();
    void mergeBranch();

private:
    // -----------------------------------------------------------------------
    // Layout
    // -----------------------------------------------------------------------

    static constexpr int kHeaderHeight   = 36;
    static constexpr int kTabHeight      = 28;
    static constexpr int kCommitBoxHeight = 80;
    static constexpr int kButtonHeight   = 26;
    static constexpr int kMargin         = 8;
    static constexpr int kStatusHeight   = 24;

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    void createComponents();
    void updateStatusSummary();
    void updateBranchList();
    void refreshStatusAsync();
    void refreshHistoryAsync();
    void onStatusRefreshComplete();
    void onHistoryRefreshComplete();
    void buildChangeRows();
    void paintChangesTab(juce::Graphics& g);
    void paintHistoryTab(juce::Graphics& g);

    // -----------------------------------------------------------------------
    // Timer (polls for async completion)
    // -----------------------------------------------------------------------

    static constexpr int kPollIntervalHz = 15;
    void timerCallback() override;

    // -----------------------------------------------------------------------
    // Child components — Changes tab
    // -----------------------------------------------------------------------

    std::unique_ptr<juce::ListBox>        changesList_;
    std::unique_ptr<juce::ListBox>        stagedList_;
    std::unique_ptr<juce::TextEditor>     commitMessage_;
    std::unique_ptr<juce::TextButton>     stageBtn_;
    std::unique_ptr<juce::TextButton>     unstageBtn_;
    std::unique_ptr<juce::TextButton>     discardBtn_;
    std::unique_ptr<juce::TextButton>     commitBtn_;
    std::unique_ptr<juce::TextButton>     pushBtn_;
    std::unique_ptr<juce::TextButton>     pullBtn_;
    std::unique_ptr<juce::TextButton>     fetchBtn_;
    std::unique_ptr<juce::TextButton>     branchBtn_;
    std::unique_ptr<juce::ComboBox>       branchCombo_;
    std::unique_ptr<juce::Label>          statusLabel_;
    std::unique_ptr<juce::Label>          branchLabel_;

    // -----------------------------------------------------------------------
    // Child components — History tab
    // -----------------------------------------------------------------------

    std::unique_ptr<GitGraph>             gitGraph_;
    std::unique_ptr<juce::ListBox>        historyList_;
    std::unique_ptr<juce::TextEditor>     commitDetail_;
    std::unique_ptr<GitDiffView>          historyDiffView_;

    // -----------------------------------------------------------------------
    // Tab bar
    // -----------------------------------------------------------------------

    GitPanelTab activeTab_ = GitPanelTab::Changes;

    // -----------------------------------------------------------------------
    // Tab bar (local button model, no dependency on EnhancedTabBar)
    // -----------------------------------------------------------------------

    struct TabButton
    {
        GitPanelTab tab;
        juce::String label;
        juce::Rectangle<int> bounds;
        bool active = false;
    };
    std::array<TabButton, 2> tabButtons_;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    std::unique_ptr<GitRepository> repository_;
    std::vector<GitChangeRow> changeRows_;
    std::vector<GitChangeRow> stagedRows_;
    std::vector<GitCommit> history_;
    GitCommit selectedCommit_;
    bool hasSelectedCommit_ = false;
    bool isBusy_ = false;
    bool statusRefreshPending_ = false;
    bool historyRefreshPending_ = false;
    std::atomic<bool> statusRefreshDone_ = false;
    std::atomic<bool> historyRefreshDone_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SourceControlPanel)
};

} // namespace hathor::ui
