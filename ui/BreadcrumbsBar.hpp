// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * BreadcrumbsBar.hpp — current file context + quick navigation bar.
 *
 * Shows the current file path as a trail of clickable segments, plus
 * quick-access buttons for the command palette, find, and split.
 *
 * Requirement references: L-1 §5 (breadcrumbs / current-file context)
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <string>
#include <vector>

namespace hathor::ui {

class BreadcrumbsBar : public juce::Component
{
public:
    static constexpr int kBarHeight = 28;

    BreadcrumbsBar();
    ~BreadcrumbsBar() override;

    BreadcrumbsBar(BreadcrumbsBar&&) = delete;
    BreadcrumbsBar& operator=(BreadcrumbsBar&&) = delete;

    /**
     * Set the current file path to display as breadcrumbs.
     * @param fullPath  The full file path.
     * @param editorName  Optional editor type label (e.g. "ChucK", "Mini-Notation").
     */
    void setCurrentFile(const juce::File& fullPath, juce::String editorName = {});

    /** Clear the current breadcrumb (no file open). */
    void clear();

    // Callbacks installed by MainWindow/EditorArea
    std::function<void()> onCommandPaletteClicked;
    std::function<void()> onFindClicked;
    std::function<void()> onSplitClicked;
    std::function<void(const juce::File&)> onBreadcrumbClicked;  // argument: path segment file

    // juce::Component
    void resized() override;
    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    struct Crumb
    {
        juce::Rectangle<int> bounds;
        juce::String label;
        juce::File file;  // the file this crumb represents (or a directory)
    };

    std::vector<Crumb> crumbs_;
    juce::String editorName_;
    juce::Rectangle<int> commandPaletteBtn_;
    juce::Rectangle<int> findBtn_;
    juce::Rectangle<int> splitBtn_;

    void buildCrumbs(const juce::File& file);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(BreadcrumbsBar)
};

} // namespace hathor::ui
