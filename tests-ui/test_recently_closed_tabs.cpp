// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_recently_closed_tabs.cpp — unit tests for RecentlyClosedTabs.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

#include "RecentlyClosedTabs.hpp"

using namespace hathor::ui;

// ===========================================================================
// Basic push/pop
// ===========================================================================

TEST_CASE("RecentlyClosedTabs: push and pop", "[recentlyclosed]")
{
    RecentlyClosedTabs r;

    TabSnapshot snap1;
    snap1.label = "tab1";
    snap1.content = "content1";
    r.push(std::move(snap1));

    REQUIRE(r.size() == 1);

    auto popped = r.pop();
    REQUIRE(popped.has_value());
    REQUIRE(popped->label == "tab1");
    REQUIRE(popped->content == "content1");
    REQUIRE(r.size() == 0);
}

TEST_CASE("RecentlyClosedTabs: pop from empty returns nullopt", "[recentlyclosed]")
{
    RecentlyClosedTabs r;
    auto result = r.pop();
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("RecentlyClosedTabs: LIFO order", "[recentlyclosed]")
{
    RecentlyClosedTabs r;

    TabSnapshot snap1;
    snap1.label = "first";
    r.push(std::move(snap1));

    TabSnapshot snap2;
    snap2.label = "second";
    r.push(std::move(snap2));

    TabSnapshot snap3;
    snap3.label = "third";
    r.push(std::move(snap3));

    auto r1 = r.pop();
    REQUIRE(r1->label == "third");

    auto r2 = r.pop();
    REQUIRE(r2->label == "second");

    auto r3 = r.pop();
    REQUIRE(r3->label == "first");

    auto r4 = r.pop();
    REQUIRE_FALSE(r4.has_value());
}

// ===========================================================================
// Max history
// ===========================================================================

TEST_CASE("RecentlyClosedTabs: respects max history", "[recentlyclosed]")
{
    RecentlyClosedTabs r(3);

    for (int i = 0; i < 5; ++i)
    {
        TabSnapshot snap;
        snap.label = "tab" + std::to_string(i);
        r.push(std::move(snap));
    }

    REQUIRE(r.size() == 3);

    // Should have tab2, tab3, tab4
    auto t = r.pop();
    REQUIRE(t->label == "tab4");
    t = r.pop();
    REQUIRE(t->label == "tab3");
    t = r.pop();
    REQUIRE(t->label == "tab2");
}

// ===========================================================================
// Peek
// ===========================================================================

TEST_CASE("RecentlyClosedTabs: peek does not remove", "[recentlyclosed]")
{
    RecentlyClosedTabs r;

    TabSnapshot snap;
    snap.label = "peekable";
    r.push(std::move(snap));

    auto peeked = r.peek();
    REQUIRE(peeked.has_value());
    REQUIRE(peeked->get().label == "peekable");

    REQUIRE(r.size() == 1);

    auto popped = r.pop();
    REQUIRE(popped->label == "peekable");
    REQUIRE(r.size() == 0);
}

TEST_CASE("RecentlyClosedTabs: peek on empty returns nullopt", "[recentlyclosed]")
{
    RecentlyClosedTabs r;
    auto result = r.peek();
    REQUIRE_FALSE(result.has_value());
}

// ===========================================================================
// Clear
// ===========================================================================

TEST_CASE("RecentlyClosedTabs: clear empties the stack", "[recentlyclosed]")
{
    RecentlyClosedTabs r;

    TabSnapshot snap;
    snap.label = "test";
    r.push(std::move(snap));

    REQUIRE(r.size() == 1);
    r.clear();
    REQUIRE(r.size() == 0);
    REQUIRE(r.empty());

    auto result = r.pop();
    REQUIRE_FALSE(result.has_value());
}

// ===========================================================================
// Full snapshot fields
// ===========================================================================

TEST_CASE("RecentlyClosedTabs: preserves all snapshot fields", "[recentlyclosed]")
{
    RecentlyClosedTabs r;

    TabSnapshot snap;
    snap.label = "myTab";
    snap.fileName = "/path/to/file.hathor";
    snap.content = "sine(440)";
    snap.cursorOffset = 5;
    r.push(std::move(snap));

    auto popped = r.pop();
    REQUIRE(popped->label == "myTab");
    REQUIRE(popped->fileName == "/path/to/file.hathor");
    REQUIRE(popped->content == "sine(440)");
    REQUIRE(popped->cursorOffset == 5);
}
