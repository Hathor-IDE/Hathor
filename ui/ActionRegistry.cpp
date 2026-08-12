// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ActionRegistry.cpp — implementation.
 *
 * Requirement references: L-1 §5
 */

#include "ActionRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void ActionRegistry::registerAction(std::string id, std::string label,
                                    std::string category, std::string description)
{
    auto it = idToIndex_.find(id);
    if (it != idToIndex_.end())
    {
        auto& entry = actions_[it->second];
        entry.info.label       = std::move(label);
        entry.info.category     = std::move(category);
        entry.info.description  = std::move(description);
        // Keep existing keyEquivalent + callback
        return;
    }

    ActionEntry entry;
    entry.info.id          = std::move(id);
    entry.info.label       = std::move(label);
    entry.info.category    = std::move(category);
    entry.info.description = std::move(description);

    idToIndex_[entry.info.id] = actions_.size();
    actions_.push_back(std::move(entry));
}

bool ActionRegistry::hasAction(std::string_view id) const noexcept
{
    return idToIndex_.find(std::string(id)) != idToIndex_.end();
}

const EditorAction* ActionRegistry::getAction(std::string_view id) const noexcept
{
    auto it = idToIndex_.find(std::string(id));
    if (it == idToIndex_.end())
        return nullptr;
    return &actions_[it->second].info;
}

std::vector<const EditorAction*> ActionRegistry::listActions(std::string_view categoryFilter) const
{
    std::vector<const EditorAction*> result;
    result.reserve(actions_.size());

    for (const auto& a : actions_)
    {
        if (!categoryFilter.empty() && a.info.category != categoryFilter)
            continue;
        result.push_back(&a.info);
    }

    std::sort(result.begin(), result.end(),
              [](const EditorAction* a, const EditorAction* b)
              {
                  if (a->category != b->category)
                      return a->category < b->category;
                  return a->label < b->label;
              });

    return result;
}

// ---------------------------------------------------------------------------
// Key bindings
// ---------------------------------------------------------------------------

bool ActionRegistry::bindKey(const KeyEquivalent& key, std::string actionId)
{
    auto it = idToIndex_.find(actionId);
    if (it == idToIndex_.end())
        return false;

    auto& entry = actions_[it->second];

    // Remove any old reverse binding
    if (entry.keyEquivalent.has_value())
        keyToId_.erase(*entry.keyEquivalent);

    entry.keyEquivalent = key;
    keyToId_[key] = actionId;
    return true;
}

void ActionRegistry::unbindKey(const std::string& actionId)
{
    auto it = idToIndex_.find(actionId);
    if (it == idToIndex_.end())
        return;
    auto& entry = actions_[it->second];
    if (entry.keyEquivalent.has_value())
    {
        keyToId_.erase(*entry.keyEquivalent);
        entry.keyEquivalent.reset();
    }
}

std::string ActionRegistry::findActionForKey(const KeyEquivalent& key) const
{
    auto it = keyToId_.find(key);
    if (it == keyToId_.end())
        return {};
    return it->second;
}

// ---------------------------------------------------------------------------
// Callbacks / dispatch
// ---------------------------------------------------------------------------

void ActionRegistry::setCallback(std::string actionId, std::function<void()> cb)
{
    auto it = idToIndex_.find(actionId);
    if (it == idToIndex_.end())
        return;
    actions_[it->second].callback = std::move(cb);
}

bool ActionRegistry::dispatch(std::string_view actionId)
{
    std::string id(actionId);
    auto it = idToIndex_.find(id);
    if (it == idToIndex_.end())
        return false;
    auto& entry = actions_[it->second];
    if (!entry.callback)
        return false;
    entry.callback();
    return true;
}

bool ActionRegistry::dispatchKey(const KeyEquivalent& key)
{
    std::string actionId = findActionForKey(key);
    if (actionId.empty())
        return false;
    return dispatch(actionId);
}

void ActionRegistry::clear() noexcept
{
    actions_.clear();
    idToIndex_.clear();
    keyToId_.clear();
}

// ---------------------------------------------------------------------------
// Key equivalent parsing
// ---------------------------------------------------------------------------

namespace {

struct ParsedMod
{
    ModFlag flag;
    const char* text;
    size_t    len;
};

const ParsedMod modTable[] = {
    { ModFlag::Ctrl,  "Ctrl",  4 },
    { ModFlag::Cmd,   "Cmd",   3 },
    { ModFlag::Alt,   "Alt",   3 },
    { ModFlag::Shift, "Shift", 5 },
};

const char* namedKeys[] = {
    "Enter", "Tab", "Escape", "Backspace", "Delete",
    "Home", "End", "PageUp", "PageDown", "Space",
    "Up", "Down", "Left", "Right",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"
};

bool isNamedKey(std::string_view k)
{
    for (const char* n : namedKeys)
    {
        if (k == n)
            return true;
    }
    return false;
}

} // namespace

std::optional<KeyEquivalent> parseKeyEquivalent(std::string_view text)
{
    if (text.empty())
        return std::nullopt;

    // Split on '+'
    std::vector<std::string_view> parts;
    size_t start = 0;
    size_t pos = 0;
    while (true)
    {
        pos = text.find('+', start);
        if (pos == std::string_view::npos)
        {
            parts.push_back(text.substr(start));
            break;
        }
        parts.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }

    if (parts.empty())
        return std::nullopt;

    KeyEquivalent ke;
    uint8_t modValue = 0;

    // Last part is the key; earlier parts are modifiers.
    for (size_t i = 0; i < parts.size(); ++i)
    {
        // Trim whitespace
        auto p = parts[i];
        while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
            p.remove_prefix(1);
        while (!p.empty() && (p.back() == ' ' || p.back() == '\t'))
            p.remove_suffix(1);
        if (p.empty())
            return std::nullopt;

        if (i < parts.size() - 1)
        {
            // Must be a modifier
            bool matched = false;
            for (const auto& m : modTable)
            {
                if (p.size() == m.len &&
                    std::equal(p.begin(), p.end(), m.text,
                               [](char a, char b) {
                                   return std::tolower(static_cast<unsigned char>(a)) ==
                                          std::tolower(static_cast<unsigned char>(b));
                               }))
                {
                    ModFlag thisFlag = m.flag;
                    uint8_t thisVal = static_cast<uint8_t>(thisFlag);
                    // Reject duplicate modifiers
                    if (modValue & thisVal)
                        return std::nullopt;
                    modValue |= thisVal;
                    ke.modifiers = ke.modifiers | m.flag;
                    matched = true;
                    break;
                }
            }
            if (!matched)
                return std::nullopt;
        }
        else
        {
            // This is the key token
            if (p.size() == 1)
            {
                // Single character key — preserve case
                ke.key = std::string(p);
            }
            else if (isNamedKey(p))
            {
                // Named key — preserve original case
                ke.key = std::string(p);
            }
            else
            {
                return std::nullopt;
            }
        }
    }

    if (ke.key.empty())
        return std::nullopt;

    return ke;
}

} // namespace hathor::ui
