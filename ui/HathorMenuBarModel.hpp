// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * HathorMenuBarModel — native macOS menu bar.
 *
 * Wires the standard macOS menu bar (File / Edit / View / Transport / Help)
 * to the existing ActionRegistry actions.  No duplicated handlers — every
 * menu item dispatches through the action registry or a MainWindow method.
 *
 * Requirements: S1 (§1.6), Wave 2.3.
 */

#include <juce_gui_basics/juce_gui_basics.h>

namespace hathor::ui {

class ActionRegistry;

class HathorMenuBarModel : public juce::MenuBarModel
{
public:
    HathorMenuBarModel() = default;
    ~HathorMenuBarModel() override = default;

    void setActionRegistry(hathor::ui::ActionRegistry* reg) noexcept { actionRegistry_ = reg; }

    struct Callbacks
    {
        std::function<void()>      onNewFile;
        std::function<void()>      onOpenFile;
        std::function<void()>      onOpenFolder;
        std::function<void()>      onQuit;
    std::function<void()>      onToggleExplorer;
        std::function<void()>      onToggleChat;
        std::function<void()>      onToggleVisualizer;
        std::function<void()>      onToggleTerminal;
        std::function<void()>      onToggleProblems;
        std::function<void()>      onToggleSourceControl;
        std::function<void()>      onToggleDebug;
        std::function<void()>      onPlay;
        std::function<void()>      onStop;
        std::function<void()>      onRecord;
        std::function<void()>      onOpenSettings;
        std::function<void()>      onOpenShortcuts;
        std::function<void()>      onOpenDocs;
        std::function<void()>      onAbout;
        juce::StringArray          recentProjects;
        std::function<std::vector<std::string>()> getRecentProjects;
        std::function<void(int)>   onSelectRecent;   // index into recentProjects
    };

    void setCallbacks(Callbacks cb) noexcept { callbacks_ = std::move(cb); }

    // --- Menu item IDs ---

    static constexpr int NewFile         = 1001;
    static constexpr int OpenFile        = 1002;
    static constexpr int OpenFolder      = 1003;
    static constexpr int OpenRecentBase  = 1004;
    static constexpr int Save            = 1005;
    static constexpr int SaveAs          = 1006;
    static constexpr int CloseTab        = 1007;
    static constexpr int Quit            = 1008;

    static constexpr int Undo            = 2001;
    static constexpr int Redo            = 2002;
    static constexpr int Cut             = 2003;
    static constexpr int Copy            = 2004;
    static constexpr int Paste           = 2005;

    static constexpr int ToggleExplorer      = 3001;
    static constexpr int ToggleChat          = 3002;
    static constexpr int ToggleVisualizer    = 3003;
    static constexpr int ToggleTerminal      = 3004;
    static constexpr int ToggleProblems      = 3005;
    static constexpr int ToggleSourceControl = 3006;
    static constexpr int ToggleDebug         = 3007;

    static constexpr int TransportPlay       = 4001;
    static constexpr int TransportStop       = 4002;
    static constexpr int TransportRecord     = 4003;

    static constexpr int HelpSettings        = 5001;
    static constexpr int HelpShortcuts       = 5002;
    static constexpr int HelpDocs            = 5003;
    static constexpr int HelpAbout           = 5004;

    // --- JUCE MenuBarModel overrides ---

    juce::StringArray getMenuBarNames() override
    {
        return {"File", "Edit", "View", "Transport", "Help"};
    }

    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex,
                                    const juce::String& /*menuName*/) override
    {
        juce::PopupMenu menu;

        if (topLevelMenuIndex == 0)  // File
        {
            menu.addItem(NewFile,     "New File…");
            menu.addItem(OpenFile,    "Open File…");
            menu.addItem(OpenFolder,  "Open Folder…");

            // Populate Open Recent dynamically each time the menu opens.
            juce::StringArray recentList;
            if (callbacks_.getRecentProjects)
            {
                const auto recent = callbacks_.getRecentProjects();
                for (const auto& r : recent)
                    recentList.add(juce::String(r));
            }
            else
            {
                recentList = callbacks_.recentProjects;
            }

            if (!recentList.isEmpty())
            {
                juce::PopupMenu recent;
                for (int i = 0; i < recentList.size(); ++i)
                    recent.addItem(OpenRecentBase + i, recentList[i]);
                menu.addSubMenu("Open Recent", recent);
            }

            menu.addSeparator();
            menu.addItem(Save,       "Save");
            menu.addItem(SaveAs,     "Save As…");
            menu.addItem(CloseTab,   "Close Tab");
            menu.addSeparator();
            menu.addItem(Quit,       "Quit");
        }
        else if (topLevelMenuIndex == 1)  // Edit
        {
            menu.addItem(Undo,   "Undo");
            menu.addItem(Redo,   "Redo");
            menu.addSeparator();
            menu.addItem(Cut,    "Cut");
            menu.addItem(Copy,   "Copy");
            menu.addItem(Paste,  "Paste");
        }
        else if (topLevelMenuIndex == 2)  // View
        {
            menu.addItem(ToggleExplorer,      "Show Explorer");
            menu.addItem(ToggleChat,          "Show Chat");
            menu.addItem(ToggleVisualizer,    "Show Visualizer");
            menu.addItem(ToggleTerminal,      "Show Terminal");
            menu.addItem(ToggleProblems,      "Show Problems");
            menu.addItem(ToggleSourceControl, "Show Source Control");
            menu.addItem(ToggleDebug,         "Show Debug & Inspector");
        }
        else if (topLevelMenuIndex == 3)  // Transport
        {
            menu.addItem(TransportPlay,   "Play");
            menu.addItem(TransportStop,   "Stop");
            menu.addItem(TransportRecord, "Record");
        }
        else if (topLevelMenuIndex == 4)  // Help
        {
            menu.addItem(HelpSettings,  "Settings…");
            menu.addItem(HelpShortcuts, "Keyboard Shortcuts…");
            menu.addItem(HelpDocs,      "Hathor Documentation");
            menu.addSeparator();
            menu.addItem(HelpAbout,     "About Hathor");
        }

        return menu;
    }

    void menuItemSelected(int menuItemID,
                          int /*topLevelMenuIndex*/) override
    {
        // Try action registry first (dispatches registered actions like
        // file.save, file.saveAs, tab.close, etc.).
        if (actionRegistry_ && dispatchThroughRegistry(menuItemID))
            return;

        // Fall back to direct callbacks.
        switch (menuItemID)
        {
            case NewFile:       if (callbacks_.onNewFile)       callbacks_.onNewFile();       break;
            case OpenFile:      if (callbacks_.onOpenFile)      callbacks_.onOpenFile();      break;
            case OpenFolder:    if (callbacks_.onOpenFolder)    callbacks_.onOpenFolder();    break;
            case Quit:          if (callbacks_.onQuit)          callbacks_.onQuit();          break;
            case Undo:          handleTextEditorCommand(juce::StandardApplicationCommandIDs::undo);      break;
            case Redo:          handleTextEditorCommand(juce::StandardApplicationCommandIDs::redo);      break;
            case Cut:           handleTextEditorCommand(juce::StandardApplicationCommandIDs::cut);       break;
            case Copy:          handleTextEditorCommand(juce::StandardApplicationCommandIDs::copy);      break;
            case Paste:         handleTextEditorCommand(juce::StandardApplicationCommandIDs::paste);     break;
            case ToggleExplorer:      if (callbacks_.onToggleExplorer)      callbacks_.onToggleExplorer();      break;
            case ToggleChat:          if (callbacks_.onToggleChat)          callbacks_.onToggleChat();          break;
            case ToggleVisualizer:    if (callbacks_.onToggleVisualizer)    callbacks_.onToggleVisualizer();    break;
            case ToggleTerminal:      if (callbacks_.onToggleTerminal)      callbacks_.onToggleTerminal();      break;
            case ToggleProblems:      if (callbacks_.onToggleProblems)      callbacks_.onToggleProblems();      break;
            case ToggleSourceControl: if (callbacks_.onToggleSourceControl) callbacks_.onToggleSourceControl(); break;
            case ToggleDebug:         if (callbacks_.onToggleDebug)         callbacks_.onToggleDebug();         break;
            case TransportPlay:       if (callbacks_.onPlay)      callbacks_.onPlay();          break;
            case TransportStop:       if (callbacks_.onStop)      callbacks_.onStop();          break;
            case TransportRecord:     if (callbacks_.onRecord)    callbacks_.onRecord();        break;
            case HelpSettings:        if (callbacks_.onOpenSettings) callbacks_.onOpenSettings(); break;
            case HelpShortcuts:       if (callbacks_.onOpenShortcuts) callbacks_.onOpenShortcuts(); break;
            case HelpDocs:            if (callbacks_.onOpenDocs) callbacks_.onOpenDocs(); break;
            case HelpAbout:           if (callbacks_.onAbout)  callbacks_.onAbout();  break;
            default:
                if (menuItemID >= OpenRecentBase && menuItemID < OpenRecentBase + 10)
                {
                    const int idx = menuItemID - OpenRecentBase;
                    if (callbacks_.onSelectRecent)
                        callbacks_.onSelectRecent(idx);
                }
                break;
        }
    }

private:
    hathor::ui::ActionRegistry* actionRegistry_ = nullptr;
    Callbacks callbacks_;

    /// Map menu item IDs that correspond to registered actions.
    bool dispatchThroughRegistry(int menuItemID) noexcept
    {
        if (menuItemID == Save)       return actionRegistry_->dispatch("file.save");
        if (menuItemID == SaveAs)     return actionRegistry_->dispatch("file.saveAs");
        if (menuItemID == CloseTab)   return actionRegistry_->dispatch("tab.close");
        if (menuItemID == NewFile)    return actionRegistry_->dispatch("tab.new");
        if (menuItemID == ToggleExplorer)      return actionRegistry_->dispatch("view.toggleExplorer");
        if (menuItemID == ToggleChat)          return actionRegistry_->dispatch("view.toggleChat");
        if (menuItemID == ToggleVisualizer)    return actionRegistry_->dispatch("view.toggleVisualizer");
        if (menuItemID == ToggleTerminal)      return actionRegistry_->dispatch("view.toggleTerminal");
        if (menuItemID == ToggleProblems)      return actionRegistry_->dispatch("view.toggleProblems");
        if (menuItemID == ToggleSourceControl) return actionRegistry_->dispatch("view.toggleSourceControl");
        if (menuItemID == ToggleDebug)         return actionRegistry_->dispatch("view.toggleDebug");
        return false;
    }

    /// Send a standard text-editor command (undo/redo/cut/copy/paste) to
    /// whatever component currently has keyboard focus.
    void handleTextEditorCommand(int commandID)
    {
        auto* focus = juce::Component::getCurrentlyFocusedComponent();
        if (auto* editor = dynamic_cast<juce::TextEditor*>(focus))
        {
            switch (commandID)
            {
                case juce::StandardApplicationCommandIDs::undo:  editor->undo();   break;
                case juce::StandardApplicationCommandIDs::redo:  editor->redo();   break;
                case juce::StandardApplicationCommandIDs::cut:   editor->cut();    break;
                case juce::StandardApplicationCommandIDs::copy:  editor->copy();   break;
                case juce::StandardApplicationCommandIDs::paste: editor->paste();  break;
                default: break;
            }
        }
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorMenuBarModel)
};

} // namespace hathor::ui
