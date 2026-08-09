// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * SettingsComponent.cpp — implementation of the A2 settings tab.
 *
 * Requirements: A2, B3, B5
 */

#include "SettingsComponent.hpp"

#include <cstdlib>

#include "MasterEq.hpp"
#include "AudioEngineFacade.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SettingsComponent::SettingsComponent(juce::ApplicationProperties* props,
                                       AudioEngineFacade* audio)
    : appProperties_(props)
    , audioEngine_(audio)
{
    setInterceptsMouseClicks(true, true);

    // Determine platform opacity support (B5).
    // On Linux, transparency requires a running compositor — feature-detect.
    // On macOS/Windows, transparency is always supported.
#if JUCE_LINUX
    opacitySupported_ = false;  // will be updated by controller if available
    blurSupported_    = false;
#else
    opacitySupported_ = true;
    blurSupported_    = true;
#endif

    // Load committed (source-of-truth) settings from properties.
    committed_ = loadSettings();
    pending_   = committed_;

    // If we have an appearance controller, check actual capabilities.
    if (appearanceController_ != nullptr)
    {
        const auto caps = appearanceController_->detectCapabilities();
        opacitySupported_ = caps.transparencySupported;
        blurSupported_    = caps.blurSupported;
    }

    // Build the scrollable content panel.
    contentPanel_ = std::make_unique<juce::Component>();

    buildAppearanceSection();

    // Infer hathor-mcp path (same logic as HathorApplication.cpp).
    std::string hathorMcpPath;
#if JUCE_WINDOWS
    const std::string exeName = "hathor-mcp.exe";
#else
    const std::string exeName = "hathor-mcp";
#endif
    const juce::File mcpFile =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getSiblingFile(exeName);
    hathorMcpPath = mcpFile.getFullPathName().toStdString();

    buildAgentSection(hathorMcpPath);
    buildPetdexSection();
    buildChuckPlaceholder();
    buildActionButtons();

    // Wrap in a viewport.
    scrollView_ = std::make_unique<juce::Viewport>();
    scrollView_->setScrollBarsShown(true, false);
    scrollView_->addAndMakeVisible(*contentPanel_);

    addAndMakeVisible(*scrollView_);

    updateDirtyFlag();
}

SettingsComponent::~SettingsComponent() = default;

// ---------------------------------------------------------------------------
// B7-K3: EQ preset persistence helpers
// ---------------------------------------------------------------------------

namespace {

/// Stable string identifiers for the four v1 EQ presets (B7-K3 §1, §7).
/// These are stable keys independent of UI display text — the UI displays
/// presetName() but persistence stores the canonical lowercase identifier.
const char* eqPresetKey(hathor::EqPreset p) noexcept
{
    switch (p) {
        case hathor::EqPreset::Flat:      return "flat";
        case hathor::EqPreset::BassBoost: return "bass-boost";
        case hathor::EqPreset::Vocal:     return "vocal";
        case hathor::EqPreset::Bright:    return "bright";
    }
    return "flat";
}

/// Parse a preset identifier string back into EqPreset.
/// Unknown values fall back to Flat (the v1 default).
hathor::EqPreset parseEqPreset(const juce::String& s) noexcept
{
    const juce::String lc = s.toLowerCase();
    if (lc == "flat")                       return hathor::EqPreset::Flat;
    if (lc == "bass-boost" || lc == "bassboost") return hathor::EqPreset::BassBoost;
    if (lc == "vocal")                      return hathor::EqPreset::Vocal;
    if (lc == "bright")                     return hathor::EqPreset::Bright;
    return hathor::EqPreset::Flat;
}

} // anonymous namespace

SettingsComponent::SettingsModel
SettingsComponent::loadSettings() const
{
    SettingsModel m;

    if (appProperties_ == nullptr)
    {
        m.theme           = ThemeId::Dark;
        m.opacityPercent  = opacitySupported_ ? 70.0f : 100.0f;
        m.macosBlurRadius = 30;
        m.windowsAcrylic  = false;
        m.agentExePath    = "";
        m.petSelection    = "";
        m.eqPreset        = hathor::EqPreset::Flat;
        return m;
    }

    const auto* props = appProperties_->getUserSettings();
    if (props == nullptr)
    {
        m.theme           = ThemeId::Dark;
        m.opacityPercent  = opacitySupported_ ? 70.0f : 100.0f;
        m.macosBlurRadius = 30;
        m.windowsAcrylic  = false;
        m.agentExePath    = std::getenv("HATHOR_AGENT")
                              ? std::string(std::getenv("HATHOR_AGENT"))
                              : "";
        m.petSelection    = "";
        m.eqPreset        = hathor::EqPreset::Flat;
        return m;
    }

    const int themeIdx = props->getIntValue("settings.theme",
                                            static_cast<int>(ThemeId::Dark));

    static constexpr int kMinTheme = static_cast<int>(ThemeId::Dark);
    static constexpr int kMaxTheme = static_cast<int>(ThemeId::Light);
    const int clampedTheme = juce::jlimit(kMinTheme, kMaxTheme, themeIdx);
    m.theme = static_cast<ThemeId>(clampedTheme);

    // B5: opacity defaults — 70% on mac/win, 100% on Linux
    m.opacityPercent = props->getValue("settings.opacity",
                                       juce::String(opacitySupported_ ? 70.0f : 100.0f))
                          .getFloatValue();

    // B5: blur/acrylic persistence
    m.macosBlurRadius = props->getIntValue("settings.macosBlurRadius", 30);
    m.windowsAcrylic  = props->getBoolValue("settings.windowsAcrylic", false);

    m.agentExePath = props->getValue("settings.agentExePath").toStdString();
    m.petSelection = props->getValue("settings.petSelection").toStdString();

    // B7-K3: Load persisted EQ preset (stable key, not display text).
    m.eqPreset = parseEqPreset(props->getValue("settings.eqPreset", "flat"));

    return m;
}

void SettingsComponent::saveSettings(const SettingsModel& model) const
{
    if (appProperties_ == nullptr)
        return;

    auto* props = appProperties_->getUserSettings();
    if (props == nullptr)
        return;

    props->setValue("settings.theme",          static_cast<int>(model.theme));
    props->setValue("settings.opacity",        juce::String(model.opacityPercent));
    props->setValue("settings.macosBlurRadius", model.macosBlurRadius);
    props->setValue("settings.windowsAcrylic",  model.windowsAcrylic);
    props->setValue("settings.agentExePath",   juce::String(model.agentExePath));
    props->setValue("settings.petSelection",   juce::String(model.petSelection));
    props->setValue("settings.eqPreset",       juce::String(eqPresetKey(model.eqPreset)));

    props->saveIfNeeded();
}

// ---------------------------------------------------------------------------
// Section builders
// ---------------------------------------------------------------------------

void SettingsComponent::buildAppearanceSection()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const int labelW   = kLabelWidth;
    const int controlX = labelW + 8;
    const int controlW = 300;
    const int contentW = 600;

    int y = 0;

    // --- Section header ---
    auto* header = new juce::Label();
    header->setText("Appearance", juce::dontSendNotification);
    header->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    header->setColour(juce::Label::textColourId, palette.textPrimary);
    header->setJustificationType(juce::Justification::centredLeft);
    header->setBounds(0, y, contentW, kControlHeight);
    contentPanel_->addAndMakeVisible(header);

    y += kControlHeight + 8;

    // --- Theme picker ---
    auto* themeLabel = new juce::Label();
    themeLabel->setText("Theme:", juce::dontSendNotification);
    themeLabel->setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    themeLabel->setColour(juce::Label::textColourId, palette.textSecondary);
    themeLabel->setJustificationType(juce::Justification::centredRight);
    themeLabel->setBounds(0, y, labelW, kControlHeight);
    contentPanel_->addAndMakeVisible(themeLabel);

    themeCombo_.setBounds(controlX, y, controlW, kControlHeight);
    themeCombo_.setEditableText(false);

    themeCombo_.addItem("Dark",           static_cast<int>(ThemeId::Dark) + 1);
    themeCombo_.addItem("Purple / Neon",  static_cast<int>(ThemeId::PurpleNeon) + 1);
    themeCombo_.addItem("Capuchin",       static_cast<int>(ThemeId::Capuchin) + 1);
    themeCombo_.addItem("Sand",           static_cast<int>(ThemeId::Sand) + 1);
    themeCombo_.addItem("Light",          static_cast<int>(ThemeId::Light) + 1);

    themeCombo_.setSelectedId(static_cast<int>(pending_.theme) + 1,
                              juce::dontSendNotification);
    themeCombo_.addListener(this);
    contentPanel_->addAndMakeVisible(themeCombo_);

    y += kControlHeight + 12;

    // --- Window subsection header ---
    auto* windowHeader = new juce::Label();
    windowHeader->setText("Window", juce::dontSendNotification);
    windowHeader->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    windowHeader->setColour(juce::Label::textColourId, palette.textPrimary);
    windowHeader->setJustificationType(juce::Justification::centredLeft);
    windowHeader->setBounds(0, y, contentW, kControlHeight);
    contentPanel_->addAndMakeVisible(windowHeader);

    y += kControlHeight + 8;

    // Build the opacity + blur/acrylic controls
    buildWindowSubsection(y);

    // --- Audio subsection header (B7-K3) ---
    // Master EQ preset selector lives under Appearance > Audio.
    auto* audioHeader = new juce::Label();
    audioHeader->setText("Audio", juce::dontSendNotification);
    audioHeader->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    audioHeader->setColour(juce::Label::textColourId, palette.textPrimary);
    audioHeader->setJustificationType(juce::Justification::centredLeft);
    audioHeader->setBounds(0, y, contentW, kControlHeight);
    contentPanel_->addAndMakeVisible(audioHeader);

    y += kControlHeight + 8;

    // Build the EQ preset selector.
    buildAudioSection(y);

    contentPanel_->setBounds(0, 0, contentW, y + 16);
}

void SettingsComponent::buildWindowSubsection(int& y)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const int labelW   = kLabelWidth;
    const int controlX = labelW + 8;
    const int controlW = 300;

    // --- Opacity slider ---
    auto* opacityLabel = new juce::Label();
    opacityLabel->setText("Opacity:", juce::dontSendNotification);
    opacityLabel->setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    opacityLabel->setColour(juce::Label::textColourId, palette.textSecondary);
    opacityLabel->setJustificationType(juce::Justification::centredRight);
    opacityLabel->setBounds(0, y, labelW, kControlHeight);
    contentPanel_->addAndMakeVisible(opacityLabel);

    opacitySlider_.setBounds(controlX, y, controlW, kControlHeight);
    opacitySlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    // B5: range is 1–100 (was 20–100)
    opacitySlider_.setRange(1.0, 100.0, 1.0);
    opacitySlider_.setValue(pending_.opacityPercent, juce::dontSendNotification);
    opacitySlider_.addListener(this);
    contentPanel_->addAndMakeVisible(opacitySlider_);

    opacityValueLabel_.setBounds(controlX + controlW + 8, y, 48, kControlHeight);
    opacityValueLabel_.setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
    opacityValueLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    opacityValueLabel_.setJustificationType(juce::Justification::centredLeft);
    opacityValueLabel_.setText(juce::String(pending_.opacityPercent, 0) + "%",
                               juce::dontSendNotification);
    contentPanel_->addAndMakeVisible(opacityValueLabel_);

    if (!opacitySupported_)
    {
        opacitySlider_.setEnabled(false);
        opacityValueLabel_.setText("100% (unsupported)", juce::dontSendNotification);
    }

    y += kControlHeight + 12;

    // --- Platform-specific blur/Acrylic control ---
    // On macOS: blur slider (radius 0–100)
    // On Windows: Acrylic toggle (on/off)
    // On Linux: show warning message if unsupported

    if (!opacitySupported_)
    {
        // Linux without compositor — show warning, no blur control
        opacityWarningLabel_ = new juce::Label();
        opacityWarningLabel_->setText("Background transparency and blur are unavailable "
                                      "on this Linux environment (no compositor detected).",
                                      juce::dontSendNotification);
        opacityWarningLabel_->setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
        opacityWarningLabel_->setColour(juce::Label::textColourId, palette.textMuted);
        opacityWarningLabel_->setJustificationType(juce::Justification::centredLeft);
        opacityWarningLabel_->setBounds(0, y, 600, kControlHeight * 2);
        opacityWarningLabel_->setInterceptsMouseClicks(false, false);
        contentPanel_->addAndMakeVisible(opacityWarningLabel_);

        y += kControlHeight * 2 + 8;
    }
    else
    {
        // macOS: blur slider
        // Windows: Acrylic toggle
#if JUCE_MAC
        // --- Blur slider ---
        auto* blurLabel = new juce::Label();
        blurLabel->setText("Blur:", juce::dontSendNotification);
        blurLabel->setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
        blurLabel->setColour(juce::Label::textColourId, palette.textSecondary);
        blurLabel->setJustificationType(juce::Justification::centredRight);
        blurLabel->setBounds(0, y, labelW, kControlHeight);
        contentPanel_->addAndMakeVisible(blurLabel);

        blurSlider_.setBounds(controlX, y, controlW, kControlHeight);
        blurSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
        blurSlider_.setRange(0.0, 100.0, 1.0);
        blurSlider_.setValue(pending_.macosBlurRadius, juce::dontSendNotification);
        blurSlider_.addListener(this);
        contentPanel_->addAndMakeVisible(blurSlider_);

        blurValueLabel_.setBounds(controlX + controlW + 8, y, 48, kControlHeight);
        blurValueLabel_.setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
        blurValueLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
        blurValueLabel_.setJustificationType(juce::Justification::centredLeft);
        blurValueLabel_.setText(juce::String(pending_.macosBlurRadius),
                                 juce::dontSendNotification);
        contentPanel_->addAndMakeVisible(blurValueLabel_);

        y += kControlHeight + 12;
#elif JUCE_WINDOWS
        // --- Acrylic toggle ---
        acrylicButton_.setButtonText("Acrylic background");
        acrylicButton_.setClickingTogglesButton(true);
        acrylicButton_.setToggleButtonAction(true);
        acrylicButton_.setBounds(controlX, y, controlW, kControlHeight);
        acrylicButton_.setButtonText(juce::String(pending_.windowsAcrylic ? "ON" : "OFF"));
        acrylicButton_.addListener(this);
        contentPanel_->addAndMakeVisible(acrylicButton_);

        acrylicLabel_.setBounds(0, y, labelW, kControlHeight);
        acrylicLabel_.setText("Acrylic:", juce::dontSendNotification);
        acrylicLabel_.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
        acrylicLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
        acrylicLabel_.setJustificationType(juce::Justification::centredRight);
        contentPanel_->addAndMakeVisible(acrylicLabel_);

        y += kControlHeight + 12;
#else
        // Linux with transparency support — show a simple message
        auto* blurInfo = new juce::Label();
        blurInfo->setText("Background blur requires a compositor with blur support.",
                          juce::dontSendNotification);
        blurInfo->setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
        blurInfo->setColour(juce::Label::textColourId, palette.textMuted);
        blurInfo->setJustificationType(juce::Justification::centredLeft);
        blurInfo->setBounds(0, y, 600, kControlHeight);
        blurInfo->setInterceptsMouseClicks(false, false);
        contentPanel_->addAndMakeVisible(blurInfo);

        y += kControlHeight + 8;
#endif
    }

    // Update initial blur control state
    updateBlurControlState();
}

void SettingsComponent::buildAgentSection(const std::string& hathorMcpPath)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const int labelW   = kLabelWidth;
    const int controlX = labelW + 8;
    const int controlW = 300;
    const int contentW = 600;

    int y = contentPanel_->getHeight() + 8;

    // --- Section header ---
    auto* header = new juce::Label();
    header->setText("Agent / ACP", juce::dontSendNotification);
    header->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    header->setColour(juce::Label::textColourId, palette.textPrimary);
    header->setJustificationType(juce::Justification::centredLeft);
    header->setBounds(0, y, contentW, kControlHeight);
    contentPanel_->addAndMakeVisible(header);

    y += kControlHeight + 8;

    // --- Agent executable path ---
    agentPathLabel_ = new juce::Label();
    agentPathLabel_->setText("Agent exe:", juce::dontSendNotification);
    agentPathLabel_->setBounds(0, y, labelW, kControlHeight);
    agentPathLabel_->setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    agentPathLabel_->setColour(juce::Label::textColourId, palette.textSecondary);
    agentPathLabel_->setJustificationType(juce::Justification::centredRight);
    contentPanel_->addAndMakeVisible(*agentPathLabel_);

    agentPathEditor_.setBounds(controlX, y, controlW, kControlHeight);
    agentPathEditor_.setText(juce::String(pending_.agentExePath), juce::dontSendNotification);
    agentPathEditor_.addListener(this);
    contentPanel_->addAndMakeVisible(agentPathEditor_);

    y += kControlHeight + 4;

    // --- hathor-mcp path (read-only, inferred) ---
    auto* mcpLabel = new juce::Label();
    mcpLabel->setText("hathor-mcp:", juce::dontSendNotification);
    mcpLabel->setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    mcpLabel->setColour(juce::Label::textColourId, palette.textSecondary);
    mcpLabel->setJustificationType(juce::Justification::centredRight);
    mcpLabel->setBounds(0, y, labelW, kControlHeight);
    contentPanel_->addAndMakeVisible(mcpLabel);

    mcpPathLabel_.setBounds(controlX, y, controlW, kControlHeight);
    mcpPathLabel_.setText(hathorMcpPath.empty()
                              ? "(not found beside executable)"
                              : juce::String(hathorMcpPath),
                          juce::dontSendNotification);
    mcpPathLabel_.setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
    mcpPathLabel_.setColour(juce::Label::textColourId, palette.textMuted);
    mcpPathLabel_.setJustificationType(juce::Justification::centredLeft);
    contentPanel_->addAndMakeVisible(mcpPathLabel_);

    y += kControlHeight + 8;

    contentPanel_->setBounds(0, 0, contentW, y + 8);
}

void SettingsComponent::buildPetdexSection()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const int labelW   = kLabelWidth;
    const int controlX = labelW + 8;
    const int controlW = 300;
    const int contentW = 600;

    int y = contentPanel_->getHeight() + 8;

    // --- Section header ---
    auto* header = new juce::Label();
    header->setText("Petdex", juce::dontSendNotification);
    header->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    header->setColour(juce::Label::textColourId, palette.textPrimary);
    header->setJustificationType(juce::Justification::centredLeft);
    header->setBounds(0, y, contentW, kControlHeight);
    contentPanel_->addAndMakeVisible(header);

    y += kControlHeight + 8;

    // --- Pet selection combo ---
    petLabel_.setBounds(0, y, labelW, kControlHeight);
    petLabel_.setText("Mascot:", juce::dontSendNotification);
    petLabel_.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    petLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    petLabel_.setJustificationType(juce::Justification::centredRight);
    contentPanel_->addAndMakeVisible(petLabel_);

    petCombo_.setBounds(controlX, y, controlW, kControlHeight);
    petCombo_.setEditableText(false);

    populatePetList();

    if (!pending_.petSelection.empty())
    {
        for (int i = 1; i <= petCombo_.getNumItems(); ++i)
        {
            if (petCombo_.getItemText(i - 1) == juce::String(pending_.petSelection))
            {
                petCombo_.setSelectedId(i, juce::dontSendNotification);
                break;
            }
        }
    }
    else
    {
        petCombo_.setText("(none)", juce::dontSendNotification);
    }

    petCombo_.addListener(this);
    contentPanel_->addAndMakeVisible(petCombo_);

    y += kControlHeight + 24;

    contentPanel_->setBounds(0, 0, contentW, y);
}

void SettingsComponent::buildChuckPlaceholder()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    int y = contentPanel_->getHeight() + 8;

    auto* header = new juce::Label();
    header->setText("ChucK", juce::dontSendNotification);
    header->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    header->setColour(juce::Label::textColourId, palette.textPrimary);
    header->setJustificationType(juce::Justification::centredLeft);
    header->setBounds(0, y, 600, kControlHeight);
    contentPanel_->addAndMakeVisible(header);

    y += kControlHeight + 8;

    auto* note = new juce::Label();
    note->setText("ChucK integration - implemented in Phase C (B4).",
                  juce::dontSendNotification);
    note->setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
    note->setColour(juce::Label::textColourId, palette.textMuted);
    note->setJustificationType(juce::Justification::centredLeft);
    note->setBounds(0, y, 600, kControlHeight);
    contentPanel_->addAndMakeVisible(note);

    y += kControlHeight + 24;

    contentPanel_->setBounds(0, 0, 600, y);
}

void SettingsComponent::buildAudioSection(int& y)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const int labelW   = kLabelWidth;
    const int controlX = labelW + 8;
    const int controlW = 300;
    const int contentW = 600;

    // --- Master EQ Preset label ---
    eqPresetLabel_.setBounds(0, y, labelW, kControlHeight);
    eqPresetLabel_.setText("Master EQ Preset:", juce::dontSendNotification);
    eqPresetLabel_.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    eqPresetLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    eqPresetLabel_.setJustificationType(juce::Justification::centredRight);
    contentPanel_->addAndMakeVisible(eqPresetLabel_);

    // --- Preset selector combo box ---
    // Uses the existing B7-K2 EqPreset enum.  Each preset is added with a
    // stable ID = static_cast<int>(EqPreset) + 1 (JUCE combo box IDs are 1-based
    // and 0 means "no selection").  The presetName() function from MasterEq.hpp
    // provides the human-readable display labels.
    eqPresetCombo_.setBounds(controlX, y, controlW, kControlHeight);
    eqPresetCombo_.setEditableText(false);

    for (int i = 0; i < hathor::kNumEqPresets; ++i) {
        const hathor::EqPreset preset = hathor::allPresets()[static_cast<std::size_t>(i)];
        eqPresetCombo_.addItem(juce::String(hathor::presetName(preset)),
                               static_cast<int>(preset) + 1);
    }

    eqPresetCombo_.setSelectedId(
        static_cast<int>(pending_.eqPreset) + 1,
        juce::dontSendNotification);
    eqPresetCombo_.addListener(this);
    contentPanel_->addAndMakeVisible(eqPresetCombo_);

    y += kControlHeight + 16;

    contentPanel_->setBounds(0, 0, contentW, y);
}

void SettingsComponent::buildActionButtons()
{
    applyButton_.setButtonText("Apply");
    applyButton_.addListener(this);
    applyButton_.setEnabled(false);
    addAndMakeVisible(applyButton_);

    resetButton_.setButtonText("Reset");
    resetButton_.addListener(this);
    addAndMakeVisible(resetButton_);
}

void SettingsComponent::populatePetList()
{
    petCombo_.clear();
    petCombo_.addItem("(none)", 1);

    if (pending_.petSelection.empty())
        petCombo_.setText("(none)", juce::dontSendNotification);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void SettingsComponent::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    g.fillAll(palette.surfaceLow);
}

void SettingsComponent::resized()
{
    auto b = getLocalBounds();

    const int buttonBarHeight = kButtonHeight + 16;

    scrollView_->setBounds(b.removeFromTop(b.getHeight() - buttonBarHeight));

    contentPanel_->setTopLeftPosition(0, 0);
    contentPanel_->setSize(juce::jmax(400, b.getWidth()),
                           contentPanel_->getHeight());

    const int bx = b.getWidth() - kButtonWidth * 2 - kButtonGap;
    applyButton_.setBounds(bx, b.getHeight() - kButtonHeight,
                           kButtonWidth, kButtonHeight);
    resetButton_.setBounds(bx + kButtonWidth + kButtonGap,
                           b.getHeight() - kButtonHeight,
                           kButtonWidth, kButtonHeight);
}

// ---------------------------------------------------------------------------
// Slider::Listener
// ---------------------------------------------------------------------------

void SettingsComponent::sliderValueChanged(juce::Slider* slider)
{
    if (slider == &opacitySlider_)
    {
        const float val = static_cast<float>(slider->getValue());
        pending_.opacityPercent = val;

        opacityValueLabel_.setText(juce::String(val, 0) + "%",
                                   juce::dontSendNotification);

        // B5: When opacity is 100%, blur has no visible effect — disable blur controls
        updateBlurControlState();

        // Live preview: apply opacity immediately through the controller
        // (the edit buffer is NOT committed until Apply is pressed)
        if (appearanceController_ != nullptr)
            appearanceController_->applyOpacity(val);

        updateDirtyFlag();
    }
    else if (slider == &blurSlider_)
    {
        const int val = static_cast<int>(slider->getValue());
        pending_.macosBlurRadius = val;

        blurValueLabel_.setText(juce::String(val),
                                juce::dontSendNotification);

        // Live preview: apply blur immediately through the controller
        if (appearanceController_ != nullptr)
        {
            WindowAppearanceState state;
            state.opacityPercent  = pending_.opacityPercent;
            state.macosBlurRadius = pending_.macosBlurRadius;
            state.windowsAcrylic  = pending_.windowsAcrylic;
            appearanceController_->applyBlur(state);
        }

        updateDirtyFlag();
    }
}

// ---------------------------------------------------------------------------
// Button::Listener
// ---------------------------------------------------------------------------

void SettingsComponent::buttonClicked(juce::Button* button)
{
    if (button == &applyButton_)
    {
        committed_ = pending_;
        saveSettings(committed_);
        applyTheme(committed_.theme);
        applyWindowAppearance(committed_);

        // B7-K3: Apply the EQ preset to the live AudioEngine via B7-K2's
        // atomic swap.  This is the only place the EQ preset reaches the
        // engine — the UI does NOT touch coefficients or audio-thread state.
        applyEqPreset(committed_.eqPreset);

        pendingChanges_ = false;
        applyButton_.setEnabled(false);
        resetButton_.setEnabled(false);

        committed_ = loadSettings();

        if (onSettingsApplied)
            onSettingsApplied();

        if (onEqPresetApplied)
            onEqPresetApplied(committed_.eqPreset);
    }
    else if (button == &resetButton_)
    {
        resetToCommitted();
    }
    else if (button == &acrylicButton_)
    {
        // Toggle Acrylic state (B5)
        pending_.windowsAcrylic = !pending_.windowsAcrylic;
        acrylicButton_.setButtonText(pending_.windowsAcrylic ? "ON" : "OFF");

        // Live preview: apply Acrylic through the controller
        if (appearanceController_ != nullptr && pending_.opacityPercent < 100.0f)
        {
            WindowAppearanceState state;
            state.opacityPercent  = pending_.opacityPercent;
            state.windowsAcrylic  = pending_.windowsAcrylic;
            state.macosBlurRadius = pending_.macosBlurRadius;
            appearanceController_->applyBlur(state);
        }

        updateDirtyFlag();
    }
}

// ---------------------------------------------------------------------------
// ComboBox::Listener
// ---------------------------------------------------------------------------

void SettingsComponent::comboBoxChanged(juce::ComboBox* comboBox)
{
    if (comboBox == &themeCombo_)
    {
        const int selectedId = comboBox->getSelectedId();
        if (selectedId > 0)
        {
            pending_.theme = static_cast<ThemeId>(selectedId - 1);
            updateDirtyFlag();
        }
    }
    else if (comboBox == &petCombo_)
    {
        if (comboBox->getSelectedId() == 1)
            pending_.petSelection = "";
        else
            pending_.petSelection = comboBox->getText().toStdString();

        updateDirtyFlag();
    }
    else if (comboBox == &eqPresetCombo_)
    {
        const int selectedId = comboBox->getSelectedId();
        if (selectedId > 0)
        {
            pending_.eqPreset = static_cast<hathor::EqPreset>(selectedId - 1);
            updateDirtyFlag();
        }
    }
}

// ---------------------------------------------------------------------------
// TextEditor::Listener
// ---------------------------------------------------------------------------

void SettingsComponent::textEditorTextChanged(juce::TextEditor& editor)
{
    if (&editor == &agentPathEditor_)
    {
        pending_.agentExePath = editor.getText().toStdString();
        updateDirtyFlag();
    }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void SettingsComponent::updateDirtyFlag()
{
    pendingChanges_ = (pending_.theme           != committed_.theme)
                   || (pending_.opacityPercent  != committed_.opacityPercent)
                   || (pending_.macosBlurRadius != committed_.macosBlurRadius)
                   || (pending_.windowsAcrylic  != committed_.windowsAcrylic)
                   || (pending_.agentExePath    != committed_.agentExePath)
                   || (pending_.petSelection    != committed_.petSelection)
                   || (pending_.eqPreset        != committed_.eqPreset);

    applyButton_.setEnabled(pendingChanges_);
    resetButton_.setEnabled(pendingChanges_);
}

void SettingsComponent::applyTheme(ThemeId theme)
{
    const Palette newPalette = paletteForTheme(theme);

    HathorLookAndFeel& lookAndFeel = HathorLookAndFeel::fromComponent(*this);
    lookAndFeel.setPalette(newPalette);
}

void SettingsComponent::applyWindowAppearance(const SettingsModel& model)
{
    // B5: Apply opacity + blur/Acrylic to the live window.
    // Uses the WindowAppearanceController if available, otherwise falls back
    // to direct setAlpha on the MainWindow.

    if (appearanceController_ != nullptr)
    {
        WindowAppearanceState state;
        state.opacityPercent   = model.opacityPercent;
        state.macosBlurRadius  = model.macosBlurRadius;
        state.windowsAcrylic   = model.windowsAcrylic;
        appearanceController_->apply(state);
    }
    else
    {
        // Fallback: direct opacity (no blur/acrylic without controller)
        if (opacitySupported_)
        {
            for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
            {
                if (auto* w = dynamic_cast<juce::TopLevelWindow*>(
                        juce::Desktop::getInstance().getComponent(i)))
                {
                    if (w->getName() == "Hathor")
                    {
                        w->setAlpha(model.opacityPercent / 100.0f);
                        w->setOpaque(false);
                        return;
                    }
                }
            }
        }
    }
}

void SettingsComponent::applyEqPreset(hathor::EqPreset preset)
{
    // B7-K3: Route the preset selection to B7-K2's existing atomic swap.
    // The AudioEngine computes coefficients on the control thread and publishes
    // a complete MasterEqState via std::atomic_store — no mutex, no allocation
    // on the audio thread.  The UI never touches DSP state directly.
    if (audioEngine_ != nullptr)
        audioEngine_->setMasterEqPreset(preset);
}

void SettingsComponent::updateBlurControlState()
{
    // B5: When opacity is 100%, blur has no visible effect — disable blur controls
    // but do NOT destroy the stored preference value.
    const bool blurActive = pending_.opacityPercent < 100.0f;

#if JUCE_MAC
    blurSlider_.setEnabled(blurActive);
    blurValueLabel_.setEnabled(blurActive);
#elif JUCE_WINDOWS
    acrylicButton_.setEnabled(blurActive);
    acrylicLabel_.setEnabled(blurActive);
#endif

    // Linux: opacitySupported_ is already false, so no blur controls to update
}

void SettingsComponent::resetToCommitted()
{
    pending_ = committed_;

    themeCombo_.setSelectedId(static_cast<int>(pending_.theme) + 1,
                              juce::dontSendNotification);

    opacitySlider_.setValue(pending_.opacityPercent,
                            juce::dontSendNotification);
    opacityValueLabel_.setText(juce::String(pending_.opacityPercent, 0) + "%",
                               juce::dontSendNotification);

#if JUCE_MAC
    blurSlider_.setValue(pending_.macosBlurRadius,
                         juce::dontSendNotification);
    blurValueLabel_.setText(juce::String(pending_.macosBlurRadius),
                            juce::dontSendNotification);
#elif JUCE_WINDOWS
    acrylicButton_.setButtonText(pending_.windowsAcrylic ? "ON" : "OFF");
#endif

    agentPathEditor_.setText(juce::String(pending_.agentExePath),
                             juce::dontSendNotification);

    if (!pending_.petSelection.empty())
    {
        for (int i = 1; i <= petCombo_.getNumItems(); ++i)
        {
            if (petCombo_.getItemText(i - 1) == juce::String(pending_.petSelection))
            {
                petCombo_.setSelectedId(i, juce::dontSendNotification);
                break;
            }
        }
    }
    else
    {
        petCombo_.setText("(none)", juce::dontSendNotification);
    }

    // B7-K3: Restore the EQ preset combo to the committed (last-applied) value.
    // Reset does NOT push to the engine — the live audio stays on the applied preset.
    eqPresetCombo_.setSelectedId(static_cast<int>(pending_.eqPreset) + 1,
                                 juce::dontSendNotification);

    // Restore live preview to the committed state
    applyWindowAppearance(committed_);
    if (appearanceController_ != nullptr && committed_.opacityPercent < 100.0f)
    {
        WindowAppearanceState state;
        state.opacityPercent  = committed_.opacityPercent;
        state.macosBlurRadius = committed_.macosBlurRadius;
        state.windowsAcrylic  = committed_.windowsAcrylic;
        appearanceController_->applyBlur(state);
    }

    updateBlurControlState();
    updateDirtyFlag();
}

} // namespace hathor::ui
