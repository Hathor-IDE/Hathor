// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_navigation_history.cpp — unit tests for NavigationHistory.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target.
 *
 * Requirement references: L-2 §2
 */

#include <catch2/catch_test_macros.hpp>

#include "NavigationHistory.hpp"

using namespace hathor::ui;

// ===========================================================================
// Basic navigation
// ===========================================================================

TEST_CASE("NavigationHistory: initial state is empty", "[navigation]")
{
    NavigationHistory history;
    REQUIRE_FALSE(history.canGoBack());
    REQUIRE_FALSE(history.canGoForward());
    REQUIRE_FALSE(history.current().has_value());
    REQUIRE(history.backCount() == 0);
    REQUIRE(history.forwardCount() == 0);
}

TEST_CASE("NavigationHistory: setCurrent establishes current entry", "[navigation]")
{
    NavigationHistory history;
    NavigationEntry e1{"file:///test.hathor", 5, 10};
    history.setCurrent(e1);

    REQUIRE(history.current().has_value());
    REQUIRE(history.current()->uri == "file:///test.hathor");
    REQUIRE(history.current()->line == 5);
    REQUIRE(history.current()->column == 10);

    // setCurrent does not push to back stack
    REQUIRE_FALSE(history.canGoBack());
}

TEST_CASE("NavigationHistory: navigateTo pushes current to back stack", "[navigation]")
{
    NavigationHistory history;

    NavigationEntry e1{"file:///a.hathor", 0, 0};
    NavigationEntry e2{"file:///b.hathor", 5, 10};

    history.setCurrent(e1);
    history.navigateTo(e2);

    REQUIRE(history.canGoBack());
    REQUIRE_FALSE(history.canGoForward());

    auto current = history.current();
    REQUIRE(current.has_value());
    REQUIRE(current->uri == "file:///b.hathor");
}

// ===========================================================================
// Back / Forward
// ===========================================================================

TEST_CASE("NavigationHistory: goBack returns to previous entry", "[navigation]")
{
    NavigationHistory history;

    NavigationEntry e1{"file:///a.hathor", 0, 0};
    NavigationEntry e2{"file:///b.hathor", 5, 10};
    NavigationEntry e3{"file:///c.hathor", 3, 7};

    history.navigateTo(e1);
    history.navigateTo(e2);
    history.navigateTo(e3);

    REQUIRE(history.current()->uri == "file:///c.hathor");

    auto back = history.goBack();
    REQUIRE(back.has_value());
    REQUIRE(back->uri == "file:///b.hathor");

    back = history.goBack();
    REQUIRE(back.has_value());
    REQUIRE(back->uri == "file:///a.hathor");

    REQUIRE_FALSE(history.canGoBack());
    auto noMore = history.goBack();
    REQUIRE_FALSE(noMore.has_value());
}

TEST_CASE("NavigationHistory: goForward restores forward entries", "[navigation]")
{
    NavigationHistory history;

    NavigationEntry e1{"file:///a.hathor", 0, 0};
    NavigationEntry e2{"file:///b.hathor", 5, 10};

    history.navigateTo(e1);
    history.navigateTo(e2);

    history.goBack();  // at e1
    REQUIRE(history.canGoForward());

    auto fwd = history.goForward();
    REQUIRE(fwd.has_value());
    REQUIRE(fwd->uri == "file:///b.hathor");

    REQUIRE_FALSE(history.canGoForward());
}

TEST_CASE("NavigationHistory: navigateTo clears forward stack", "[navigation]")
{
    NavigationHistory history;

    NavigationEntry e1{"file:///a.hathor", 0, 0};
    NavigationEntry e2{"file:///b.hathor", 5, 10};
    NavigationEntry e3{"file:///c.hathor", 3, 7};

    history.navigateTo(e1);
    history.navigateTo(e2);
    history.goBack();  // back to e1, forward stack has e2
    REQUIRE(history.canGoForward());

    // Now navigate to a new entry — forward stack should be cleared
    history.navigateTo(e3);
    REQUIRE_FALSE(history.canGoForward());

    auto back = history.goBack();
    REQUIRE(back.has_value());
    REQUIRE(back->uri == "file:///a.hathor");
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST_CASE("NavigationHistory: goBack with empty stack returns nullopt", "[navigation]")
{
    NavigationHistory history;
    REQUIRE_FALSE(history.goBack().has_value());
    REQUIRE_FALSE(history.goForward().has_value());
}

TEST_CASE("NavigationHistory: clear empties everything", "[navigation]")
{
    NavigationHistory history;
    history.navigateTo({"file:///a.hathor", 0, 0});
    history.navigateTo({"file:///b.hathor", 5, 10});
    history.goBack();

    REQUIRE(history.canGoBack());
    REQUIRE(history.canGoForward());

    history.clear();

    REQUIRE_FALSE(history.canGoBack());
    REQUIRE_FALSE(history.canGoForward());
    REQUIRE_FALSE(history.current().has_value());
}

TEST_CASE("NavigationHistory: max size limits back stack", "[navigation]")
{
    NavigationHistory history(3);  // small max for testing

    for (int i = 0; i < 5; i++)
    {
        history.navigateTo({"file:///f" + std::to_string(i) + ".hathor", i, i});
    }

    // After 5 navigations with max back stack = 3, the back stack should have at most 3 entries
    // (current_ + max 3 in back stack)
    REQUIRE(history.backCount() <= 3);

    // Navigate back 3 times, should exhaust the back stack
    int count = 0;
    while (history.canGoBack())
    {
        history.goBack();
        count++;
    }
    // May not be 5 because older entries were evicted
    REQUIRE(count <= 3);
}
