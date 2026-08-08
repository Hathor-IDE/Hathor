// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * SettingsComponent.cpp — implementation of the A2 settings tab.
 *
 * Requirements: A2, B3, B5
 */

#include "SettingsComponent.hpp"

#include <cstdlib>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

SettingsComponent::SettingsComponent(juce::ApplicationProperties* props)
    : appProperties_(props)
{
    setInterceptsMouseClicks(true, true);

    // Determine platform opacity support (B5).
#if JUCE_LINUX
    opacitySupported_ = false;
#else
    opacitySupported_ = true;
#endif

    // Load committed (source-of-truth) settings from properties.
    committed_ = loadSettings();
    pending_    = committed_;

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
    buildEqPlaceholder();
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
// Settings persistence (ApplicationProperties pattern from MainWindow.cpp:193)
// ---------------------------------------------------------------------------

SettingsComponent::SettingsModel
SettingsComponent::loadSettings() const
{
    SettingsModel m;

    if (appProperties_ == nullptr)
    {
        m.theme          = ThemeId::Dark;
        m.opacityPercent = opacitySupported_ ? 70.0f : 100.0f;
        m.agentExePath   = "";
        m.petSelection   = "";
        return m;
    }

    const auto* props = appProperties_->getUserSettings();
    if (props == nullptr)
    {
        m.theme          = ThemeId::Dark;
        m.opacityPercent = opacitySupported_ ? 70.0f : 100.0f;
        m.agentExePath   = std::getenv("HATHOR_AGENT")
                              ? std::string(std::getenv("HATHOR_AGENT"))
                              : "";
        m.petSelection   = "";
        return m;
    }

    const int themeIdx = props->getIntValue("settings.theme",
                                             static_cast<int>(ThemeId::Dark));

    // Clamp to the valid ThemeId range so a stale/unknown persisted value
    // (e.g. from a future version that added a 6th theme, or a corrupt prefs
    // file) falls back safely to Dark rather than producing UB via a raw
    // static_cast (B3 persistence-safety requirement).
    static constexpr int kMinTheme = static_cast<int>(ThemeId::Dark);
    static constexpr int kMaxTheme = static_cast<int>(ThemeId::Light);
    const int clampedTheme = juce::jlimit(kMinTheme, kMaxTheme, themeIdx);
    m.theme = static_cast<ThemeId>(clampedTheme);

    m.opacityPercent = props->getValue("settings.opacity",
                                       juce::String(opacitySupported_ ? 70.0f : 100.0f))
                            .getFloatValue();

    m.agentExePath = props->getValue("settings.agentExePath").toStdString();
    m.petSelection = props->getValue("settings.petSelection").toStdString();

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
    props->setValue("settings.agentExePath",   juce::String(model.agentExePath));
    props->setValue("settings.petSelection",   juce::String(model.petSelection));

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
    opacitySlider_.setRange(20.0, 100.0, 1.0);
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
        opacityValueLabel_.setText("100% (Linux: unsupported)", juce::dontSendNotification);
    }

    y += kControlHeight + 12;

    contentPanel_->setBounds(0, 0, contentW, y + 16);
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
    agentPathLabel_.setBounds(0, y, labelW, kControlHeight);
    agentPathLabel_.setText("Agent exe:", juce::dontSendNotification);
    agentPathLabel_.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    agentPathLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    agentPathLabel_.setJustificationType(juce::Justification::centredRight);
    contentPanel_->addAndMakeVisible(agentPathLabel_);

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

    // Set from pending (empty = no selection → "(none)" hint)
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

void SettingsComponent::buildEqPlaceholder()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    int y = contentPanel_->getHeight() + 8;

    auto* header = new juce::Label();
    header->setText("EQ", juce::dontSendNotification);
    header->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    header->setColour(juce::Label::textColourId, palette.textPrimary);
    header->setJustificationType(juce::Justification::centredLeft);
    header->setBounds(0, y, 600, kControlHeight);
    contentPanel_->addAndMakeVisible(header);

    y += kControlHeight + 8;

    auto* note = new juce::Label();
    note->setText("EQ integration - implemented in Phase D (B7).",
                  juce::dontSendNotification);
    note->setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
    note->setColour(juce::Label::textColourId, palette.textMuted);
    note->setJustificationType(juce::Justification::centredLeft);
    note->setBounds(0, y, 600, kControlHeight);
    contentPanel_->addAndMakeVisible(note);

    y += kControlHeight + 24;

    contentPanel_->setBounds(0, 0, 600, y);
}

void SettingsComponent::buildActionButtons()
{
    // Buttons are laid out in resized(). Only added as children here.
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

    // Placeholder for future mascot discovery (D1-D4):
    //   petCombo_.addItem("Strudel Fox", 2);
    //   petCombo_.addItem("Hathor Cat",  3);
    // Actual mascot assets and licensing data arrive in D1-D4.

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

    // Sync content panel width to the viewport.
    contentPanel_->setTopLeftPosition(0, 0);
    contentPanel_->setSize(juce::jmax(400, b.getWidth()),
                           contentPanel_->getHeight());

    // Buttons pinned to bottom-right of the full bounds (below scroll view).
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
        applyOpacity(committed_.opacityPercent);
        pendingChanges_ = false;
        applyButton_.setEnabled(false);
        resetButton_.setEnabled(false);

        committed_ = loadSettings();

        if (onSettingsApplied)
            onSettingsApplied();
    }
    else if (button == &resetButton_)
    {
        resetToCommitted();
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
    pendingChanges_ = (pending_.theme != committed_.theme)
                   || (pending_.opacityPercent != committed_.opacityPercent)
                   || (pending_.agentExePath != committed_.agentExePath)
                   || (pending_.petSelection != committed_.petSelection);

    applyButton_.setEnabled(pendingChanges_);
    resetButton_.setEnabled(pendingChanges_);
}

void SettingsComponent::applyTheme(ThemeId theme)
{
    const Palette newPalette = paletteForTheme(theme);

    // Resolve the HathorLookAndFeel instance installed on the window hierarchy
    // (MainWindow calls setLookAndFeel(&lookAndFeel_)) and swap its palette.
    // setPalette() calls sendLookAndFeelChange() on all live components, so
    // every themed zone re-paints with the new palette immediately (B3).
    HathorLookAndFeel& lookAndFeel = HathorLookAndFeel::fromComponent(*this);
    lookAndFeel.setPalette(newPalette);
}

void SettingsComponent::applyOpacity(float percent)
{
    if (!opacitySupported_)
        return;

    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
    {
        if (auto* w = dynamic_cast<juce::TopLevelWindow*>(
                juce::Desktop::getInstance().getComponent(i)))
        {
            // MainWindow's component name is "Hathor" (set in DocumentWindow ctor).
            if (w->getName() == "Hathor")
            {
                w->setAlpha(percent / 100.0f);
                return;
            }
        }
    }
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

    updateDirtyFlag();
}

} // namespace hathor::ui
