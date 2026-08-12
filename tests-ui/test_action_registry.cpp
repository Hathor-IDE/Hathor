// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_action_registry.cpp — unit tests for ActionRegistry and KeyEquivalent parsing.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

#include "ActionRegistry.hpp"

using namespace hathor::ui;

// ===========================================================================
// parseKeyEquivalent
// ===========================================================================

TEST_CASE("parseKeyEquivalent: simple key", "[actionregistry]")
{
    auto r = parseKeyEquivalent("F");
    REQUIRE(r.has_value());
    REQUIRE(r->key == "F");
    REQUIRE(r->modifiers == ModFlag::None);
}

TEST_CASE("parseKeyEquivalent: Ctrl+Shift+F", "[actionregistry]")
{
    auto r = parseKeyEquivalent("Ctrl+Shift+F");
    REQUIRE(r.has_value());
    REQUIRE(r->key == "F");
    REQUIRE(hasFlag(r->modifiers, ModFlag::Ctrl));
    REQUIRE(hasFlag(r->modifiers, ModFlag::Shift));
}

TEST_CASE("parseKeyEquivalent: order-independent", "[actionregistry]")
{
    auto r1 = parseKeyEquivalent("Shift+Ctrl+F");
    auto r2 = parseKeyEquivalent("Ctrl+Shift+F");
    REQUIRE(r1.has_value());
    REQUIRE(r2.has_value());
    REQUIRE(r1->modifiers == r2->modifiers);
    REQUIRE(r1->key == r2->key);
}

TEST_CASE("parseKeyEquivalent: Cmd+period", "[actionregistry]")
{
    auto r = parseKeyEquivalent("Cmd+.");
    REQUIRE(r.has_value());
    REQUIRE(r->key == ".");
    REQUIRE(hasFlag(r->modifiers, ModFlag::Cmd));
}

TEST_CASE("parseKeyEquivalent: special keys", "[actionregistry]")
{
    for (auto name : {"Enter", "Tab", "Escape", "F1", "F12", "Up", "Down", "Left", "Right",
                       "Backspace", "Delete", "Home", "End", "PageUp", "PageDown", "Space"})
    {
        auto r = parseKeyEquivalent(name);
        REQUIRE(r.has_value());
        REQUIRE(r->key == name);
        REQUIRE(r->modifiers == ModFlag::None);
    }
}

TEST_CASE("parseKeyEquivalent: invalid returns nullopt", "[actionregistry]")
{
    REQUIRE_FALSE(parseKeyEquivalent("").has_value());
    REQUIRE_FALSE(parseKeyEquivalent("Ctrl+").has_value());
    REQUIRE_FALSE(parseKeyEquivalent("Bogus+Ctrl").has_value());
    REQUIRE_FALSE(parseKeyEquivalent("Ctrl+Ctrl+F").has_value());
}

TEST_CASE("parseKeyEquivalent: unknown modifier returns nullopt", "[actionregistry]")
{
    auto r = parseKeyEquivalent("Meta+F");
    REQUIRE_FALSE(r.has_value());
}

// ===========================================================================
// ActionRegistry registration
// ===========================================================================

TEST_CASE("ActionRegistry: register and lookup", "[actionregistry]")
{
    ActionRegistry reg;
    reg.registerAction("editor.find", "Find", "Editor", "Search in file");

    REQUIRE(reg.hasAction("editor.find"));
    auto* a = reg.getAction("editor.find");
    REQUIRE(a != nullptr);
    REQUIRE(a->label == "Find");
    REQUIRE(a->category == "Editor");
    REQUIRE(a->description == "Search in file");
}

TEST_CASE("ActionRegistry: hasAction returns false for unknown", "[actionregistry]")
{
    ActionRegistry reg;
    REQUIRE_FALSE(reg.hasAction("nonexistent"));
}

TEST_CASE("ActionRegistry: registerAction replaces existing", "[actionregistry]")
{
    ActionRegistry reg;
    reg.registerAction("editor.find", "Find", "Editor", "Search in file");
    reg.registerAction("editor.find", "Find in Files", "Editor", "Search everywhere");

    auto* a = reg.getAction("editor.find");
    REQUIRE(a != nullptr);
    REQUIRE(a->label == "Find in Files");
}

TEST_CASE("ActionRegistry: listActions sorted", "[actionregistry]")
{
    ActionRegistry reg;
    reg.registerAction("a1", "Alpha", "CategoryB", "");
    reg.registerAction("a2", "Beta", "CategoryA", "");
    reg.registerAction("a3", "Gamma", "CategoryA", "");

    auto all = reg.listActions();
    REQUIRE(all.size() == 3);

    // Sorted by category then label
    REQUIRE(all[0]->category == "CategoryA");
    REQUIRE(all[0]->label == "Beta");
    REQUIRE(all[1]->category == "CategoryA");
    REQUIRE(all[1]->label == "Gamma");
    REQUIRE(all[2]->category == "CategoryB");
}

TEST_CASE("ActionRegistry: listActions with category filter", "[actionregistry]")
{
    ActionRegistry reg;
    reg.registerAction("a1", "Alpha", "Editor", "");
    reg.registerAction("a2", "Beta", "Go", "");

    auto filtered = reg.listActions("Editor");
    REQUIRE(filtered.size() == 1);
    REQUIRE(filtered[0]->label == "Alpha");
}

// ===========================================================================
// Key binding
// ===========================================================================

TEST_CASE("ActionRegistry: bindKey and dispatch", "[actionregistry]")
{
    ActionRegistry reg;
    reg.registerAction("editor.find", "Find", "Editor", "");

    auto key = parseKeyEquivalent("Cmd+F");
    REQUIRE(key.has_value());

    bool called = false;
    reg.setCallback("editor.find", [&called]() { called = true; });
    reg.bindKey(*key, "editor.find");

    REQUIRE(reg.dispatchKey(*key));
    REQUIRE(called);
}

TEST_CASE("ActionRegistry: bindKey replaces existing binding", "[actionregistry]")
{
    ActionRegistry reg;
    reg.registerAction("editor.find", "Find", "Editor", "");
    reg.registerAction("editor.replace", "Replace", "Editor", "");

    auto keyF = parseKeyEquivalent("Cmd+F");
    auto keyH = parseKeyEquivalent("Cmd+H");
    REQUIRE(keyF.has_value());
    REQUIRE(keyH.has_value());

    reg.bindKey(*keyF, "editor.find");
    reg.bindKey(*keyH, "editor.replace");

    // Rebind Cmd+F to replace
    reg.bindKey(*keyF, "editor.replace");

    REQUIRE(reg.findActionForKey(*keyF) == "editor.replace");
    REQUIRE(reg.findActionForKey(*keyH) == "editor.replace");
}

TEST_CASE("ActionRegistry: dispatch unknown action returns false", "[actionregistry]")
{
    ActionRegistry reg;
    REQUIRE_FALSE(reg.dispatch("nonexistent"));
}

TEST_CASE("ActionRegistry: dispatch key without binding returns false", "[actionregistry]")
{
    ActionRegistry reg;
    reg.registerAction("editor.find", "Find", "Editor", "");

    auto key = parseKeyEquivalent("Cmd+Z");
    REQUIRE(key.has_value());
    REQUIRE_FALSE(reg.dispatchKey(*key));
}

TEST_CASE("ActionRegistry: unbindKey", "[actionregistry]")
{
    ActionRegistry reg;
    reg.registerAction("editor.find", "Find", "Editor", "");

    auto key = parseKeyEquivalent("Cmd+F");
    REQUIRE(key.has_value());
    reg.bindKey(*key, "editor.find");
    REQUIRE(reg.dispatchKey(*key));

    reg.unbindKey("editor.find");
    REQUIRE_FALSE(reg.dispatchKey(*key));
    REQUIRE(reg.findActionForKey(*key) == "");
}

TEST_CASE("ActionRegistry: clear", "[actionregistry]")
{
    ActionRegistry reg;
    reg.registerAction("a1", "A1", "Cat", "");
    reg.registerAction("a2", "A2", "Cat", "");

    reg.clear();

    REQUIRE_FALSE(reg.hasAction("a1"));
    REQUIRE_FALSE(reg.hasAction("a2"));
}
