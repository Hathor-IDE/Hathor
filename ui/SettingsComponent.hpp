// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SettingsComponent.hpp — settings tab content (A2).
 *
 * A dedicated juce::Component shown inside the EditorArea's Tab_Bar alongside
 * .hathor/.ck file tabs (per PROGRAM.md decision #7). Reachable from the
 * Ribbon Settings/Profile button.
 *
 * Sections:
 *   1. Appearance — theme picker (5 themes from A1); opacity slider (B5).
 *   2. Agent / ACP — agent executable path; hathor-mcp path (read-only inferred).
 *   3. Petdex — browse/select a mascot (D1–D4); opt-in only, no default.
 *   4. ChucK placeholder — inert until B4 ships.
 *   5. EQ placeholder — inert until B7 ships.
 *
 * Apply/Reset/Close semantics (PROGRAM.md §A2):
 *   - Two-state model: _committed (source of truth from ApplicationProperties)
 *     and _pending (current UI control values).
 *   - Apply: commits _pending to _committed, applies immediately (theme/opacity),
 *     persists via ApplicationProperties. Clears pending-changes indicator.
 *   - Reset: reverts _pending to _committed (discard edits, NOT factory defaults).
 *   - Close-without-Apply: discards _pending (same as Reset); _committed persists.
 *
 * Requirements: A2, B3, B5
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>
#include <string>

#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

/**
 * SettingsComponent
 *
 * A scrollable settings panel with Appearance, Agent/ACP, Petdex, and
 * placeholder (ChucK / EQ) sections. Exposes Apply / Reset / Close buttons.
 *
 * The two-state model:
 *   - On construction, _committed values are loaded from ApplicationProperties.
 *   - _pending values are initialised from _committed.
 *   - UI controls edit _pending.
 *   - Apply() writes _pending → _committed + live state + persistent storage.
 *   - Reset() writes _committed → _pending (reverts the edit buffer).
 *   - Closing without Apply() discards _pending (== Reset, no Apply persists).
 */
class SettingsComponent : public juce::Component,
                           public juce::Slider::Listener,
                          public juce::Button::Listener,
                          public juce::ComboBox::Listener,
                          public juce::TextEditor::Listener
{
public:
    /**
     * Construct the settings panel.
     *
     * @param props    ApplicationProperties for persistence (same instance as MainWindow).
     *                 May be nullptr — in which case defaults are used and no persistence occurs.
     */
    explicit SettingsComponent(juce::ApplicationProperties* props);

    ~SettingsComponent() override;

    // Non-copyable / non-movable (owns JUCE child components).
    SettingsComponent(SettingsComponent&&)                 = delete;
    SettingsComponent& operator=(SettingsComponent&&)      = delete;

    // -----------------------------------------------------------------------
    // Public API consumed by EditorArea / MainWindow
    // -----------------------------------------------------------------------

    /**
     * Returns true if the pending (edit-buffer) values differ from committed values.
     * Used by TabBarComponent for the Unsaved_Dot-equivalent indicator (A2).
     */
    bool hasPendingChanges() const noexcept { return pendingChanges_; }

    /**
      * Discard pending edits (revert _pending to _committed, like Reset).
      * Called when the Settings tab is closed without Apply (A2).
      */
    void resetToCommitted();

    /**
      * Callback invoked after Apply commits the edit buffer to live state.
      * MainWindow can use this to restart the agent session with the new
      * agent executable path.
      */
    std::function<void()> onSettingsApplied;

    /**
     * Return the display label for the tab bar.
     */
    juce::String tabLabel() const noexcept { return "Settings"; }

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------

    void paint(juce::Graphics& g) override;
    void resized() override;

    // -----------------------------------------------------------------------
    // Slider::Listener
    // -----------------------------------------------------------------------

    void sliderValueChanged(juce::Slider* slider) override;

    // -----------------------------------------------------------------------
    // Button::Listener
    // -----------------------------------------------------------------------

    void buttonClicked(juce::Button* button) override;

    // -----------------------------------------------------------------------
    // ComboBox::Listener
    // -----------------------------------------------------------------------

    /** Return the human-readable name for a theme (for Settings list).
     */
    void comboBoxChanged(juce::ComboBox* comboBox) override;

    // -----------------------------------------------------------------------
    // TextEditor::Listener
    // -----------------------------------------------------------------------

    void textEditorTextChanged(juce::TextEditor& editor) override;

private:
    // -----------------------------------------------------------------------
    // Internal: settings data model
    // -----------------------------------------------------------------------

    struct SettingsModel
    {
        ThemeId   theme          = ThemeId::Dark;
        float     opacityPercent = 70.0f;   // B5: 70% default on mac/win, 100% on Linux in Apply
        std::string agentExePath;
        std::string petSelection;           // empty = no mascot selected
    };

    // -----------------------------------------------------------------------
    // Internal: committed (source of truth) vs pending (edit buffer)
    // -----------------------------------------------------------------------

    SettingsModel committed_;   ///< last Applied values (persisted)
    SettingsModel pending_;     ///< current UI control values

    // -----------------------------------------------------------------------
    // Child components
    // -----------------------------------------------------------------------

    // Appearance section
    juce::ComboBox   themeCombo_;
    juce::Slider     opacitySlider_;
    juce::Label      opacityValueLabel_;

    // Agent / ACP section
    juce::TextEditor agentPathEditor_;
    juce::Label      agentPathLabel_;
    juce::Label      mcpPathLabel_;

    // Petdex section
    juce::ComboBox   petCombo_;
    juce::Label      petLabel_;

    // Action buttons
    juce::TextButton applyButton_;
    juce::TextButton resetButton_;

    // Scroll container
    std::unique_ptr<juce::Viewport> scrollView_;
    std::unique_ptr<juce::Component> contentPanel_;

    // -----------------------------------------------------------------------
    // Non-owning
    // -----------------------------------------------------------------------

    juce::ApplicationProperties* appProperties_;

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    bool pendingChanges_ = false;

    bool opacitySupported_ = true;  ///< false on Linux (B5 platform matrix)

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /** Load committed settings from ApplicationProperties. */
    SettingsModel loadSettings() const;

    /** Persist settings to ApplicationProperties. */
    void saveSettings(const SettingsModel& model) const;

    /** Build the Appearance section UI. */
    void buildAppearanceSection();

    /** Build the Agent / ACP section UI. */
    void buildAgentSection(const std::string& hathorMcpPath);

    /** Build the Petdex section UI. */
    void buildPetdexSection();

    /** Build the ChucK placeholder section. */
    void buildChuckPlaceholder();

    /** Build the EQ placeholder section. */
    void buildEqPlaceholder();

    /** Build the Apply/Reset buttons. */
    void buildActionButtons();

    /** Refresh pendingChanges_ flag and update button states. */
    void updateDirtyFlag();

    /** Apply a theme change at runtime (A1 integration). */
    void applyTheme(ThemeId theme);

    /** Apply opacity at runtime (B5). */
    void applyOpacity(float percent);

    /** Populate the pet selection combo box with known mascots. */
    void populatePetList();

    /** Layout constants */
    static constexpr int kLabelWidth   = 120;
    static constexpr int kSectionGap   = 24;
    static constexpr int kControlHeight = 24;
    static constexpr int kButtonHeight  = 28;
    static constexpr int kButtonWidth   = 80;
    static constexpr int kButtonGap     = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsComponent)
};

} // namespace hathor::ui
