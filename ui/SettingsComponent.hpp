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
 *   1. Appearance — theme picker (5 themes from A1); opacity slider + blur/acrylic (B5);
 *      Audio subsection — Master EQ preset selector (B7-K3, drives B7-K2).
 *   2. Agent / ACP — agent executable path; hathor-mcp path (read-only inferred).
 *   3. Petdex — browse/select a mascot (D1–D4); opt-in only, no default.
 *   4. ChucK placeholder — inert until B4 ships.
 *   5. EQ — implemented in Appearance > Audio (B7-K3).
 *
 * Apply/Reset/Close semantics (PROGRAM.md §A2):
 *   - Two-state model: _committed (source of truth from ApplicationProperties)
 *     and _pending (current UI control values).
 *   - Apply: commits _pending to _committed, applies immediately (theme/opacity/blur),
 *     persists via ApplicationProperties. Clears pending-changes indicator.
 *   - Reset: reverts _pending to _committed (discard edits, NOT factory defaults).
 *   - Close-without-Apply: discards _pending (same as Reset); _committed persists.
 *
 * Requirements: A2, B3, B5
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>
#include <string>

#include "HathorLookAndFeel.hpp"
#include "WindowAppearanceController.hpp"
#include "MasterEq.hpp"

#include "PetdexTypes.hpp"

class AudioEngineFacade;

namespace hathor::ui {

class PetdexManifestService;

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
      * @param audio    AudioEngineFacade for applying B7-K2 EQ preset changes live (B7-K3).
      *                 May be nullptr — in which case the EQ preset selector is inert
      *                 (loads/persists only, does not call setMasterEqPreset).
      * @param petdex   App-lifetime PetdexManifestService (Phase G / D1). May be nullptr
      *                 — in which case the Petdex section shows an unavailable state.
      *                 The service is not contacted until this component is built,
      *                 i.e. only when the user actually opens the Settings tab (opt-in).
      */
    explicit SettingsComponent(juce::ApplicationProperties* props,
                               AudioEngineFacade* audio = nullptr,
                               PetdexManifestService* petdex = nullptr);

    ~SettingsComponent() override;

    SettingsComponent(SettingsComponent&&)                 = delete;
    SettingsComponent& operator=(SettingsComponent&&)      = delete;

    /**
     * Returns true if the pending (edit-buffer) values differ from committed values.
     * Used by TabBarComponent for the Unsaved_Dot-equivalent indicator (A2).
     */
    bool hasPendingChanges() const noexcept { return pendingChanges_; }

    /**
       Discard pending edits (revert _pending to _committed, like Reset).
       Called when the Settings tab is closed without Apply (A2).
     */
    void resetToCommitted();

    /**
       Callback invoked after Apply commits the edit buffer to live state.
       MainWindow can use this to restart the agent session with the new
       agent executable path.
     */
    std::function<void()> onSettingsApplied;

    /**
       Callback invoked after Apply commits the EQ preset to live state.
       MainWindow can use this to react to preset changes if needed (B7-K3).
       The EQ preset is already applied to the AudioEngine before this fires.
     */
    std::function<void(hathor::EqPreset)> onEqPresetApplied;

    /**
       Return the display label for the tab bar.
    */
    juce::String tabLabel() const noexcept { return "Settings"; }

    /** Set the WindowAppearanceController used for live preview of opacity/blur. */
    void setAppearanceController(WindowAppearanceController* controller) noexcept
    {
        appearanceController_ = controller;
    }

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
        float     opacityPercent = 70.0f;   // B5: 70% default on mac/win, 100% on Linux
        int       macosBlurRadius = 30;     // macOS only: blur radius 0–100
        bool      windowsAcrylic  = false;  // Windows only: Acrylic on/off
        std::string agentExePath;
        std::string petSelection;
        hathor::EqPreset eqPreset  = hathor::EqPreset::Flat;  // B7-K3
    };

    // -----------------------------------------------------------------------
    // Internal: committed (source of truth) vs pending (edit buffer)
    // -----------------------------------------------------------------------

    SettingsModel committed_;   ///< last Applied values (persisted)
    SettingsModel pending_;     ///< current UI control values

    // -----------------------------------------------------------------------
    // Child components
    // -----------------------------------------------------------------------

    // Appearance — Window subsection (B5)
    juce::Slider     opacitySlider_;
    juce::Label      opacityValueLabel_;
    juce::Label*     opacityWarningLabel_ = nullptr;  // Linux: "unavailable" message

    // macOS blur slider (under opacity, disabled when opacity == 100%)
    juce::Slider     blurSlider_;
    juce::Label      blurValueLabel_;

    // Windows Acrylic toggle (replaces blur slider on Windows)
    juce::TextButton acrylicButton_;
    juce::Label      acrylicLabel_;

    // Appearance — Theme subsection
    juce::ComboBox   themeCombo_;

    // Agent / ACP section
    juce::TextEditor agentPathEditor_;
    juce::Label*     agentPathLabel_ = nullptr;
    juce::Label      mcpPathLabel_;

    // Petdex section (Phase G / D1)
    juce::ComboBox   petCombo_;
    juce::Label      petLabel_;
    juce::TextEditor petSearchEditor_;
    juce::Label      petStatusLabel_;
    juce::Label      petAttributionLabel_;
    juce::TextButton petRefreshButton_;

    // Appearance — Audio subsection (B7-K3)
    juce::ComboBox   eqPresetCombo_;
    juce::Label      eqPresetLabel_;

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
    WindowAppearanceController* appearanceController_ = nullptr;
    AudioEngineFacade*           audioEngine_ = nullptr;  // B7-K3: for setMasterEqPreset
    PetdexManifestService*       petdexService_ = nullptr; // Phase G / D1 (app-lifetime, not owned)

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------

    bool pendingChanges_ = false;
    bool opacitySupported_ = true;   ///< false on Linux without compositor (B5)
    bool blurSupported_    = true;   ///< false on Linux without blur protocol

    // Phase G / D1: Petdex catalog state (selection itself lives in
    // SettingsModel::petSelection — a slug — and persists via the A2 model).
    PetdexManifest       manifest_;            ///< last manifest delivered by the service
    PetdexManifestStatus manifestStatus_ = PetdexManifestStatus::Idle;
    std::string          petStatusMessage_;
    std::vector<std::string> petComboSlugs_;   ///< slug per combo item (parallel, after "(none)")

    // -----------------------------------------------------------------------
    // Internal helpers
    // -----------------------------------------------------------------------

    /** Load committed settings from ApplicationProperties. */
    SettingsModel loadSettings() const;

    /** Persist settings to ApplicationProperties. */
    void saveSettings(const SettingsModel& model) const;

    /** Build the Appearance section UI (theme + window subsection). */
    void buildAppearanceSection();

    /** Build the Window subsection within Appearance (opacity + blur/acrylic). */
    void buildWindowSubsection(int& y);

    /** Build the Agent / ACP section UI. */
    void buildAgentSection(const std::string& hathorMcpPath);

    /** Build the Petdex section UI. */
    void buildPetdexSection();

    /** Build the ChucK placeholder section. */
    void buildChuckPlaceholder();

    /** Build the Appearance > Audio subsection (EQ preset selector, B7-K3). */
    void buildAudioSection(int& y);

    /** Build the Apply/Reset buttons. */
    void buildActionButtons();

    /** Rebuild the pet combo from manifest_ + the search filter. */
    void rebuildPetList();

    /** Select the combo entry matching @p slug (or "(none)"). */
    void selectPetInCombo(const std::string& slug);

    /** Handle a manifest result delivered by PetdexManifestService (message thread). */
    void onPetdexManifest(const PetdexManifestResult& result);

    /** Update the catalog status label from manifestStatus_/petStatusMessage_. */
    void updatePetdexStatusLabel();

    /** Update the D4 attribution/licensing label for the selected pet. */
    void updatePetAttribution();

    /** Find a pet in manifest_ by slug, or nullptr. */
    const PetdexPet* findPet(const std::string& slug) const noexcept;

    /** Refresh pendingChanges_ flag and update button states. */
    void updateDirtyFlag();

    /** Apply a theme change at runtime (A1 integration). */
    void applyTheme(ThemeId theme);

    /** Apply opacity + blur/acylic to the live window (delegates to controller or direct setAlpha). */
    void applyWindowAppearance(const SettingsModel& model);

    /** Apply the EQ preset to the live AudioEngine via B7-K2's atomic swap (B7-K3). */
    void applyEqPreset(hathor::EqPreset preset);

    /** Update the enabled/disabled state of blur controls based on opacity. */
    void updateBlurControlState();

    /** Layout constants */
    static constexpr int kLabelWidth   = 120;
    static constexpr int kControlHeight = 24;
    static constexpr int kButtonHeight  = 28;
    static constexpr int kButtonWidth   = 80;
    static constexpr int kButtonGap     = 8;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SettingsComponent)
};

} // namespace hathor::ui
