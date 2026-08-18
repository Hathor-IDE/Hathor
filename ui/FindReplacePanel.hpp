// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * FindReplacePanel.hpp — JUCE find/replace UI panel.
 *
 * A dockable panel at the bottom of the editor that wraps FindReplaceModel
 * and drives the active HathorTab's editor component.
 *
 * Requirement references: L-1 §4 (find/replace within current document)
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include "FindReplaceModel.hpp"

namespace hathor::ui {

class FindReplacePanel : public juce::Component
{
public:
    static constexpr int kPanelHeight = 44;

    FindReplacePanel();
    ~FindReplacePanel() override;

    // Non-copyable
    FindReplacePanel(FindReplacePanel&&) = delete;
    FindReplacePanel& operator=(FindReplacePanel&&) = delete;

    /**
     * Connect this panel to an editor component for live search highlighting.
     * The panel does NOT own the editor.
     */
    void setTargetEditor(juce::CodeEditorComponent* editor,
                         juce::CodeDocument* document);

    /** Toggle panel visibility. */
    void setVisible(bool visible) override;

    /// Access the underlying search engine (for replace operations driven by EditorArea).
    FindReplaceModel& model() noexcept { return model_; }
    const FindReplaceModel& model() const noexcept { return model_; }

    // juce::Component
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Callbacks — installed by the editor area / command palette
    std::function<void()> onFindNext;
    std::function<void()> onFindPrev;
    std::function<void()> onReplace;
    std::function<void()> onReplaceAll;
    std::function<void()> onClosePanel;

private:
    // UI controls
    std::unique_ptr<juce::TextEditor> findField_;
    std::unique_ptr<juce::TextEditor> replaceField_;
    std::unique_ptr<juce::TextButton> findNextBtn_;
    std::unique_ptr<juce::TextButton> findPrevBtn_;
    std::unique_ptr<juce::TextButton> replaceBtn_;
    std::unique_ptr<juce::TextButton> replaceAllBtn_;
    std::unique_ptr<juce::TextButton> closeBtn_;
    std::unique_ptr<juce::ToggleButton> regexCheckbox_;
    std::unique_ptr<juce::ToggleButton> caseSensitiveCheckbox_;
    std::unique_ptr<juce::ToggleButton> wholeWordCheckbox_;
    std::unique_ptr<juce::ToggleButton> wrapAroundCheckbox_;

    // The JUCE-free search engine
    FindReplaceModel model_;

    // Weak references to the target editor
    juce::CodeEditorComponent* editor_{ nullptr };
    juce::CodeDocument* document_{ nullptr };

    void updateModelFlags();
    void syncUIFromSearch(const juce::String& text);
    void syncUIFromReplace(const juce::String& text);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FindReplacePanel)
};

} // namespace hathor::ui
