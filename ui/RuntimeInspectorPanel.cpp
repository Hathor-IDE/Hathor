// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * RuntimeInspectorPanel.cpp — L-6: Hathor runtime-inspection UI implementation.
 *
 * Requirement references: L-6 §Hathor Runtime Inspection, L-6 §Workspace Integration
 */

#include "RuntimeInspectorPanel.hpp"

#include <algorithm>
#include <cmath>

namespace hathor::ui {

namespace {

// ---------------------------------------------------------------------------
// Scrollable content component that paints the row list produced by the panel
// ---------------------------------------------------------------------------
class InspectorContent : public juce::Component
{
public:
    std::function<void(juce::Graphics&)> onPaint;

    void paint(juce::Graphics& g) override
    {
        if (onPaint)
            onPaint(g);
    }
};

/// Human-readable worker status text + colour (L-6: liveness/restart/crash).
juce::Colour workerStatusColour(const std::string& status, const Palette& pal)
{
    if (status == "healthy")         return pal.accent;
    if (status == "dead")            return pal.error;
    if (status == "start_error")     return pal.error;
    if (status == "shutting_down")   return pal.warning;
    if (status == "stale_generation") return pal.warning;
    return pal.textMuted;
}

juce::String workerStatusText(const std::string& status)
{
    if (status == "healthy")         return "Healthy";
    if (status == "dead")            return "Dead — crashed/stopped";
    if (status == "shutting_down")   return "Shutting down";
    if (status == "stale_generation") return "Stale generation — restarted";
    if (status == "not_started")     return "Not started";
    if (status == "start_error")     return "Start error";
    if (status == "unknown")         return "Unknown";
    return status.empty() ? "—" : juce::String(status);
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

RuntimeInspectorPanel::RuntimeInspectorPanel(AudioEngineFacade& audio,
                                             hathor::control::DiagnosticRegistry* registry)
{
    model_.setSources(&audio, registry);

    titleLabel_ = std::make_unique<juce::Label>();
    titleLabel_->setText("Runtime Inspector", juce::dontSendNotification);
    titleLabel_->setFont(HathorLookAndFeel::fontSemiBold(12.0f));
    addAndMakeVisible(*titleLabel_);

    problemsBtn_ = std::make_unique<juce::TextButton>("Open Problems");
    problemsBtn_->onClick = [this]() {
        if (onOpenProblems)
            onOpenProblems();
    };
    addAndMakeVisible(*problemsBtn_);

    refreshBtn_ = std::make_unique<juce::TextButton>("Refresh");
    refreshBtn_->onClick = [this]() {
        model_.requestVmCapture();
        model_.refreshQuick();
        applySnapshot();
    };
    addAndMakeVisible(*refreshBtn_);

    closeBtn_ = std::make_unique<juce::TextButton>("\u00D7");
    closeBtn_->onClick = [this]() {
        if (onClosePanel)
            onClosePanel();
    };
    addAndMakeVisible(*closeBtn_);

    viewport_ = std::make_unique<juce::Viewport>();
    viewport_->setScrollBarsShown(true, false);
    addAndMakeVisible(*viewport_);

    auto content = std::make_unique<InspectorContent>();
    content->onPaint = [this](juce::Graphics& g) {
        const auto& pal = HathorLookAndFeel::fromComponent(*this).getPalette();
        g.fillAll(pal.surfaceLow);

        const int width = content_->getWidth();
        int y = kMargin;

        for (const auto& row : rows_)
        {
            if (row.kind == RuntimeInspectorPanel::Row::Kind::Section)
            {
                g.setColour(pal.textMuted);
                g.setFont(HathorLookAndFeel::fontMedium(10.0f));
                g.drawText(juce::String(row.label).toUpperCase(),
                           juce::Rectangle<int>(kMargin, y, width - 2 * kMargin, kSectionHeight),
                           juce::Justification::left, false);
                // 1 px accent underline
                g.setColour(pal.accent.withAlpha(0.35f));
                g.fillRect(juce::Rectangle<int>(kMargin, y + kSectionHeight - 2, width - 2 * kMargin, 1));
                y += kSectionHeight;
                continue;
            }

            const int labelW = std::max(70, width / 4);
            g.setColour(pal.textSecondary);
            g.setFont(HathorLookAndFeel::fontRegular(11.0f));
            g.drawText(juce::String(row.label),
                       juce::Rectangle<int>(kMargin, y, labelW, kRowHeight),
                       juce::Justification::left, false);

            g.setColour(row.colour);
            g.drawText(juce::String(row.value),
                       juce::Rectangle<int>(kMargin + labelW, y,
                                            width - 2 * kMargin - labelW, kRowHeight),
                       juce::Justification::left, false);
            y += kRowHeight;
        }
    };
    viewport_->setViewedComponent(content.get(), false);
    content_.reset(content.release());
}

RuntimeInspectorPanel::~RuntimeInspectorPanel()
{
    stopTimer();
    model_.shutdown();
}

// ---------------------------------------------------------------------------
// juce::Component
// ---------------------------------------------------------------------------

void RuntimeInspectorPanel::resized()
{
    auto b = getLocalBounds();

    // Header row
    auto header = b.removeFromTop(kHeaderHeight);
    closeBtn_->setBounds(header.removeFromRight(24));
    refreshBtn_->setBounds(header.removeFromRight(64).reduced(2, 3));
    problemsBtn_->setBounds(header.removeFromRight(110).reduced(2, 3));
    titleLabel_->setBounds(header.reduced(kMargin, 5));

    viewport_->setBounds(b);
    applySnapshot();
}

void RuntimeInspectorPanel::paint(juce::Graphics& g)
{
    g.fillAll(HathorLookAndFeel::fromComponent(*this).getPalette().surface);
}

void RuntimeInspectorPanel::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
    if (visible)
    {
        refreshNow();
        startTimerHz(kPollIntervalHz);
    }
    else
    {
        stopTimer();
    }
}

// ---------------------------------------------------------------------------
// Refresh
// ---------------------------------------------------------------------------

void RuntimeInspectorPanel::refreshNow()
{
    model_.refreshQuick();
    model_.requestVmCapture();
    applySnapshot();
}

void RuntimeInspectorPanel::timerCallback()
{
    const auto nowMs = static_cast<juce::int64>(juce::Time::getMillisecondCounter());

    model_.refreshQuick();

    // Kick the background VM capture on a slow cadence (control-plane IPC is
    // bounded but should not run on every 10 Hz tick).
    if (!model_.vmCaptureInFlight() && (nowMs - lastVmCaptureMs_ >= kVmCaptureIntervalMs))
    {
        model_.requestVmCapture();
        lastVmCaptureMs_ = nowMs;
    }

    applySnapshot();
}

void RuntimeInspectorPanel::applySnapshot()
{
    snap_ = model_.snapshot();
    rows_ = buildRows();

    if (content_ != nullptr)
    {
        int h = kMargin;
        for (const auto& row : rows_)
            h += (row.kind == Row::Kind::Section) ? kSectionHeight : kRowHeight;
        h += kMargin;
        content_->setSize(std::max(viewport_->getWidth(), 200), h);
        content_->repaint();
    }
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

std::vector<RuntimeInspectorPanel::Row> RuntimeInspectorPanel::buildRows() const
{
    const auto& pal = HathorLookAndFeel::globalPalette();
    std::vector<Row> rows;

    const auto section = [&rows](const juce::String& name)
    {
        Row r;
        r.kind = Row::Kind::Section;
        r.label = name.toStdString();
        rows.push_back(std::move(r));
    };
    const auto item = [&rows](const juce::String& label, const juce::String& value,
                              juce::Colour colour)
    {
        Row r;
        r.kind = Row::Kind::Item;
        r.label = label.toStdString();
        r.value = value.toStdString();
        r.colour = colour;
        rows.push_back(std::move(r));
    };

    // ------------------------------------------------------------------
    // Playback
    // ------------------------------------------------------------------
    section("Playback");
    const auto& a = snap_.audio;
    {
        juce::String transport = a.running ? "Playing" : "Stopped";
        const juce::String bpm = juce::String(a.bpm, 1) + " BPM";

        juce::String barBeat = "-";
        if (a.running && a.sampleRate > 0 && a.bpm > 0.0)
        {
            // Same cycle math as the audio callback (Req 9.4): total cycles
            // elapsed = (sampleClock * bpm) / (sampleRate * 60).
            const double totalCycles =
                (static_cast<double>(a.sampleClock) * a.bpm) /
                (static_cast<double>(a.sampleRate) * 60.0);
            const int bar = static_cast<int>(std::floor(totalCycles)) + 1;
            barBeat = "bar " + juce::String(bar) + " \u00B7 beat " +
                      juce::String(a.currentBeat) + "/4";
        }

        item("Transport", transport + " \u00B7 " + bpm, a.running ? pal.accent : pal.textMuted);
        item("Cycle", barBeat, pal.textSecondary);
        item("Clock", juce::String(a.sampleClock) + " samples", pal.textSecondary);
    }

    // ------------------------------------------------------------------
    // Active slots
    // ------------------------------------------------------------------
    {
        juce::String slots;
        for (const auto& slot : snap_.slots)
        {
            if (!slot.hasPattern && !slot.running)
                continue;
            if (!slots.isEmpty())
                slots += " \u00B7 ";
            slots += juce::String(slot.slotName);
            slots += slot.running ? " \u25CF" : " \u25CB";
        }
        item("Slots", slots.isEmpty() ? "none active" : slots,
             slots.isEmpty() ? pal.textMuted : pal.accent);
    }

    // ------------------------------------------------------------------
    // Voices
    // ------------------------------------------------------------------
    {
        const int n = static_cast<int>(snap_.voices.size());
        item("Voices", juce::String(n) + (n == 1 ? " active" : " active total"),
             n > 0 ? pal.textSecondary : pal.textMuted);

        // Detail line for the first few voices (slot · gain · pan · speed).
        juce::String detail;
        const int shown = std::min(n, 4);
        for (int i = 0; i < shown; ++i)
        {
            const auto& v = snap_.voices[static_cast<std::size_t>(i)];
            if (!detail.isEmpty())
                detail += "  ";
            detail += "slot " + juce::String(static_cast<int>(v.slotId)) +
                      " g=" + juce::String(v.gain, 2) +
                      " pan=" + juce::String(v.pan, 2) +
                      " spd=" + juce::String(v.speed, 2);
        }
        if (!detail.isEmpty())
            item("", detail, pal.textSecondary);
    }

    // ------------------------------------------------------------------
    // ChucK
    // ------------------------------------------------------------------
    section("ChucK");
    {
        const auto col = workerStatusColour(snap_.workerStatus, pal);
        juce::String worker = workerStatusText(snap_.workerStatus);
        if (snap_.workerGeneration > 0)
            worker += " \u00B7 gen " + juce::String(snap_.workerGeneration);
        worker += snap_.workerAlive ? " \u00B7 alive" : " \u00B7 not responding";
        item("Worker", worker, col);
    }
    {
        if (snap_.vmStates.empty())
        {
            item("VMs", "no live VMs", pal.textMuted);
        }
        else
        {
            for (std::size_t i = 0; i < snap_.vmStates.size(); ++i)
            {
                const auto& vm = snap_.vmStates[i];
                const int slot = snap_.vmSlotIndices[i];
                juce::String text = vm.state.empty() ? "unknown" : juce::String(vm.state);
                if (!vm.shredInfo.empty() && vm.shredInfo != vm.state)
                    text += " \u00B7 " + juce::String(vm.shredInfo);
                if (!vm.lastError.empty())
                    text += " \u00B7 " + juce::String(vm.lastError);
                item(slot >= 0 ? juce::String("tab ") + juce::String(slot)
                               : "worker",
                     text,
                     vm.state == "active" || vm.state == "live" ? pal.accent : pal.textSecondary);
            }
        }
    }

    // ------------------------------------------------------------------
    // Audio
    // ------------------------------------------------------------------
    section("Audio");
    {
        juce::String engine = juce::String(static_cast<double>(a.sampleRate) / 1000.0, 1) + " kHz";
        engine += " \u00B7 gain " + juce::String(a.masterGain, 2);
        engine += " \u00B7 EQ " + juce::String(a.eqPreset);
        engine += a.deviceOpen ? " \u00B7 device open" : " \u00B7 device closed";
        item("Engine", engine, a.deviceOpen ? pal.textSecondary : pal.warning);
        item("Renders", juce::String(a.activeRenders) + " in-flight", pal.textSecondary);
    }

    // ------------------------------------------------------------------
    // Diagnostics (L-3 link)
    // ------------------------------------------------------------------
    section("Diagnostics");
    {
        const bool hasErrors = (snap_.diagErrors > 0);
        item("Problems",
             juce::String(snap_.diagErrors) + " errors \u00B7 " +
             juce::String(snap_.diagWarnings) + " warnings \u00B7 " +
             juce::String(snap_.diagInfo) + " info",
             hasErrors ? pal.error : pal.textSecondary);
    }

    return rows;
}

} // namespace hathor::ui
