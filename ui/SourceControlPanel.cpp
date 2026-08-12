// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * SourceControlPanel.cpp — L-5: native JUCE Git source-control panel.
 *
 * Implements the Changes/Commit view (default) and the History view with
 * Git graph. All Git operations run asynchronously via GitRepository (which
 * uses worker threads) — the JUCE message thread never blocks on git.
 *
 * Requirement references: L-5 §Source Control Panel, L-5 §History,
 *   L-5 §Git Graph, L-5 §Diff View
 */

#include "SourceControlPanel.hpp"

#include <algorithm>
#include <sstream>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// GitChangeRow helpers
// ---------------------------------------------------------------------------

std::string GitChangeRow::statusText() const
{
    switch (status)
    {
        case GitFileStatus::Modified:  return staged == GitStaged::Yes ? "M" : "M";
        case GitFileStatus::Added:     return "A";
        case GitFileStatus::Deleted:   return "D";
        case GitFileStatus::Renamed:   return "R";
        case GitFileStatus::Copied:    return "C";
        case GitFileStatus::Untracked: return "??";
        case GitFileStatus::UntrackedDir: return "!!";
        case GitFileStatus::Clean:     return " ";
    }
    return " ";
}

juce::Colour GitChangeRow::statusColor(const Palette& palette) const
{
    if (isConflicted)
        return palette.error;
    switch (status)
    {
        case GitFileStatus::Modified:  return palette.warning;
        case GitFileStatus::Added:
        case GitFileStatus::Untracked: return palette.accent;
        case GitFileStatus::Deleted:   return palette.error;
        case GitFileStatus::Renamed:
        case GitFileStatus::Copied:    return juce::Colours::yellow;
        default:                       return palette.textSecondary;
    }
}

// ---------------------------------------------------------------------------
// TabButton helper struct
// ---------------------------------------------------------------------------

struct TabButton
{
    GitPanelTab tab;
    juce::String label;
    juce::Rectangle<int> bounds;
    bool active = false;
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SourceControlPanel::SourceControlPanel(const std::string& projectDir)
{
    // Create the Git repository model.
    repository_ = std::make_unique<GitRepository>();
    repository_->setRepoPath(projectDir.empty()
        ? juce::File::getCurrentWorkingDirectory().getFullPathName().toStdString()
        : projectDir);

    createComponents();

    // Tab buttons
    tabButtons_[0] = { GitPanelTab::Changes, "Changes", {}, true };
    tabButtons_[1] = { GitPanelTab::History, "History", {}, false };

    // Start polling timer (30 Hz is too fast — use 15 Hz like ProblemsPanel).
    startTimerHz(kPollIntervalHz);
}

SourceControlPanel::~SourceControlPanel()
{
    stopTimer();
    repository_->cancel();
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

void SourceControlPanel::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
    if (!visible)
    {
        // Pause the timer when hidden to avoid unnecessary polling.
        if (isVisible())
            stopTimer();
    }
    else
    {
        if (!isVisible())
            startTimerHz(kPollIntervalHz);
        // Auto-refresh when shown.
        refresh();
    }
}

// ---------------------------------------------------------------------------
// Components
// ---------------------------------------------------------------------------

void SourceControlPanel::createComponents()
{
    const auto& palette = HathorLookAndFeel::defaultPalette();

    // Create all child components.
    changesList_ = std::make_unique<juce::ListBox>();
    changesList_->setModel(this);
    changesList_->setRowHeight(22);
    addAndMakeVisible(changesList_.get());

    stagedList_ = std::make_unique<juce::ListBox>();
    stagedList_->setModel(this);
    stagedList_->setRowHeight(22);
    addAndMakeVisible(stagedList_.get());

    commitMessage_ = std::make_unique<juce::TextEditor>();
    commitMessage_->setMultiLine(true, true);
    commitMessage_->setReadOnly(false);
    commitMessage_->setFont(HathorLookAndFeel::fontRegular(13.0f));
    commitMessage_->setColour(juce::TextEditor::backgroundColourId, palette.surfaceLow);
    commitMessage_->setColour(juce::TextEditor::textColourId, palette.textPrimary);
    commitMessage_->addListener(this);
    addAndMakeVisible(commitMessage_.get());

    stageBtn_ = std::make_unique<juce::TextButton>("Stage");
    stageBtn_->onClick = [this]() { stageSelected(); };
    addAndMakeVisible(stageBtn_.get());

    unstageBtn_ = std::make_unique<juce::TextButton>("Unstage");
    unstageBtn_->onClick = [this]() { unstageSelected(); };
    addAndMakeVisible(unstageBtn_.get());

    discardBtn_ = std::make_unique<juce::TextButton>("Discard");
    discardBtn_->onClick = [this]() { discardSelected(); };
    addAndMakeVisible(discardBtn_.get());

    commitBtn_ = std::make_unique<juce::TextButton>("Commit");
    commitBtn_->onClick = [this]() { commit(); };
    addAndMakeVisible(commitBtn_.get());

    pushBtn_ = std::make_unique<juce::TextButton>("Push");
    pushBtn_->onClick = [this]() { push(); };
    addAndMakeVisible(pushBtn_.get());

    pullBtn_ = std::make_unique<juce::TextButton>("Pull");
    pullBtn_->onClick = [this]() { pull(); };
    addAndMakeVisible(pullBtn_.get());

    fetchBtn_ = std::make_unique<juce::TextButton>("Fetch");
    fetchBtn_->onClick = [this]() { fetch(); };
    addAndMakeVisible(fetchBtn_.get());

    branchBtn_ = std::make_unique<juce::TextButton>("Branch…");
    branchBtn_->onClick = [this]() { createBranch(); };
    addAndMakeVisible(branchBtn_.get());

    branchCombo_ = std::make_unique<juce::ComboBox>();
    branchCombo_->setEditableText(true);
    addAndMakeVisible(branchCombo_.get());

    statusLabel_ = std::make_unique<juce::Label>();
    statusLabel_->setFont(HathorLookAndFeel::fontMedium(11.0f));
    addAndMakeVisible(statusLabel_.get());

    branchLabel_ = std::make_unique<juce::Label>();
    branchLabel_->setFont(HathorLookAndFeel::fontMedium(11.0f));
    addAndMakeVisible(branchLabel_.get());

    // History tab components
    gitGraph_ = std::make_unique<GitGraph>();
    gitGraph_->onCommitSelected = [this](const GitCommit& commit) {
        selectedCommit_ = commit;
        hasSelectedCommit_ = true;
        // Update commit detail view
        juce::String detail;
        detail << commit.shortSha << " · " << commit.authorName
               << " · " << commit.commitTime;
        commitDetail_->setText(detail, juce::dontSendNotification);
    };
    gitGraph_->onBranchClicked = [this](const std::string& branchName) {
        // Switch to the clicked branch.
        repository_->checkoutBranch(branchName, [](bool) {});
    };
    addChildComponent(gitGraph_.get());

    historyList_ = std::make_unique<juce::ListBox>();
    addAndMakeVisible(historyList_.get());

    commitDetail_ = std::make_unique<juce::TextEditor>();
    commitDetail_->setReadOnly(true);
    commitDetail_->setFont(HathorLookAndFeel::fontRegular(11.0f));
    addAndMakeVisible(commitDetail_.get());

    historyDiffView_ = std::make_unique<GitDiffView>();
    addChildComponent(historyDiffView_.get());
}

// ---------------------------------------------------------------------------
// Panel management
// -----------------------------------------------------------------------

void SourceControlPanel::setActiveTab(GitPanelTab tab)
{
    activeTab_ = tab;

    tabButtons_[0].active = (tab == GitPanelTab::Changes);
    tabButtons_[1].active = (tab == GitPanelTab::History);

    // Refresh history when switching to it.
    if (tab == GitPanelTab::History && history_.empty())
        refreshHistoryAsync();

    resized();
    repaint();
}

void SourceControlPanel::refresh()
{
    refreshStatusAsync();
    refreshHistoryAsync();
}

// ---------------------------------------------------------------------------
// Async refresh
// -----------------------------------------------------------------------

void SourceControlPanel::refreshStatusAsync()
{
    if (isBusy_ || !repository_->hasRepository())
        return;

    statusRefreshPending_ = true;
    isBusy_ = true;
    if (onBusyChanged)
        onBusyChanged(true);

    // GitRepository::refreshStatus runs on a detached worker thread.
    // The callback fires on that worker thread — it must NOT touch JUCE UI.
    // We just set the completion flag; the timer polls it on the message thread.
    repository_->refreshStatus([this]() {
        statusRefreshDone_ = true;
    });
}

void SourceControlPanel::onStatusRefreshComplete()
{
    // Called on the MESSAGE thread (via timer polling).
    statusRefreshPending_ = false;
    isBusy_ = false;
    if (onBusyChanged)
        onBusyChanged(false);

    buildChangeRows();
    updateStatusSummary();
    updateBranchList();

    // Force UI update
    changesList_->updateContent();
    stagedList_->updateContent();
    repaint();
}

void SourceControlPanel::refreshHistoryAsync()
{
    if (!repository_->hasRepository())
        return;

    historyRefreshPending_ = true;

    repository_->refreshHistory([this]() {
        historyRefreshDone_ = true;
    });
}

void SourceControlPanel::onHistoryRefreshComplete()
{
    // Called on the MESSAGE thread (via timer polling).
    historyRefreshPending_ = false;

    history_ = repository_->getHistory();

    // Update the graph.
    gitGraph_->setCommits(history_);
    gitGraph_->setCurrentBranch(repository_->getCurrentBranch());
    gitGraph_->setHeadSha(repository_->getHeadSha());

    // Update the history list.
    historyList_->updateContent();

    repaint();
}

// ---------------------------------------------------------------------------
// Row building
// ---------------------------------------------------------------------------

void SourceControlPanel::buildChangeRows()
{
    auto entries = repository_->getStatusEntries();
    changeRows_.clear();
    stagedRows_.clear();

    for (const auto& entry : entries)
    {
        GitChangeRow row;
        row.path = entry.path;
        row.displayName = entry.path;
        row.status = entry.status;
        row.staged = entry.staged;
        row.isConflicted = entry.isConflicted;

        if (entry.staged == GitStaged::Yes)
            stagedRows_.push_back(row);
        else
            changeRows_.push_back(row);
    }
}

void SourceControlPanel::updateStatusSummary()
{
    statusLabel_->setText(repository_->getStatusSummary(),
                          juce::dontSendNotification);
}

void SourceControlPanel::updateBranchList()
{
    branchLabel_->setText(repository_->getCurrentBranch(),
                          juce::dontSendNotification);

    auto refs = repository_->getRefs();
    branchCombo_->clear();

    int currentIdx = 0;
    for (size_t i = 0; i < refs.size(); ++i)
    {
        if (refs[i].kind == GitRef::Kind::LocalBranch)
        {
            branchCombo_->addItem(juce::String(refs[i].name),
                                   static_cast<int>(i) + 1);
            if (refs[i].isCurrent)
                currentIdx = static_cast<int>(i) + 1;
        }
    }

    branchCombo_->setSelectedId(currentIdx > 0 ? currentIdx : 1);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void SourceControlPanel::stageSelected()
{
    if (changeRows_.empty())
        return;

    // Stage all unstaged changes.
    repository_->stageAll([](bool) {});
}

void SourceControlPanel::unstageSelected()
{
    if (stagedRows_.empty())
        return;

    // Unstage all staged changes.
    for (const auto& row : stagedRows_)
    {
        repository_->unstageFile(row.path, [](bool) {});
    }
    refreshStatusAsync();
}

void SourceControlPanel::discardSelected()
{
    if (changeRows_.empty())
        return;

    // Show confirmation dialog (async to support all JUCE configurations).
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withTitle("Discard Changes?")
            .withMessage("This will discard all unstaged changes. This cannot be undone. Continue?")
            .withButton("OK")
            .withButton("Cancel"),
        [this](int result)
        {
            if (result != 1)  // not OK
                return;
            // Discard each unstaged file.
            for (const auto& row : changeRows_)
                repository_->discardFile(row.path, [](bool) {});
            refreshStatusAsync();
        });
}

void SourceControlPanel::commit()
{
    juce::String message = commitMessage_->getText();
    if (message.isEmpty())
        return;

    repository_->commit(message.toStdString(),
        [this](bool success, const std::string& error)
        {
            juce::MessageManager::callAsync([this, success, error]()
            {
                if (success)
                {
                    commitMessage_->clear();
                    refresh();
                }
                else
                {
                    juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                        "Commit failed", juce::String(error), "OK", nullptr);
                }
            });
        });
}

void SourceControlPanel::push()
{
    repository_->push([this](bool success, const std::string& output)
    {
        juce::MessageManager::callAsync([this, success, output]()
        {
            if (!success)
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                    "Push failed", juce::String(output), "OK", nullptr);
            else
                refresh();
        });
    });
}

void SourceControlPanel::pull()
{
    repository_->pull([this](bool success, const std::string& output)
    {
        juce::MessageManager::callAsync([this, success, output]()
        {
            if (success)
                refresh();
            else
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                    "Pull failed", juce::String(output), "OK", nullptr);
        });
    });
}

void SourceControlPanel::fetch()
{
    repository_->fetch([this](bool success, const std::string& output)
    {
        juce::MessageManager::callAsync([this, success, output]()
        {
            if (success)
                refresh();
            else
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                    "Fetch failed", juce::String(output), "OK", nullptr);
        });
    });
}

void SourceControlPanel::createBranch()
{
    // Prompt for branch name using an AlertWindow with a text editor.
    // We create an AlertWindow, add a text editor to it, and show it
    // asynchronously. The callback is invoked when the user dismisses it.
    auto* window = new juce::AlertWindow("Create Branch",
                                         "Enter branch name:",
                                         juce::AlertWindow::QuestionIcon);
    window->addTextEditor("branchName", juce::String());
    window->addButton("OK", 1);
    window->addButton("Cancel", 0);

    // Use showAsync with a callback that reads the text editor.
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withTitle("Create Branch")
            .withMessage("Enter branch name:")
            .withButton("OK")
            .withButton("Cancel"),
        [window](int result)
        {
            if (result == 1 && window != nullptr)
            {
                if (auto* editor = window->getTextEditor("branchName"))
                {
                    juce::String branchName = editor->getText();
                    // Branch creation will be handled by a follow-up
                    // call — for now, we just close the window.
                    // In a fuller implementation, we'd call repository_.
                }
            }
            delete window;
        });
}

void SourceControlPanel::switchBranch()
{
    // Switch to the selected branch in the combo box.
    juce::String branchName = branchCombo_->getText();
    if (!branchName.isEmpty())
    {
        repository_->checkoutBranch(branchName.toStdString(),
            [this](bool success)
            {
                juce::MessageManager::callAsync([this, success]()
                {
                    if (success)
                        refresh();
                    else
                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                            "Checkout failed", "Could not switch branch.", "OK", nullptr);
                });
            });
    }
}

void SourceControlPanel::mergeBranch()
{
    juce::String branchName = branchCombo_->getText();
    if (!branchName.isEmpty())
    {
        repository_->merge(branchName.toStdString(),
            [this](bool success, const std::string& output, bool hasConflicts)
            {
                juce::MessageManager::callAsync([this, success, output, hasConflicts]()
                {
                    if (hasConflicts)
                    {
                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                            "Merge conflicts", "Merge completed with conflicts. Resolve in the editor.", "OK", nullptr);
                        refresh();
                    }
                    else if (success)
                        refresh();
                    else
                        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon,
                            "Merge failed", juce::String(output), "OK", nullptr);
                });
            });
    }
}

// ---------------------------------------------------------------------------
// Timer — polls for async completion
// ---------------------------------------------------------------------------

void SourceControlPanel::timerCallback()
{
    // The GitRepository's async refresh methods run on detached worker threads.
    // They set completion flags (statusRefreshDone_, historyRefreshDone_)
    // which we poll here on the JUCE message thread to safely update the UI.

    if (statusRefreshDone_.exchange(false))
    {
        onStatusRefreshComplete();
        repaint();
    }

    if (historyRefreshDone_.exchange(false))
    {
        onHistoryRefreshComplete();
        repaint();
    }

    // Periodic auto-refresh: every ~2 seconds while the panel is visible.
    if (isVisible() && !isBusy_ && repository_->hasRepository())
    {
        static int tickCount = 0;
        if (++tickCount > kPollIntervalHz * 2)
        {
            tickCount = 0;
            refreshStatusAsync();
        }
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void SourceControlPanel::resized()
{
    auto b = getLocalBounds();

    // Tab bar
    auto tabArea = b.removeFromTop(kTabHeight);

    int tabW = tabArea.getWidth() / 2;
    tabButtons_[0].bounds = tabArea.removeFromLeft(tabW);
    tabButtons_[1].bounds = tabArea;

    if (activeTab_ == GitPanelTab::Changes)
    {
        // --- Changes tab layout ---
        // Top row: branch label + status label
        auto topRow = b.removeFromTop(kStatusHeight);
        branchLabel_->setBounds(topRow.removeFromLeft(120));
        statusLabel_->setBounds(topRow);

        // Changes list
        auto changesArea = b.removeFromTop(b.getHeight() / 2 - kCommitBoxHeight / 2);
        changesList_->setBounds(changesArea);

        // Staged list
        auto stagedArea = b.removeFromTop(b.getHeight() / 2 - kCommitBoxHeight / 2);
        stagedList_->setBounds(stagedArea);

        // Commit message + buttons
        auto commitArea = b.removeFromBottom(kCommitBoxHeight + kButtonHeight + kMargin);
        commitMessage_->setBounds(commitArea.removeFromTop(kCommitBoxHeight));

        auto buttonRow = commitArea.removeFromTop(kButtonHeight);
        int btnW = 80;
        stageBtn_->setBounds(buttonRow.removeFromLeft(btnW));
        buttonRow.removeFromLeft(8);
        unstageBtn_->setBounds(buttonRow.removeFromLeft(btnW));
        buttonRow.removeFromLeft(8);
        discardBtn_->setBounds(buttonRow.removeFromLeft(btnW));
        buttonRow.removeFromRight(8);
        pushBtn_->setBounds(buttonRow.removeFromRight(btnW));
        buttonRow.removeFromRight(8);
        pullBtn_->setBounds(buttonRow.removeFromRight(btnW));
        buttonRow.removeFromRight(8);
        fetchBtn_->setBounds(buttonRow.removeFromRight(btnW));
        buttonRow.removeFromRight(8);
        branchBtn_->setBounds(buttonRow.removeFromRight(btnW));
        buttonRow.removeFromRight(8);
        commitBtn_->setBounds(buttonRow.removeFromRight(btnW));
    }
    else
    {
        // --- History tab layout ---
        // Git graph on top half, commit details on bottom.
        auto graphArea = b.removeFromTop(b.getHeight() / 2);
        gitGraph_->setBounds(graphArea);
        commitDetail_->setBounds(b);
    }
}

void SourceControlPanel::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Background
    g.fillAll(palette.surfaceLow);

    // Tab bar background
    g.setColour(palette.surfaceHigh);
    g.fillRect(0, 0, getWidth(), kTabHeight);

    // Tab bar separator
    g.setColour(palette.surfaceHighest);
    g.fillRect(0, kTabHeight - 1, getWidth(), 1);

    // Paint tab buttons
    for (auto& tb : tabButtons_)
    {
        const bool isActive = tb.active;
        const juce::Colour bgCol = isActive
            ? palette.surfaceContainer
            : palette.surfaceLow;

        g.setColour(bgCol);
        g.fillRect(tb.bounds);

        g.setColour(isActive ? palette.textPrimary : palette.textSecondary);
        g.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::labelMd));
        g.drawText(tb.label, tb.bounds.reduced(8, 4),
                   juce::Justification::centred, false);

        // Active tab underline
        if (isActive)
        {
            g.setColour(palette.accent);
            g.fillRect(tb.bounds.getX(), tb.bounds.getBottom() - 2,
                       tb.bounds.getWidth(), 2);
        }
    }

    // Paint the active tab content background.
    auto contentBounds = getLocalBounds().withTrimmedTop(kTabHeight + kStatusHeight);
    g.setColour(palette.surface);
    g.fillRect(contentBounds);
}

// ---------------------------------------------------------------------------
// Mouse input
// ---------------------------------------------------------------------------

bool SourceControlPanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (onClosePanel)
            onClosePanel();
        return true;
    }
    if (key == juce::KeyPress::tabKey && key.getModifiers().isCtrlDown())
    {
        // Ctrl+Tab to switch tabs.
        setActiveTab(getActiveTab() == GitPanelTab::Changes
            ? GitPanelTab::History
            : GitPanelTab::Changes);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ListBoxModel (changes list)
// ---------------------------------------------------------------------------

int SourceControlPanel::getNumRows()
{
    if (activeTab_ == GitPanelTab::Changes)
        return static_cast<int>(changeRows_.size() + stagedRows_.size());
    return static_cast<int>(history_.size());
}

void SourceControlPanel::paintListBoxItem(int row, juce::Graphics& g,
                                          int width, int height, bool isSelected)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    if (activeTab_ == GitPanelTab::Changes)
    {
        const std::vector<GitChangeRow>& rows =
            (row < static_cast<int>(stagedRows_.size()))
                ? stagedRows_ : changeRows_;
        int idx = (row < static_cast<int>(stagedRows_.size()))
            ? row : row - static_cast<int>(stagedRows_.size());
        if (idx < 0 || idx >= static_cast<int>(rows.size()))
            return;
        const auto& item = rows[idx];

        // Background
        if (isSelected)
            g.setColour(palette.surfaceHigh);
        else
            g.setColour(palette.surfaceLow);
        g.fillRect(0, 0, width, height);

        // Status icon
        const juce::Colour statusCol = item.statusColor(palette);
        g.setColour(statusCol);
        g.setFont(HathorLookAndFeel::fontBold(12.0f));
        juce::String statusStr(item.statusText());
        g.drawText(statusStr, 4, 2, 24, height - 4,
                   juce::Justification::centred, false);

        // File path
        g.setColour(palette.textPrimary);
        g.setFont(HathorLookAndFeel::fontRegular(12.0f));
        g.drawText(juce::String(item.displayName),
                   32, 2, width - 36, height - 4,
                   juce::Justification::centredLeft, false);
    }
    else
    {
        // History list row
        if (row < 0 || row >= static_cast<int>(history_.size()))
            return;
        const auto& commit = history_[row];

        if (isSelected)
            g.setColour(palette.surfaceHigh);
        else
            g.setColour(palette.surfaceLow);
        g.fillRect(0, 0, width, height);

        g.setColour(palette.textPrimary);
        g.setFont(HathorLookAndFeel::fontRegular(12.0f));

        juce::String line = commit.shortSha;
        line << "  " << commit.subject;
        g.drawText(line, 32, 2, width - 36, height - 4,
                   juce::Justification::centredLeft, false);

        // Author + date
        juce::String authorDate = commit.authorName;
        if (!commit.commitTime.empty())
        {
            std::string t = commit.commitTime;
            if (t.size() > 10)
                authorDate << " · " << t.substr(0, 10).c_str();
        }
        g.setColour(palette.textSecondary);
        g.setFont(HathorLookAndFeel::fontRegular(10.0f));
        g.drawText(authorDate, 32, 2, width - 36, height - 4,
                   juce::Justification::bottomLeft, false);
    }
}

void SourceControlPanel::selectedRowsChanged(int lastSelectedRow)
{
    if (activeTab_ == GitPanelTab::History && lastSelectedRow >= 0
        && lastSelectedRow < static_cast<int>(history_.size()))
    {
        selectedCommit_ = history_[lastSelectedRow];
        hasSelectedCommit_ = true;

        // Update the graph selection.
        gitGraph_->selectedCommit(); // just triggers repaint in graph

        // Show commit detail.
        juce::String detail;
        detail << "Commit: " << selectedCommit_.sha << "\n"
               << "Author: " << selectedCommit_.authorName
               << " <" << selectedCommit_.authorEmail << ">\n"
               << "Date:   " << selectedCommit_.authorTime << "\n\n"
               << "    " << selectedCommit_.subject;
        commitDetail_->setText(detail, juce::dontSendNotification);
    }
}

} // namespace hathor::ui
