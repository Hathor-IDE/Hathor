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
    enum class Orientation { Horizontal, Vertical };

    using DragCallback = std::function<void(int delta)>;

    explicit SplitterBar(DragCallback onDrag_,
                         Orientation orient = Orientation::Horizontal)
        : juce::Component({"splitter"}), orient_(orient), onDrag_(std::move(onDrag_))
    {
        setInterceptsMouseClicks(true, false);
        setMouseCursor(orient == Orientation::Horizontal
                           ? juce::MouseCursor::LeftRightResizeCursor
                           : juce::MouseCursor::UpDownResizeCursor);
        setTitle("Splitter");
    }

    ~SplitterBar() override = default;

    void paint(juce::Graphics& g) override
    {
        const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
        g.fillAll(isMouseOver() ? palette.accent.withAlpha(0.4f)
                                : palette.surfaceContainer.withAlpha(0.3f));
    }

    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        mouseDownPos_ = (orient_ == Orientation::Horizontal) ? e.getScreenX()
                                                            : e.getScreenY();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        const int pos = (orient_ == Orientation::Horizontal) ? e.getScreenX()
                                                            : e.getScreenY();
        const int delta = pos - mouseDownPos_;
        mouseDownPos_ = pos;
        if (delta != 0 && onDrag_)
            onDrag_(delta);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (onDragFinished_)
            onDragFinished_();
    }

    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        if (onDoubleClick_)
            onDoubleClick_();
    }

    std::function<void()> onDoubleClick_;
    /// Fired on mouse-up after a drag so the host can persist layout.
    std::function<void()> onDragFinished_;

private:
    int mouseDownPos_ = 0;
    Orientation orient_;
    DragCallback onDrag_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SplitterBar)
};

} // namespace hathor::ui
