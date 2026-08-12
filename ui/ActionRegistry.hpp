// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ActionRegistry.hpp — JUCE-free registry of editor IDE actions and their
 * keyboard shortcut bindings.
 *
 * Each action is identified by a stable string ID (e.g. "editor.find",
 * "editor.toggleFold", "group.splitVertical").  Actions can be:
 *   - Registered with an optional human-readable name, category, and
 *     one or more key-equivalent strings.
 *   - Looked up by ID for programmatic dispatch.
 *   - Looked up by key-equivalent to map keystrokes to actions.
 *
 * Key equivalents use a simple, portable notation:
 *   "Ctrl+Shift+F"   "Cmd+."   "Alt+Left"   "F5"   "Ctrl+Alt+Enter"
 *   Modifiers: Ctrl | Cmd | Alt | Shift (order-independent, '+'-separated)
 *   Final token: a single character, a function-key name (F1..F12, Enter,
 *   Tab, Escape, Up, Down, Left, Right, Backspace, Delete), or a
 *   named key (Space, Home, End, PageUp, PageDown).
 *
 * The registry is intentionally JUCE-free so it can be unit-tested in
 * `hathor-ui-tests` without linking the GUI framework.
 *
 * Requirement references: L-1 §5 (keyboard shortcut/action registry)
 */

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hathor::ui {

/// Normalised modifier flags as a bitmask.
enum class ModFlag : uint8_t
{
    None   = 0,
    Ctrl   = 1,
    Cmd    = 2,
    Alt    = 4,
    Shift  = 8,
};

inline ModFlag operator|(ModFlag a, ModFlag b) noexcept
{
    return static_cast<ModFlag>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

inline ModFlag operator&(ModFlag a, ModFlag b) noexcept
{
    return static_cast<ModFlag>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

inline bool hasFlag(ModFlag v, ModFlag f) noexcept
{
    return (static_cast<uint8_t>(v) & static_cast<uint8_t>(f)) != 0;
}

/// Parsed key equivalent.
struct KeyEquivalent
{
    ModFlag     modifiers{ ModFlag::None };
    std::string key;                 ///< e.g. "F", "Enter", "F1", "Tab"

    bool operator==(const KeyEquivalent& o) const noexcept
    {
        return modifiers == o.modifiers && key == o.key;
    }
};

/// An editor action known to the registry.
struct EditorAction
{
    std::string id;            ///< stable machine-readable ID (e.g. "editor.find")
    std::string label;         ///< human-readable name shown in palette
    std::string category;      ///< grouping (e.g. "Editor", "Go", "Run", "Window")
    std::string description;   ///< optional longer description / tooltip
};

/// Hash helper for KeyEquivalent.
struct KeyEquivalentHash
{
    std::size_t operator()(const KeyEquivalent& k) const noexcept
    {
        // FNV-1a on modifiers + key
        std::size_t h = static_cast<std::size_t>(k.modifiers);
        for (char c : k.key)
            h = (h ^ static_cast<std::size_t>(c)) * 14000037ULL;
        return h;
    }
};

/**
 * ActionRegistry
 *
 * Stores actions and their keyboard shortcut bindings.  Callers register
 * actions (and optionally bind shortcuts) at startup, then look up actions
 * by ID or key-equivalent at runtime.
 *
 * The registry does NOT own the callbacks — they are stored as
 * std::function<void()> and installed by the caller.  This keeps the
 * registry JUCE-free and testable.
 */
class ActionRegistry
{
public:
    /**
     * Register or replace an action descriptor.
     * If an action with the same ID already exists, its metadata is replaced
     * (bindings are preserved unless `clearBindings` is true).
     */
    void registerAction(std::string id, std::string label,
                        std::string category, std::string description = {});

    /** Return true if an action with the given ID is registered. */
    bool hasAction(std::string_view id) const noexcept;

    /** Look up the action descriptor by ID. */
    const EditorAction* getAction(std::string_view id) const noexcept;

    /**
     * List all registered actions, optionally filtered by category.
     * The returned vector is stable-sorted by category, then label.
     */
    std::vector<const EditorAction*> listActions(std::string_view categoryFilter = {}) const;

    /**
     * Bind a key equivalent to an action ID.
     * Replaces any existing binding for the same key.
     * Returns false if the action ID is unknown.
     */
    bool bindKey(const KeyEquivalent& key, std::string actionId);

    /**
     * Remove the key binding for an action ID (if any).
     */
    void unbindKey(const std::string& actionId);

    /**
     * Look up an action by its key equivalent.
     * Returns empty string if no binding matches.
     */
    std::string findActionForKey(const KeyEquivalent& key) const;

    /**
     * Install (or replace) the executable callback for an action.
     */
    void setCallback(std::string actionId, std::function<void()> cb);

    /**
     * Dispatch an action by ID.  Returns true if the action existed and had
     * a callback installed.
     */
    bool dispatch(std::string_view actionId);

    /**
     * Dispatch an action by parsed key equivalent.
     * Returns true if a matching binding with a callback was found.
     */
    bool dispatchKey(const KeyEquivalent& key);

    /** Clear all actions, bindings, and callbacks. */
    void clear() noexcept;

private:
    struct ActionEntry
    {
        EditorAction                         info;
        std::optional<KeyEquivalent>         keyEquivalent;
        std::function<void()>                callback;
    };

    std::vector<ActionEntry>                               actions_;
    std::unordered_map<std::string, size_t>                idToIndex_;
    std::unordered_map<KeyEquivalent, std::string, KeyEquivalentHash> keyToId_;
};

/**
 * Parse a human-readable key string like "Ctrl+Shift+F" or "Cmd+."
 * into a KeyEquivalent.  Returns nullopt on parse error.
 */
std::optional<KeyEquivalent> parseKeyEquivalent(std::string_view text);

} // namespace hathor::ui
