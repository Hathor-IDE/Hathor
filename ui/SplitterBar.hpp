// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SplitterBar — a thin draggable divider between two panels.
 *
 * Handles mouse drag to resize a pair of adjacent components.  The bar is
 * transparent to clicks when not actively dragging (cursor changes on hover).
 *
 * Usage:
 *   - Create with a drag callback that notifies the layout host (MainWindow)
 *     which then repositions all panels via LayoutParams.
 */

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

#include "HathorLookAndFeel.hpp"

namespace hathor::ui {

class SplitterBar : public juce::Component
{
public:
    using DragCallback = std::function<void(int deltaX)>;

    explicit SplitterBar(DragCallback onDrag_)
        : juce::Component({"splitter"}), onDrag_(std::move(onDrag_))
    {
        setInterceptsMouseClicks(true, false);
    }

    ~SplitterBar() override = default;

    void paint(juce::Graphics& g) override
    {
        const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
        g.fillAll(palette.surfaceContainer.withAlpha(0.3f));
    }

    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseLeave(const juce::MouseEvent&) override { repaint(); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        mouseDownX_ = e.getScreenX();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const int delta = e.getScreenX() - mouseDownX_;
        mouseDownX_ = e.getScreenX();
        if (onDrag_)
            onDrag_(delta);
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (onDoubleClick_)
            onDoubleClick_();
    }

    std::function<void()> onDoubleClick_;

private:
    int mouseDownX_ = 0;
    DragCallback onDrag_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SplitterBar)
};

} // namespace hathor::ui
