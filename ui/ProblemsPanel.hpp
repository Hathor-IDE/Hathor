// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ProblemsPanel.hpp — L-3: dedicated IDE-level Problems / Diagnostics view.
 *
 * A dockable panel (bottom-docked within EditorArea, like WorkspaceSearchPanel)
 * that aggregates all deterministic diagnostics published to the
 * DiagnosticRegistry. Provides:
 *   - Error / warning / info counts in a header row
 *   - Grouping by file (collapsible sections)
 *   - Filtering by severity (Error / Warning / Info)
 *   - Filtering by diagnostic source (Strudel LSP, ChucK Compiler, etc.)
 *   - Clickable source locations — fires onDiagnosticSelected
 *   - Refresh / revalidation button
 *   - Useful empty states
 *   - Explicit per-diagnostic source label
 *
 * Does NOT introduce AI repair — diagnostics remain deterministic and
 * authoritative. AI remains available only through the existing Phase H-K
 * contextual action architecture.
 *
 * Requirement references: L-3 §1, L-3 §2
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "HathorLookAndFeel.hpp"
#include "control/DiagnosticRegistry.hpp"

namespace hathor::ui {

/**
 * A single display-row in the Problems list. Either a file-header row or
 * an individual diagnostic row.
 */
struct ProblemRow
{
    enum class Type { FileHeader, Diagnostic };

    Type        type        = Type::Diagnostic;
    std::string uri;         ///< file:// URI (for both row types)
    std::string displayPath; ///< short, human-readable path
    bool        expanded   = true;  ///< file group expanded (FileHeader only)
    int         childCount = 0;      ///< diagnostics under this file (FileHeader)

    // --- Diagnostic-specific fields (Type::Diagnostic) ---
    uint64_t    diagId     = 0;  ///< stable identity from the registry
    hathor::control::DiagSeverity severity = hathor::control::DiagSeverity::Error;
    hathor::control::DiagSource  source  = hathor::control::DiagSource::Runtime;
    std::string sourceLabel;     ///< human-readable source name
    std::string code;            ///< source-specific code
    std::string message;         ///< diagnostic message text
    int         line   = 0;      ///< 1-based (0 = no location)
    int         column = 0;      ///< 1-based (0 = no location)
    std::string relatedInfo;     ///< optional extra context
};

class ProblemsPanel : public juce::Component,
                      private juce::ListBoxModel
{
public:
    static constexpr int kPanelHeight = 220;

    /**
     * @param registry  Non-owning pointer to the shared DiagnosticRegistry.
     *                  May be null — the panel still renders (empty state).
     */
    explicit ProblemsPanel(hathor::control::DiagnosticRegistry* registry = nullptr);
    ~ProblemsPanel() override;

    // Non-copyable
    ProblemsPanel(const ProblemsPanel&) = delete;
    ProblemsPanel& operator=(const ProblemsPanel&) = delete;

    // -----------------------------------------------------------------------
    // Visibility
    // -----------------------------------------------------------------------
    void setVisible(bool visible) override;

    // -----------------------------------------------------------------------
    // Diagnostic registry binding
    // -----------------------------------------------------------------------
    void setRegistry(hathor::control::DiagnosticRegistry* registry) noexcept;

    /** Trigger a refresh from the registry (called externally on change). */
    void refresh();

    // -----------------------------------------------------------------------
    // Filtering
    // -----------------------------------------------------------------------
    void setShowErrors(bool v) noexcept;
    void setShowWarnings(bool v) noexcept;
    void setShowInfo(bool v) noexcept;

    /** Set the source filter; empty string = show all sources. */
    void setSourceFilter(const std::string& sourceLabel);

    // -----------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // -----------------------------------------------------------------------
    // Callbacks — installed by EditorArea
    // -----------------------------------------------------------------------

    /**
     * Fired when the user clicks a diagnostic row.
     * The EditorArea translates this into a file-open + cursor position
     * via the existing L-2 editor tab system.
     */
    std::function<void(const std::string& uri, int line, int column)> onDiagnosticSelected;

    /** Fired when the user clicks the Refresh button. */
    std::function<void()> onRefresh;

    /** Fired when the panel is closed. */
    std::function<void()> onClosePanel;

private:
    // -----------------------------------------------------------------------
    // Internal helpers (called by listeners defined in the .cpp)
    // -----------------------------------------------------------------------
    void onSourceFilterChanged();
    void onDoubleClick();

    // =======================================================================
    // ListBoxModel
    // =======================================================================
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool isSelected) override;
    void selectedRowsChanged(int lastSelectedRow) override;
    void deleteKeyPressed(int lastRowSelected) override;
    juce::Component* refreshComponentForRow(int row, bool rowIsNowSelected,
                                             juce::Component* existingComponentToUpdate) override;

    // =======================================================================
    // Internal helpers
    // =======================================================================

    /** Rebuild the display rows from the registry's current diagnostics,
        applying active filters. Called on refresh() and on filter changes. */
    void rebuildRows();

    /** Flatten diagnostics into display rows, grouping by file. */
    void flattenIntoRows(const std::vector<hathor::control::Diagnostic>& diags);

    /** Toggle expansion of a file group at the given row index. */
    void toggleGroup(int rowIndex);

    /** Navigate to the diagnostic at the given row (if it's a diagnostic row). */
    void navigateToRow(int rowIndex);

    /** Build the header/summary string for the status area. */
    juce::String buildSummaryText() const;

    /** Paint a single diagnostic row. */
    void paintDiagnosticRow(juce::Graphics& g, const ProblemRow& row,
                            int width, int height, bool isSelected) const;

    /** Paint a file-header row. */
    void paintFileHeader(juce::Graphics& g, const ProblemRow& row,
                         int width, int height, bool isSelected, bool isExpanded) const;

    // =======================================================================
    // Child components
    // =======================================================================
    std::unique_ptr<juce::ListBox>      listBox_;
    std::unique_ptr<juce::Label>        hintLabel_;
    std::unique_ptr<juce::TextButton>   refreshBtn_;
    std::unique_ptr<juce::TextButton>   closeBtn_;
    std::unique_ptr<juce::TextButton>   errorsBtn_;
    std::unique_ptr<juce::TextButton>   warningsBtn_;
    std::unique_ptr<juce::TextButton>   infoBtn_;

    // Source filter dropdown
    std::unique_ptr<juce::ComboBox>     sourceFilter_;

    // =======================================================================
    // State
    // =======================================================================
    hathor::control::DiagnosticRegistry* registry_ = nullptr;

    std::vector<ProblemRow>             rows_;
    int                                 selectedRow_ = -1;

    bool showErrors_    = true;
    bool showWarnings_  = true;
    bool showInfo_      = false;
    std::string         sourceFilter_;  ///< empty = all sources

    // Row height constants
    static constexpr int kRowHeight       = 22;
    static constexpr int kHeaderHeight    = 28;
    static constexpr int kFieldHeight     = 24;
    static constexpr int kPanelHeight     = 220;
    static constexpr int kMargin          = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProblemsPanel)
};

} // namespace hathor::ui
