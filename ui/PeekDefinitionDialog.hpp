// Copyright (C) 2026 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PeekDefinitionDialog.hpp — modal "Peek Definition" surface.
 *
 * Shows the LSP textDocument/definition result for the symbol at the cursor
 * in a small modal dialog (mirroring the BakeTargetDialog convention) WITHOUT
 * navigating away. The user can inspect the definition and optionally navigate
 * to it via the "Go to Definition" button, which reuses the editor's existing
 * file-opening infrastructure (EditorArea::openFile + caret move).
 *
 * Multiple definitions are presented as a ListBox (mirroring QuickOpenDialog)
 * rather than silently picking one.
 *
 * Requirement references: L-2 (Peek Definition, AI-4 LSP wiring).
 */

#include <juce_gui_extra/juce_gui_extra.h>

#include "LspProtocol.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hathor::ui {

/**
 * A single definition hit, augmented with the source context needed for
 * display (file name, line number, and the surrounding source text).
 */
struct PeekDefinitionEntry
{
    lsp::Location  location;      ///< raw LSP location (uri + range) for navigation
    juce::String   filePath;     ///< file path without the "file://" scheme
    juce::String   fileLabel;    ///< basename of the file, for display
    int            line1          = 0;   ///< 1-based start line number
    juce::String   sourceText;   ///< source context around the definition
};

/**
 * PeekDefinitionDialog
 *
 * Layout (when >1 definition):
 *   ┌──────────────────┬──────────────────────┐
 *   │ ListBox (paths)  │ source view          │
 *   ├─────────────────────────────────────────┤  ← location label
 *   │  Go to Definition   Close                │
 *   └─────────────────────────────────────────┘
 *
 * Layout (single definition): the ListBox is omitted and the source view
 * spans the full width.
 */
class PeekDefinitionDialog : public juce::Component,
                             private juce::ListBoxModel,
                             public juce::Button::Listener
{
public:
    using NavigateCallback = std::function<void(const lsp::Location&)>;

    PeekDefinitionDialog(std::vector<PeekDefinitionEntry> entries,
                         NavigateCallback onNavigate);

    ~PeekDefinitionDialog() override;

    static constexpr int kWidth  = 660;
    static constexpr int kHeight = 440;

    // juce::Component
    void paint(juce::Graphics& g) override;
    void resized() override;
    void keyPressed(const juce::KeyPress& key) override;

private:
    // Button::Listener
    void buttonClicked(juce::Button*) override;

    // ListBoxModel
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g,
                          int width, int height, bool isSelected) override;
    void selectedRowsChanged(int lastSelectedRow) override;

    /// Refresh the source view + location label to the currently-selected entry.
    void updateSelection();

    std::vector<PeekDefinitionEntry> entries_;
    int selectedEntry_ = 0;

    std::unique_ptr<juce::Label>       titleLabel_;
    std::unique_ptr<juce::Label>       locationLabel_;
    std::unique_ptr<juce::ListBox>     listBox_;
    std::unique_ptr<juce::TextEditor>  sourceView_;
    std::unique_ptr<juce::TextButton>  goToBtn_;
    std::unique_ptr<juce::TextButton>  closeBtn_;

    NavigateCallback onNavigate_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PeekDefinitionDialog)
};

/**
 * Launch the Peek Definition dialog modally, centred on @p parent.
 * The dialog is dismissed when the user navigates or closes it; navigation
 * reuses the editor's existing file-opening infrastructure.
 */
void showPeekDefinition(juce::Component* parent,
                        std::vector<PeekDefinitionEntry> entries,
                        PeekDefinitionDialog::NavigateCallback onNavigate);

} // namespace hathor::ui
