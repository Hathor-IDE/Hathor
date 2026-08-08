// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * SettingsComponent.cpp — implementation of the A2 settings tab.
 *
 * Requirements: A2, B3, B5
 */

#include "SettingsComponent.hpp"

// Need the Petdex data if available (D1–D4). For A2 we show a select-only
// combo with no default; actual mascot rendering is deferred to D1–D4.

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

    // Theme
    const int themeIdx = props->getIntValue("settings.theme",
                                             static_cast<int>(ThemeId::Dark));
    m.theme = static_cast<ThemeId>(themeIdx);

    // Opacity
    m.opacityPercent = props->getValue("settings.opacity",
                                       juce::String(opacitySupported_ ? 70.0f : 100.0f))
                           .getFloatValue();

    // Agent path
    m.agentExePath = props->getValue("settings.agentExePath").toStdString();

    // Pet selection
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
    props->setValue("settings.agentExePath",   model.agentExePath);
    props->setValue("settings.petSelection",   model.petSelection);

    props->saveIfNeeded();
}

// ---------------------------------------------------------------------------
// Section builders
// ---------------------------------------------------------------------------

void SettingsComponent::buildAppearanceSection()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const int y = 0;
    const int labelW = kLabelWidth;
    const int controlX = labelW + 8;
    const int controlW = 300;

    // --- Section header ---
    auto* header = new juce::Label();
    header->setText("Appearance", juce::dontSendNotification);
    header->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    header->setColour(juce::Label::textColourId, palette.textPrimary);
    header->setJustificationType(juce::Justification::centredLeft);
    header->setBounds(0, y, 400, kControlHeight);
    contentPanel_->addAndMakeVisible(header);

    int rowY = y + kControlHeight + 8;

    // --- Theme picker ---
    auto* themeLabel = new juce::Label();
    themeLabel->setText("Theme:", juce::dontSendNotification);
    themeLabel->setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    themeLabel->setColour(juce::Label::textColourId, palette.textSecondary);
    themeLabel->setJustificationType(juce::Justification::centredRight);
    themeLabel->setBounds(0, rowY, labelW, kControlHeight);
    contentPanel_->addAndMakeVisible(themeLabel);

    themeCombo_.setBounds(controlX, rowY, controlW, kControlHeight);
    themeCombo_.setEditableText(false);

    // Register friendly names for each theme (B3).
    themeCombo_.addChoice("Dark",           static_cast<int>(ThemeId::Dark) + 1);
    themeCombo_.addChoice("Purple / Neon",  static_cast<int>(ThemeId::PurpleNeon) + 1);
    themeCombo_.addChoice("Capuchin",       static_cast<int>(ThemeId::Capuchin) + 1);
    themeCombo_.addChoice("Sand",           static_cast<int>(ThemeId::Sand) + 1);
    themeCombo_.addChoice("Light",          static_cast<int>(ThemeId::Light) + 1);

    // Set current value from pending.
    themeCombo_.setSelectedId(static_cast<int>(pending_.theme) + 1,
                              juce::dontSendNotification);
    themeCombo_.addListener(this);
    contentPanel_->addAndMakeVisible(themeCombo_);

    rowY += kControlHeight + 12;

    // --- Opacity slider ---
    auto* opacityLabel = new juce::Label();
    opacityLabel->setText("Opacity:", juce::dontSendNotification);
    opacityLabel->setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    opacityLabel->setColour(juce::Label::textColourId, palette.textSecondary);
    opacityLabel->setJustificationType(juce::Justification::centredRight);
    opacityLabel->setBounds(0, rowY, labelW, kControlHeight);
    contentPanel_->addAndMakeVisible(opacityLabel);

    opacitySlider_.setBounds(controlX, rowY, controlW, kControlHeight);
    opacitySlider_.setSliderStyle(juce::Slider::LinearHorizontal);
    opacitySlider_.setRange(20.0, 100.0, 1.0);
    opacitySlider_.setSnapToOneDecimalPlace(0);
    opacitySlider_.setDoubleClickReadOnly(false);
    opacitySlider_.setValue(pending_.opacityPercent, juce::dontSendNotification);
    opacitySlider_.addListener(this);
    contentPanel_->addAndMakeVisible(opacitySlider_);

    opacityValueLabel_.setBounds(controlX + controlW + 8, rowY, 48, kControlHeight);
    opacityValueLabel_.setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
    opacityValueLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    opacityValueLabel_.setJustificationType(juce::Justification::centredLeft);
    opacityValueLabel_.setText(juce::String(pending_.opacityPercent) + "%",
                               juce::dontSendNotification);
    contentPanel_->addAndMakeVisible(opacityValueLabel_);

    if (!opacitySupported_)
    {
        opacitySlider_.setEnabled(false);
        opacityValueLabel_.setText("100% (Linux: unsupported)", juce::dontSendNotification);
    }

    rowY += kControlHeight + 12;

    // Store section height for layout. Content panel will be sized dynamically.
    contentPanel_->setBounds(0, 0, 600, rowY + 16);
}

void SettingsComponent::buildAgentSection(const std::string& hathorMcpPath)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const int startX = 0;
    const int labelW = kLabelWidth;
    const int controlX = labelW + 8;
    const int controlW = 300;

    int y = contentPanel_->getHeight() + 8;
    int sectionStartY = y;

    // --- Section header ---
    auto* header = new juce::Label();
    header->setText("Agent / ACP", juce::dontSendNotification);
    header->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    header->setColour(juce::Label::textColourId, palette.textPrimary);
    header->setJustificationType(juce::Justification::centredLeft);
    header->setBounds(startX, y, 400, kControlHeight);
    contentPanel_->addAndMakeVisible(header);

    y += kControlHeight + 8;

    // --- Agent executable path ---
    agentPathLabel_.setBounds(startX, y, labelW, kControlHeight);
    agentPathLabel_.setText("Agent exe:", juce::dontSendNotification);
    agentPathLabel_.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    agentPathLabel_.setColour(juce::Label::textColourId, palette.textSecondary);
    agentPathLabel_.setJustificationType(juce::Justification::centredRight);
    contentPanel_->addAndMakeVisible(agentPathLabel_);

    agentPathEditor_.setBounds(controlX, y, controlW, kControlHeight);
    agentPathEditor_.setText(juce::String(pending_.agentExePath), juce::dontSendNotification);
    contentPanel_->addAndMakeVisible(agentPathEditor_);

    y += kControlHeight + 4;

    // --- hathor-mcp path (read-only, inferred) ---
    auto* mcpLabel = new juce::Label();
    mcpLabel->setText("hathor-mcp:", juce::dontSendNotification);
    mcpLabel->setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::bodySm));
    mcpLabel->setColour(juce::Label::textColourId, palette.textSecondary);
    mcpLabel->setJustificationType(juce::Justification::centredRight);
    mcpLabel->setBounds(startX, y, labelW, kControlHeight);
    contentPanel_->addAndMakeVisible(mcpLabel);

    mcpPathLabel_.setBounds(controlX, y, controlW, kControlHeight);
    mcpPathLabel_.setText(juce::String(hathorMcpPath).isEmpty()
                              ? "(not found beside executable)"
                              : juce::String(hathorMcpPath),
                          juce::dontSendNotification);
    mcpPathLabel_.setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
    mcpPathLabel_.setColour(juce::Label::textColourId, palette.textMuted);
    mcpPathLabel_.setJustificationType(juce::Justification::centredLeft);
    contentPanel_->addAndMakeVisible(mcpPathLabel_);

    y += kControlHeight + 8;

    // Section height update
    contentPanel_->setBounds(0, 0, 600, y + 8);

    // Adjust previous sections' heights — actually we need to reposition.
    // Simpler approach: we rebuild from scratch in resized().
    // For now, just track the bottom.
    juce::ignoreUnused(sectionStartY);
}

void SettingsComponent::buildPetdexSection()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const int labelW = kLabelWidth;
    const int controlX = labelW + 8;
    const int controlW = 300;

    int y = contentPanel_->getHeight() + 8;

    // --- Section header ---
    auto* header = new juce::Label();
    header->setText("Petdex", juce::dontSendNotification);
    header->setFont(HathorLookAndFeel::fontSemiBold(HathorLookAndFeel::Typography::headlineMd));
    header->setColour(juce::Label::textColourId, palette.textPrimary);
    header->setJustificationType(juce::Justification::centredLeft);
    header->setBounds(0, y, 400, kControlHeight);
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

    // Populate with known mascots (D1–D4 deferred). For A2, show a "none"
    // option plus any discovered mascots, but no default selection.
    populatePetList();

    // Set from pending (empty = no selection → shows "(none)" hint)
    if (pending_.petSelection.isNotEmpty())
    {
        for (int i = 0; i < petCombo_.getNumItems(); ++i)
        {
            if (petCombo_.getItem(i).reference == pending_.petSelection)
            {
                petCombo_.setSelectedId(petCombo_.getItem(i).id, juce::dontSendNotification);
                break;
            }
        }
    }

    petCombo_.addListener(this);
    contentPanel_->addAndMakeVisible(petCombo_);

    y += kControlHeight + 24;

    contentPanel_->setBounds(0, 0, 600, y);
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
    header->setBounds(0, y, 400, kControlHeight);
    contentPanel_->addAndMakeVisible(header);

    y += kControlHeight + 8;

    auto* note = new juce::Label();
    note->setText("ChucK integration — implemented in Phase C (B4).",
                  juce::dontSendNotification);
    note->setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
    note->setColour(juce::Label::textColourId, palette.textMuted);
    note->setJustificationType(juce::Justification::centredLeft);
    note->setBounds(0, y, 500, kControlHeight);
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
    header->setBounds(0, y, 400, kControlHeight);
    contentPanel_->addAndMakeVisible(header);

    y += kControlHeight + 8;

    auto* note = new juce::Label();
    note->setText("EQ integration — implemented in Phase D (B7).",
                  juce::dontSendNotification);
    note->setFont(HathorLookAndFeel::fontRegular(HathorLookAndFeel::Typography::bodySm));
    note->setColour(juce::Label::textColourId, palette.textMuted);
    note->setJustificationType(juce::Justification::centredLeft);
    note->setBounds(0, y, 500, kControlHeight);
    contentPanel_->addAndMakeVisible(note);

    y += kControlHeight + 24;

    contentPanel_->setBounds(0, 0, 600, y);
}

void SettingsComponent::buildActionButtons()
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    // Buttons are placed below the scroll content — managed in resized().
    applyButton_.setButtonText("Apply");
    applyButton_.setBounds(0, 0, kButtonWidth, kButtonHeight);
    applyButton_.addListener(this);
    applyButton_.setEnabled(false);  // only enabled when there are pending changes
    addAndMakeVisible(applyButton_);

    resetButton_.setButtonText("Reset");
    resetButton_.setBounds(kButtonWidth + kButtonGap, 0, kButtonWidth, kButtonHeight);
    resetButton_.addListener(this);
    addAndMakeVisible(resetButton_);
}

void SettingsComponent::populatePetList()
{
    // A2: Petdex is deferred to D1–D4. Show "None" as the only option —
    // no default selection (per A2: "opt-in only, no default").
    petCombo_.clear();
    petCombo_.addChoice("(none)", 1);

    // Placeholder for future mascot discovery (D1–D4):
    //   petCombo_.addChoice("Strudel Fox", 2);
    //   petCombo_.addChoice("Hathor Cat",  3);
    // Actual mascot assets and licensing data arrive in D1–D4.

    petCombo_.setText(juce::String(pending_.petSelection.isEmpty()
                                       ? "(none)"
                                       : pending_.petSelection));
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

    // Reserve space for buttons at the bottom of the content panel.
    const int buttonBarHeight = kButtonHeight + 16;
    contentPanel_->setTopLeftPosition(0, 0);

    // Expand content panel width to fit viewport (minus scrollbar).
    const int contentW = juce::jmax(400, b.getWidth() - 8);
    contentPanel_->setSize(contentW, contentPanel_->getHeight());

    // Scroll view fills the top portion.
    scrollView_->setBounds(b.removeFromTop(b.getHeight() - buttonBarHeight));

    // Buttons pinned to bottom-right.
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

        // Live-update the value label.
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
        // Commit pending → committed, persist, apply live.
        committed_ = pending_;
        saveSettings(committed_);
        applyTheme(committed_.theme);
        applyOpacity(committed_.opacityPercent);
        pendingChanges_ = false;
        applyButton_.setEnabled(false);
        resetButton_.setEnabled(false);

        // Re-load from properties to ensure consistency.
        committed_ = loadSettings();
    }
    else if (button == &resetButton_)
    {
        // Revert pending to committed (discard edits, NOT factory defaults).
        pending_ = committed_;

        // Sync UI controls back to committed values.
        themeCombo_.setSelectedId(static_cast<int>(pending_.theme) + 1,
                                  juce::dontSendNotification);
        opacitySlider_.setValue(pending_.opacityPercent,
                                juce::dontSendNotification);
        opacityValueLabel_.setText(juce::String(pending_.opacityPercent, 0) + "%",
                                   juce::dontSendNotification);
        agentPathEditor_.setText(juce::String(pending_.agentExePath),
                                 juce::dontSendNotification);

        if (!pending_.petSelection.isEmpty())
        {
            for (int i = 0; i < petCombo_.getNumItems(); ++i)
            {
                if (petCombo_.getItem(i).reference == pending_.petSelection)
                {
                    petCombo_.setSelectedId(petCombo_.getItem(i).id,
                                            juce::dontSendNotification);
                    break;
                }
            }
        }
        else
        {
            petCombo_.setText("(none)");
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
        const int selectedId = comboBox->getSelectedId();
        if (selectedId == 1)
        {
            pending_.petSelection = "";
        }
        else
        {
            pending_.petSelection = comboBox->getText().toStdString();
        }
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

    // Apply through the LookAndFeel so all components update.
    // The MainWindow owns the HathorLookAndFeel instance; we set the palette
    // on it via the global pointer.
    if (auto* lf = dynamic_cast<HathorLookAndFeel*>(
            juce::Desktop::getInstance().getDefaultLookAndFeel()))
    {
        lf->setPalette(newPalette);
    }
    else
    {
        // Fallback: set global palette directly.
        HathorLookAndFeel::setGlobalPalette(&newPalette);
        juce::Component::sendLookAndFeelChange();
    }
}

void SettingsComponent::applyOpacity(float percent)
{
    // B5: use TopLevelWindow::setAlpha on macOS/Windows.
    // On Linux, opacity is unsupported — skip.
    if (!opacitySupported_)
        return;

    // Iterate all top-level components and set alpha on the main window.
    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
    {
        if (auto* w = dynamic_cast<juce::TopLevelWindow*>(
                juce::Desktop::getInstance().getComponent(i)))
        {
            if (w->isMainWindow())
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

    // Sync UI controls.
    themeCombo_.setSelectedId(static_cast<int>(pending_.theme) + 1,
                              juce::dontSendNotification);
    opacitySlider_.setValue(pending_.opacityPercent,
                            juce::dontSendNotification);
    opacityValueLabel_.setText(juce::String(pending_.opacityPercent, 0) + "%",
                               juce::dontSendNotification);
    agentPathEditor_.setText(juce::String(pending_.agentExePath),
                             juce::dontSendNotification);

    if (!pending_.petSelection.isEmpty())
    {
        for (int i = 0; i < petCombo_.getNumItems(); ++i)
        {
            if (petCombo_.getItem(i).reference == pending_.petSelection)
            {
                petCombo_.setSelectedId(petCombo_.getItem(i).id,
                                        juce::dontSendNotification);
                break;
            }
        }
    }
    else
    {
        petCombo_.setText("(none)");
    }

    updateDirtyFlag();
}

} // namespace hathor::ui
