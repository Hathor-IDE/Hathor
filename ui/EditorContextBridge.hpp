// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * EditorContextBridge.hpp — JUCE-dependent implementation of
 * hathor::control::EditorContextProvider.
 *
 * Maintains a thread-safe snapshot of the current editor state that is
 * updated from the JUCE message thread whenever the active tab, cursor
 * position, or document content changes.  The snapshot is read by the
 * control layer (AI-8 AuthoringContext) from the MCP accept-loop worker
 * thread.
 *
 * Requirement references: AI-8 §2, §4
 */

#include "EditorContextProvider.hpp"

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <atomic>
#include <mutex>
#include <string>

namespace hathor::ui {

class EditorArea;

class EditorContextBridge : public hathor::control::EditorContextProvider
{
public:
    explicit EditorContextBridge(EditorArea& editorArea);
    ~EditorContextBridge() override = default;

    // Non-copyable
    EditorContextBridge(const EditorContextBridge&)            = delete;
    EditorContextBridge& operator=(const EditorContextBridge&) = delete;

    /**
     * Refresh the internal snapshot from the active editor tab.
     * Must be called on the JUCE message thread (e.g. when the active
     * tab changes, document content changes, or cursor position changes).
     */
    void refresh();

    /**
     * Refresh the snapshot from a specific tab (used when a tab is being
     * closed or deactivated).  Must be called on the JUCE message thread.
     */
    void refreshFromTab(class HathorTab* tab);

    /**
     * Thread-safe snapshot read.
     * Safe to call from any thread (including the MCP accept-loop worker).
     */
    hathor::control::EditorContextSnapshot snapshot() const override;

private:
    EditorArea& editorArea_;

    // The snapshot is updated on the JUCE message thread and read from
    // the MCP accept-loop worker thread.  Protected by snapshotMtx_.
    mutable std::mutex                            snapshotMtx_;
    hathor::control::EditorContextSnapshot         snapshot_;
};

} // namespace hathor::ui
