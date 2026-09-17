// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SymbolSearchPanel.hpp — JUCE symbol search/results panel (Ctrl/Cmd+T or
 * accessible via CommandPalette → "Go: Show Symbols").
 *
 * Wraps SymbolSearchModel and provides a searchable listbox view of symbols.
 * Results come from metadata (Strudel functions/samples/scales) and optionally
 * from the LSP workspace/symbol response for the current document's symbols.
 *
 * Requirement references: L-2 §3
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

#include "SymbolSearchModel.hpp"

namespace hathor::ui {

class SymbolSearchPanel : public juce::Component,
                          public juce::TextEditor::Listener,
                          public juce::ListBoxModel
{
public:
    static constexpr int kPanelHeight = 300;

    explicit SymbolSearchPanel(SymbolSearchModel* model = nullptr);
    ~SymbolSearchPanel() override;

    // Non-copyable
    SymbolSearchPanel(SymbolSearchPanel&&) = delete;
    SymbolSearchPanel& operator=(SymbolSearchPanel&&) = delete;

    /** Set/replace the model. Non-owning. */
    void setModel(SymbolSearchModel* model) noexcept { model_ = model; }

    /** Trigger a search with the given query. */
    void setQuery(const juce::String& query);

    /** Workspace root for file-based symbol indexing. */
    void setWorkspaceRoot(const std::filesystem::path& root)
    {
        workspaceRoot_ = root;
    }

    /** Fired on every query change so the owner can feed LSP results back
        into the model (the panel always searches metadata + workspace
        files itself). */
    std::function<void(const std::string& query)> onQueryChanged;

    /** Re-read the model after the owner updates it (e.g. LSP results). */
    void refreshResults() { reloadResults(); }

    /** Toggle panel visibility. */
    void setVisible(bool visible) override;

    /** Clear results and hide. */
    void clear();

    // juce::Component
    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

    // Callbacks — installed by EditorArea
    std::function<void(const SymbolSearchResult&)> onSymbolSelected;
    std::function<void()> onClosePanel;

private:
    // TextEditor::Listener
    void textEditorTextChanged(juce::TextEditor& editor) override;
    void textEditorEscapeKeyPressed(juce::TextEditor& editor) override;
    void textEditorReturnKeyPressed(juce::TextEditor& editor) override;
    void textEditorFocusLost(juce::TextEditor&) override {}  // no-op — search runs on text change, not focus

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int width, int height,
                          bool isSelected) override;
    void selectedRowsChanged(int lastSelectedRow) override;

    /** Flatten model results into a display list. */
    void reloadResults();

    friend class SymbolSearchDoubleClickHandler;

    SymbolSearchModel* model_{nullptr};
    std::filesystem::path workspaceRoot_;
    std::vector<SymbolSearchResult> displayResults_;
    int selectedIndex_ = 0;

    std::unique_ptr<juce::TextEditor> searchField_;
    std::unique_ptr<juce::Label> hintLabel_;
    std::unique_ptr<juce::ListBox> listBox_;
    std::unique_ptr<juce::TextButton> closeBtn_;
    std::unique_ptr<juce::KeyListener> keyForwarder_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SymbolSearchPanel)
};

} // namespace hathor::ui
