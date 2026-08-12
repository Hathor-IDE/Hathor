// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * CommandPalette.hpp — JUCE command palette overlay backed by ActionRegistry.
 *
 * Provides a searchable list of all registered actions, with keyboard-driven
 * filtering. Shows the action label, category, and key equivalent.
 *
 * Requirement references: L-1 §5 (command palette)
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

#include "ActionRegistry.hpp"

namespace hathor::ui {

class CommandPalette : public juce::Component
{
public:
    CommandPalette();
    ~CommandPalette() override;

    CommandPalette(CommandPalette&&) = delete;
    CommandPalette& operator=(CommandPalette&&) = delete;

    /**
     * Set the action registry to query.  Non-owning — the registry must outlive
     * this palette.
     */
    void setActionRegistry(ActionRegistry* reg) noexcept { registry_ = reg; }

    /** Show the palette overlay on top of the parent component. */
    void show(juce::Component* parent);

    /** Hide the palette. */
    void hide();

    /** True if currently visible. */
    bool isVisible() const noexcept { return juce::Component::isVisible(); }

    // juce::Component
    void resized() override;
    void paint(juce::Graphics& g) override;

    /** Filter actions by query string (called on keystroke). */
    void setFilter(const juce::String& query);

    /** Execute the currently highlighted action. Returns false if none selected. */
    bool executeSelected();

    /** Move selection up in the filtered list. */
    void selectUp();

    /** Move selection down in the filtered list. */
    void selectDown();

private:
    ActionRegistry* registry_{ nullptr };

    std::unique_ptr<juce::TextEditor> filterField_;
    std::vector<const EditorAction*> filteredActions_;
    int selectedIndex_{ 0 };

    std::unique_ptr<juce::ListBox> listBox_;
    std::unique_ptr<juce::Label> hintLabel_;

    /** Refresh the filtered action list from the registry. */
    void refreshList(const juce::String& query = {});

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CommandPalette)
};

} // namespace hathor::ui
