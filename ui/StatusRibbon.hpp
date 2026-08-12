// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * StatusRibbon.hpp — L-3: compact bottom status ribbon for global IDE state.
 *
 * A 28 px tall horizontal strip docked at the very bottom of the MainWindow.
 * Exposes high-value, at-a-glance IDE state without becoming a second control
 * panel. Icon-based indicators only.
 *
 * Indicators (left → right):
 *   - Problems: error count (red dot + number)
 *   - Warnings count (yellow dot + number)
 *   - Transport: play/stop icon + BPM
 *   - Worker: alive/dead (green/red dot)
 *   - LSP: connected/disconnected (blue/grey dot)
 *   - Master gain level (text)
 *
 * Requirement references: L-3 §5
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>

#include "HathorLookAndFeel.hpp"
#include "control/DiagnosticRegistry.hpp"

namespace hathor::ui {

class StatusRibbon : public juce::Component
{
public:
    static constexpr int kRibbonHeight = 28;

    explicit StatusRibbon(hathor::control::DiagnosticRegistry* registry = nullptr);
    ~StatusRibbon() override = default;

    // -----------------------------------------------------------------------
    // State mutators — called by EditorArea / MainWindow / UITimer
    // -----------------------------------------------------------------------
    void setErrorCount(int n) noexcept;
    void setWarningCount(int n) noexcept;
    void setInfoCount(int n) noexcept;

    void setTransportRunning(bool running) noexcept;
    void setBpm(double bpm) noexcept;
    void setWorkerAlive(bool alive) noexcept;
    void setLspConnected(bool connected) noexcept;
    void setMasterGain(float gain) noexcept;

    // -----------------------------------------------------------------------
    // Registry binding — auto-updates error/warning counts when diagnostics change
    // -----------------------------------------------------------------------
    void setRegistry(hathor::control::DiagnosticRegistry* registry) noexcept;

    // -----------------------------------------------------------------------
    // Click callbacks — clicking an indicator fires these
    // -----------------------------------------------------------------------
    std::function<void()> onErrorsClicked;     ///< open Problems panel
    std::function<void()> onTransportClicked;  ///< toggle play/stop
    std::function<void()> onWorkerClicked;    ///< focus worker status
    std::function<void()> onLspClicked;       ///< focus LSP status

    // -----------------------------------------------------------------------
    // juce::Component
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    // -----------------------------------------------------------------------
    // Layout helpers
    // -----------------------------------------------------------------------
    struct IndicatorBox
    {
        juce::Rectangle<int> bounds;
        bool active = false;
    };

    void layoutIndicators();

    // -----------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------
    hathor::control::DiagnosticRegistry* registry_ = nullptr;

    int   errorCount_   = 0;
    int   warningCount_ = 0;
    int   infoCount_    = 0;
    bool  transportRunning_ = false;
    double bpm_ = 120.0;
    bool  workerAlive_ = true;
    bool  lspConnected_ = false;
    float masterGain_ = 1.0f;

    // Cached indicator rectangles for click hit-testing
    IndicatorBox errorBox_;
    IndicatorBox warningBox_;
    IndicatorBox transportBox_;
    IndicatorBox workerBox_;
    IndicatorBox lspBox_;
    IndicatorBox gainBox_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(StatusRibbon)
};

} // namespace hathor::ui
