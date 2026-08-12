// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * StatusRibbon.cpp — L-3: compact bottom status ribbon implementation.
 *
 * Requirement references: L-3 §5
 */

#include "StatusRibbon.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

StatusRibbon::StatusRibbon(hathor::control::DiagnosticRegistry* registry)
    : registry_(registry)
{
    if (registry_)
    {
        registry_->setChangeCallback([this]()
        {
            auto counts = registry_->counts();
            errorCount_ = counts.errors;
            warningCount_ = counts.warnings;
            infoCount_ = counts.info;
            repaint();
        });
    }
}

// ---------------------------------------------------------------------------
// State mutators
// ---------------------------------------------------------------------------

void StatusRibbon::setErrorCount(int n) noexcept
{
    errorCount_ = n;
    repaint();
}

void StatusRibbon::setWarningCount(int n) noexcept
{
    warningCount_ = n;
    repaint();
}

void StatusRibbon::setInfoCount(int n) noexcept
{
    infoCount_ = n;
    repaint();
}

void StatusRibbon::setTransportRunning(bool running) noexcept
{
    transportRunning_ = running;
    repaint();
}

void StatusRibbon::setBpm(double bpm) noexcept
{
    bpm_ = bpm;
    repaint();
}

void StatusRibbon::setWorkerAlive(bool alive) noexcept
{
    workerAlive_ = alive;
    repaint();
}

void StatusRibbon::setLspConnected(bool connected) noexcept
{
    lspConnected_ = connected;
    repaint();
}

void StatusRibbon::setMasterGain(float gain) noexcept
{
    masterGain_ = gain;
    repaint();
}

// ---------------------------------------------------------------------------
// Git status
// ---------------------------------------------------------------------------

void StatusRibbon::setGitStatus(const std::string& branch,
                                int stagedCount,
                                int unstagedCount) noexcept
{
    gitBranch_ = branch;
    gitStagedCount_ = stagedCount;
    gitUnstagedCount_ = unstagedCount;
    repaint();
}

void StatusRibbon::setRegistry(hathor::control::DiagnosticRegistry* registry) noexcept
{
    registry_ = registry;
    if (registry_)
    {
        registry_->setChangeCallback([this]()
        {
            auto counts = registry_->counts();
            errorCount_ = counts.errors;
            warningCount_ = counts.warnings;
            infoCount_ = counts.info;
            repaint();
        });
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void StatusRibbon::layoutIndicators()
{
    const int h = kRibbonHeight;
    const int indicatorW = 90;
    int x = 8;

    errorBox_.bounds     = {x, 4, indicatorW, h - 8}; errorBox_.active = errorCount_ > 0;     x += indicatorW + 4;
    warningBox_.bounds   = {x, 4, indicatorW, h - 8}; warningBox_.active = warningCount_ > 0; x += indicatorW + 4;
    transportBox_.bounds = {x, 4, indicatorW, h - 8}; transportBox_.active = transportRunning_; x += indicatorW + 4;
    workerBox_.bounds    = {x, 4, indicatorW, h - 8}; workerBox_.active = workerAlive_;        x += indicatorW + 4;
    lspBox_.bounds       = {x, 4, indicatorW, h - 8}; lspBox_.active = lspConnected_;         x += indicatorW + 4;
    gitBox_.bounds       = {x, 4, 140, h - 8}; gitBox_.active = (gitStagedCount_ + gitUnstagedCount_) > 0; x += 144;
    gainBox_.bounds      = {x, 4, getWidth() - x - 8, h - 8}; gainBox_.active = true;
}

// ---------------------------------------------------------------------------
// juce::Component
// ---------------------------------------------------------------------------

void StatusRibbon::resized()
{
    layoutIndicators();
}

void StatusRibbon::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    g.fillAll(palette.surfaceLow);

    // Top border (subtle divider from editor)
    g.setColour(palette.surfaceHighest);
    g.fillRect(0, 0, getWidth(), 1);

    g.setFont(HathorLookAndFeel::fontMedium(HathorLookAndFeel::Typography::labelMd));

    // --- Error indicator ---
    {
        const juce::Colour dotCol = errorCount_ > 0 ? palette.error : palette.textMuted;
        g.setColour(dotCol);
        g.fillEllipse(static_cast<float>(errorBox_.bounds.getX() + 4),
                      static_cast<float>(errorBox_.bounds.getY() + (errorBox_.bounds.getHeight() - 8) / 2),
                      8.0f, 8.0f);
        g.setColour(palette.textPrimary);
        juce::String text = juce::String(errorCount_) + " error" + (errorCount_ != 1 ? "s" : "");
        g.drawText(text,
                   juce::Rectangle<int>(errorBox_.bounds.getX() + 16, errorBox_.bounds.getY(),
                                        errorBox_.bounds.getWidth() - 16, errorBox_.bounds.getHeight()),
                   juce::Justification::centredLeft, false);
    }

    // --- Warning indicator ---
    {
        const juce::Colour dotCol = warningCount_ > 0 ? palette.warning : palette.textMuted;
        g.setColour(dotCol);
        g.fillEllipse(static_cast<float>(warningBox_.bounds.getX() + 4),
                      static_cast<float>(warningBox_.bounds.getY() + (warningBox_.bounds.getHeight() - 8) / 2),
                      8.0f, 8.0f);
        g.setColour(palette.textPrimary);
        juce::String text = juce::String(warningCount_) + " warning" + (warningCount_ != 1 ? "s" : "");
        g.drawText(text,
                   juce::Rectangle<int>(warningBox_.bounds.getX() + 16, warningBox_.bounds.getY(),
                                        warningBox_.bounds.getWidth() - 16, warningBox_.bounds.getHeight()),
                   juce::Justification::centredLeft, false);
    }

    // --- Transport indicator ---
    {
        const juce::Colour col = transportRunning_ ? palette.accent : palette.textMuted;
        g.setColour(col);
        // Play triangle or stop squares
        if (transportRunning_)
        {
            // Pause/Stop icon (two bars)
            constexpr int barW = 3, barH = 10;
            g.fillRect(transportBox_.bounds.getX() + 8, transportBox_.bounds.getY() + (transportBox_.bounds.getHeight() - barH) / 2,
                       barW, barH);
            g.fillRect(transportBox_.bounds.getX() + 14, transportBox_.bounds.getY() + (transportBox_.bounds.getHeight() - barH) / 2,
                       barW, barH);
        }
        else
        {
            // Stop icon (square)
            g.fillRect(transportBox_.bounds.getX() + 8, transportBox_.bounds.getY() + (transportBox_.bounds.getHeight() - 12) / 2,
                       12, 12);
        }
        g.setColour(palette.textPrimary);
        juce::String text = juce::String(static_cast<int>(std::round(bpm_))) + " BPM";
        g.drawText(text,
                   juce::Rectangle<int>(transportBox_.bounds.getX() + 24, transportBox_.bounds.getY(),
                                        transportBox_.bounds.getWidth() - 24, transportBox_.bounds.getHeight()),
                   juce::Justification::centredLeft, false);
    }

    // --- Worker indicator ---
    {
        const juce::Colour col = workerAlive_ ? juce::Colours::green : palette.error;
        g.setColour(col);
        g.fillEllipse(static_cast<float>(workerBox_.bounds.getX() + 4),
                      static_cast<float>(workerBox_.bounds.getY() + (workerBox_.bounds.getHeight() - 8) / 2),
                      8.0f, 8.0f);
        g.setColour(palette.textPrimary);
        g.drawText(workerAlive_ ? "Audio worker: OK" : "Audio worker: DOWN",
                   juce::Rectangle<int>(workerBox_.bounds.getX() + 16, workerBox_.bounds.getY(),
                                        workerBox_.bounds.getWidth() - 16, workerBox_.bounds.getHeight()),
                   juce::Justification::centredLeft, false);
    }

    // --- LSP indicator ---
    {
        const juce::Colour col = lspConnected_ ? palette.accent : palette.textMuted;
        g.setColour(col);
        g.fillEllipse(static_cast<float>(lspBox_.bounds.getX() + 4),
                      static_cast<float>(lspBox_.bounds.getY() + (lspBox_.bounds.getHeight() - 8) / 2),
                      8.0f, 8.0f);
        g.setColour(palette.textPrimary);
        g.drawText(lspConnected_ ? "LSP: connected" : "LSP: disconnected",
                   juce::Rectangle<int>(lspBox_.bounds.getX() + 16, lspBox_.bounds.getY(),
                                        lspBox_.bounds.getWidth() - 16, lspBox_.bounds.getHeight()),
                   juce::Justification::centredLeft, false);
    }

    // --- Git indicator ---
    {
        const bool hasChanges = (gitStagedCount_ + gitUnstagedCount_) > 0;
        const juce::Colour dotCol = hasChanges ? palette.accent : palette.textMuted;
        g.setColour(dotCol);
        // Draw a git "branch" icon (filled circle)
        g.fillEllipse(static_cast<float>(gitBox_.bounds.getX() + 4),
                      static_cast<float>(gitBox_.bounds.getY() + (gitBox_.bounds.getHeight() - 8) / 2),
                      8.0f, 8.0f);

        g.setColour(palette.textPrimary);
        juce::String text = gitBranch_.empty()
            ? "Git: n/a"
            : ("Git: " + juce::String(gitBranch_));
        if (gitStagedCount_ > 0)
            text << " +S" << gitStagedCount_;
        if (gitUnstagedCount_ > 0)
            text << " ~" << gitUnstagedCount_;
        g.drawText(text,
                   juce::Rectangle<int>(gitBox_.bounds.getX() + 16, gitBox_.bounds.getY(),
                                        gitBox_.bounds.getWidth() - 16, gitBox_.bounds.getHeight()),
                   juce::Justification::centredLeft, false);
    }

    // --- Master gain ---
    {
        g.setColour(palette.textPrimary);
        juce::String text = "Gain: " + juce::String(masterGain_, 2);
        g.drawText(text,
                   juce::Rectangle<int>(gainBox_.bounds.getX(), gainBox_.bounds.getY(),
                                        gainBox_.bounds.getWidth(), gainBox_.bounds.getHeight()),
                   juce::Justification::centredRight, false);
    }

    // Info count (if non-zero, shown as subtle text at far right)
    if (infoCount_ > 0)
    {
        g.setColour(palette.textMuted);
        g.setFont(HathorLookAndFeel::fontRegular(11.0f));
        g.drawText(juce::String(infoCount_) + " info",
                   juce::Rectangle<int>(std::max(0, gainBox_.bounds.getX() - 80), gainBox_.bounds.getY(),
                                        76, gainBox_.bounds.getHeight()),
                   juce::Justification::centredRight, false);
    }
}

void StatusRibbon::mouseDown(const juce::MouseEvent& e)
{
    const juce::Point<int> pos = e.position.toInt();

    if (errorBox_.bounds.contains(pos) && onErrorsClicked)
    {
        onErrorsClicked();
        return;
    }
    if (transportBox_.bounds.contains(pos) && onTransportClicked)
    {
        onTransportClicked();
        return;
    }
    if (workerBox_.bounds.contains(pos) && onWorkerClicked)
    {
        onWorkerClicked();
        return;
    }
    if (lspBox_.bounds.contains(pos) && onLspClicked)
    {
        onLspClicked();
        return;
    }
    if (gitBox_.bounds.contains(pos) && onGitClicked)
    {
        onGitClicked();
        return;
    }
}

} // namespace hathor::ui
