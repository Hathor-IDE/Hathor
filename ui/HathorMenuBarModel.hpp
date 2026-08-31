// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * HathorMenuBarModel — native macOS menu bar.
 *
 * Wires the standard macOS menu bar (File / Edit / View / Transport / Help)
 * to the existing ActionRegistry actions.  No duplicated handlers — every
 * menu item dispatches through editorArea_->actionRegistry() or calls a
 * MainWindow method directly for items not yet in the registry.
 *
 * Requirements: S1 (§1.6), Wave 2.3.
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "MainWindow.hpp"
#include "ActionRegistry.hpp"

namespace hathor::ui {

class HathorMenuBarModel : public juce::MenuBarModel
{
public:
    HathorMenuBarModel(MainWindow& owner_) noexcept
        : owner_(owner_) {}

    ~HathorMenuBarModel() override = default;

    // --- juce::MenuBarModel overrides -------------------------------------

    juce::StringArray getMenuBarNames() override
    {
        return {"File", "Edit", "View", "Transport", "Help"};
    }

    juce::PopupMenu::Options getMenuComponents() override
    {
        return {};
    }

    void menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/) override
    {
        // Dispatch through the action registry when possible; fall through to
        // MainWindow methods for items that need window-level handling.
        if (auto* reg = actionRegistry_)
        {
            if (reg->dispatch(actionIdForMenuItem_[menuItemID]))
                return;
        }

        switch (menuItemID)
        {
            case MenuID::NewFile:        owner_.onNewFile();        break;
            case MenuID::OpenFile:       owner_.onOpenFile();       break;
            case MenuID::OpenFolder:     owner_.openFolderChooser(); break;
            case MenuID::Quit:           juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
            case MenuID::CloseTab:       if (editorArea_) editorArea_->closeActiveTab(); break;
            default:                     break;
        }
    }

    juce::String getMenuLabel(const int itemId) const
    {
        return menuLabels_[itemId];
    }

    // --- Menu item IDs (stable, > 0 for JUCE) -----------------------------

    struct MenuID
    {
        static constexpr int NewFile        = 1001;
        static constexpr int OpenFile       = 1002;
        static constexpr int OpenFolder     = 1003;
        static constexpr int OpenRecentBase = 1004;
        static constexpr int Save           = 1005;
        static constexpr int SaveAs         = 1006;
        static constexpr int CloseTab       = 1007;
        static constexpr int Quit           = 1008;

        static constexpr int Undo           = 2001;
        static constexpr int Redo           = 2002;
        static constexpr int Cut            = 2003;
        static constexpr int Copy           = 2004;
        static constexpr int Paste          = 2005;
        static constexpr int Delete         = 2006;

        static constexpr int ToggleExplorer     = 3001;
        static constexpr int ToggleChat         = 3002;
        static constexpr int ToggleVisualizer   = 3003;
        static constexpr int ToggleTerminal     = 3004;
        static constexpr int ToggleProblems     = 3005;
        static constexpr int ToggleSourceCtrl   = 3006;
        static constexpr int ToggleDebug        = 3007;

        static constexpr int TransportPlay      = 4001;
        static constexpr int TransportStop      = 4002;
        static constexpr int TransportRecord    = 4003;

        static constexpr int HelpDocs           = 5001;
        static constexpr int HelpShortcuts      = 5002;
        static constexpr int HelpAbout          = 5003;
    };

    // --- Builder ----------------------------------------------------------

    void build();

private:
    MainWindow& owner_;
    hathor::ui::EditorArea* editorArea_ = nullptr;
    hathor::ui::ActionRegistry* actionRegistry_ = nullptr;

    juce::String map_[5004];
    std::unordered_map<int, std::string> actionIdForMenuItem_;
    std::unordered_map<int, juce::String> menuLabels_;

    void setMenuItem(int id, const juce::String& label, std::string actionId = {})
    {
        menuLabels_[id] = label;
        map_[id] = label;
        if (!actionId.empty())
            actionIdForMenuItem_[id] = std::move(actionId);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorMenuBarModel)
};

} // namespace hathor::ui
