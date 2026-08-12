// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_find_replace_model.cpp — unit tests for FindReplaceModel.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

#include "FindReplaceModel.hpp"

using namespace hathor::ui;

// ===========================================================================
// Plain text find
// ===========================================================================

TEST_CASE("FindReplaceModel: plain text find next", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("hello");
    REQUIRE(m.compilePattern());

    std::string doc = "Say hello world, hello there.";
    auto r1 = m.findNext(doc, 0);
    REQUIRE(r1.has_value());
    REQUIRE(r1->start == 4);
    REQUIRE(r1->end == 9);

    auto r2 = m.findNext(doc, r1->end);
    REQUIRE(r2.has_value());
    REQUIRE(r2->start == 17);
    REQUIRE(r2->end == 22);

    auto r3 = m.findNext(doc, r2->end);
    REQUIRE_FALSE(r3.has_value());
}

TEST_CASE("FindReplaceModel: case-insensitive search", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("HELLO");
    m.setFlags(FindFlags::None);
    REQUIRE(m.compilePattern());

    std::string doc = "Say hello world, HELLO there.";
    auto r1 = m.findNext(doc, 0);
    REQUIRE(r1.has_value());
    // Without case-insensitive flag, won't match 'hello'
    REQUIRE(r1->start == 15);
    REQUIRE(r1->end == 20);
}

TEST_CASE("FindReplaceModel: case-insensitive flag", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("hello");
    m.setFlags(FindFlags::CaseSensitive);
    REQUIRE(m.compilePattern());

    std::string doc = "Say HELLO world, hello there.";
    auto r1 = m.findNext(doc, 0);
    REQUIRE(r1.has_value());
    REQUIRE(r1->start == 15);
    REQUIRE(r1->end == 20);
}

TEST_CASE("FindReplaceModel: whole word search", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("ell");
    m.setFlags(FindFlags::WholeWord);
    REQUIRE(m.compilePattern());

    std::string doc = "ell hello yellow shell";
    auto r1 = m.findNext(doc, 0);
    REQUIRE_FALSE(r1.has_value());  // 'ell' in 'hello' is not a whole word
}

TEST_CASE("FindReplaceModel: whole word match", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("hello");
    m.setFlags(FindFlags::WholeWord);
    REQUIRE(m.compilePattern());

    std::string doc = "hello hello-world hello";
    auto r1 = m.findNext(doc, 0);
    REQUIRE(r1.has_value());
    REQUIRE(r1->start == 0);
    REQUIRE(r1->end == 5);

    auto r2 = m.findNext(doc, 6);
    REQUIRE_FALSE(r2.has_value());  // 'hello-world' is not a whole-word match
}

// ===========================================================================
// Wrap around
// ===========================================================================

TEST_CASE("FindReplaceModel: wrap around", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("test");
    m.setFlags(FindFlags::WrapAround);
    REQUIRE(m.compilePattern());

    std::string doc = "test one test two";
    auto r1 = m.findNext(doc, 5);
    REQUIRE(r1.has_value());
    REQUIRE(r1->start == 10);

    // After the last match, wrap around to the first
    auto r2 = m.findNext(doc, r1->end);
    REQUIRE(r2.has_value());
    REQUIRE(r2->start == 0);
}

// ===========================================================================
// Find all
// ===========================================================================

TEST_CASE("FindReplaceModel: find all plain text", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("ab");
    REQUIRE(m.compilePattern());

    std::string doc = "ab abc abcd ab";
    auto results = m.findAll(doc);
    REQUIRE(results.size() == 3);
    REQUIRE(results[0].start == 0);
    REQUIRE(results[1].start == 6);
    REQUIRE(results[2].start == 14);
}

// ===========================================================================
// Regex search
// ===========================================================================

TEST_CASE("FindReplaceModel: regex search", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("\\d+");
    m.setFlags(FindFlags::UseRegex);
    REQUIRE(m.compilePattern());

    std::string doc = "abc 123 def 456";
    auto r1 = m.findNext(doc, 0);
    REQUIRE(r1.has_value());
    REQUIRE(r1->start == 4);
    REQUIRE(r1->end == 7);

    auto r2 = m.findNext(doc, r1->end);
    REQUIRE(r2.has_value());
    REQUIRE(r2->start == 12);
    REQUIRE(r2->end == 15);
}

TEST_CASE("FindReplaceModel: regex replacement", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("(\\w+)=(\\w+)");
    m.setReplaceText("$2=$1");
    m.setFlags(FindFlags::UseRegex);
    REQUIRE(m.compilePattern());

    std::string doc = "key=value";
    m.replaceAll(doc);
    REQUIRE(doc == "value=key");
}

// ===========================================================================
// Replace
// ===========================================================================

TEST_CASE("FindReplaceModel: plain text replace", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("old");
    m.setReplaceText("new");
    REQUIRE(m.compilePattern());

    std::string doc = "old text old";
    m.replaceAll(doc);
    REQUIRE(doc == "new text new");
}

TEST_CASE("FindReplaceModel: replace single", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("world");
    m.setReplaceText("Hathor");
    REQUIRE(m.compilePattern());

    std::string doc = "hello world";
    auto match = m.findNext(doc, 0);
    REQUIRE(match.has_value());

    m.replaceOne(doc, *match);
    REQUIRE(doc == "hello Hathor");
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST_CASE("FindReplaceModel: empty search returns no match", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("");
    std::string doc = "some text";
    auto r = m.findNext(doc, 0);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("FindReplaceModel: no match returns nullopt", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("xyz");
    std::string doc = "hello world";
    auto r = m.findNext(doc, 0);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("FindReplaceModel: from offset at end returns no match without wrap", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("a");
    std::string doc = "a";
    auto r = m.findNext(doc, 1);
    REQUIRE_FALSE(r.has_value());
}

TEST_CASE("FindReplaceModel: invalid regex returns false", "[findreplace]")
{
    FindReplaceModel m;
    m.setSearchText("[invalid");
    m.setFlags(FindFlags::UseRegex);
    REQUIRE_FALSE(m.compilePattern());
}
