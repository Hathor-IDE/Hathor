// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * LspHoverHandler.hpp — JUCE component for displaying LSP hover tooltips.
 *
 * Shows hover documentation as a small floating tooltip near the cursor.
 * The tooltip fades out after a configurable duration.
 *
 * Requirement references: AI-4
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "LspProtocol.hpp"

#include <functional>
#include <string>

namespace hathor::ui {

class LspHoverHandler : public juce::Component,
                        private juce::Timer
{
public:
    using DismissCallback = std::function<void()>;

    explicit LspHoverHandler(DismissCallback onDismiss);

    ~LspHoverHandler() override = default;

    // -----------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------

    /**
     * Show hover content at the given editor-local position.
     * @param content  The hover contents (may have multiple lines).
     * @param anchor   Bottom-left corner of the tooltip (editor-local coords).
     */
    void showHover(const lsp::Hover& content,
                   const juce::Point<int>& anchor);

    /** Dismiss the tooltip immediately. */
    void dismiss();

    /** True if currently showing. */
    bool isShowing() const noexcept { return visible_; }

private:
    void paint(juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;
    void mouseDown(const juce::MouseEvent& e) override;

    static constexpr int kAutoDismissMs = 8000;
    static constexpr int kMaxWidth = 400;
    static constexpr int kPadding = 8;

    std::string                         text_;
    juce::Point<int>                    anchor_;
    bool                                visible_ = false;
    DismissCallback                     onDismiss_;
    int                                 displayWidth_ = 0;
    int                                 displayHeight_ = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LspHoverHandler)
};

} // namespace hathor::ui
