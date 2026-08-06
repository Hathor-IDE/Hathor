// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

#include "hathor/ParamMap.hpp"

using namespace hathor;

// ---------------------------------------------------------------------------
// get() — basic lookup
// ---------------------------------------------------------------------------

TEST_CASE("ParamMap::get returns nullptr on empty map", "[ParamMap]")
{
    ParamMap pm;
    REQUIRE(pm.get("s") == nullptr);
}

TEST_CASE("ParamMap::get returns nullptr for missing key", "[ParamMap]")
{
    ParamMap pm;
    pm.set(keys::kS, Value{std::string{"bd"}});
    REQUIRE(pm.get("gain") == nullptr);
}

TEST_CASE("ParamMap::get finds an inserted string value", "[ParamMap]")
{
    ParamMap pm;
    pm.set(keys::kS, Value{std::string{"sn"}});
    const Value* v = pm.get(keys::kS);
    REQUIRE(v != nullptr);
    REQUIRE(std::get<std::string>(*v) == "sn");
}

TEST_CASE("ParamMap::get finds an inserted double value", "[ParamMap]")
{
    ParamMap pm;
    pm.set(keys::kGain, Value{0.75});
    const Value* v = pm.get(keys::kGain);
    REQUIRE(v != nullptr);
    REQUIRE(std::get<double>(*v) == 0.75);
}

TEST_CASE("ParamMap::get finds an inserted int64 value", "[ParamMap]")
{
    ParamMap pm;
    pm.set(keys::kN, Value{int64_t{3}});
    const Value* v = pm.get(keys::kN);
    REQUIRE(v != nullptr);
    REQUIRE(std::get<int64_t>(*v) == 3);
}

// ---------------------------------------------------------------------------
// set() — overwrite and capacity
// ---------------------------------------------------------------------------

TEST_CASE("ParamMap::set overwrites an existing key", "[ParamMap]")
{
    ParamMap pm;
    pm.set(keys::kGain, Value{1.0});
    pm.set(keys::kGain, Value{0.5});
    REQUIRE(pm.size == 1);
    REQUIRE(std::get<double>(*pm.get(keys::kGain)) == 0.5);
}

TEST_CASE("ParamMap::set fills all 16 slots without throwing", "[ParamMap]")
{
    ParamMap pm;
    // Use distinct string_view literals for each key
    static constexpr std::string_view keys[16] = {
        "k0","k1","k2","k3","k4","k5","k6","k7",
        "k8","k9","k10","k11","k12","k13","k14","k15"
    };
    for (std::size_t i = 0; i < 16; ++i) {
        REQUIRE_NOTHROW(pm.set(keys[i], Value{double(i)}));
    }
    REQUIRE(pm.size == 16);
}

TEST_CASE("ParamMap::set throws overflow_error when full and key not found", "[ParamMap]")
{
    ParamMap pm;
    static constexpr std::string_view keys[16] = {
        "k0","k1","k2","k3","k4","k5","k6","k7",
        "k8","k9","k10","k11","k12","k13","k14","k15"
    };
    for (std::size_t i = 0; i < 16; ++i) {
        pm.set(keys[i], Value{double(i)});
    }
    REQUIRE_THROWS_AS(pm.set("overflow", Value{0.0}), std::overflow_error);
}

TEST_CASE("ParamMap::set allows overwriting existing key when map is full", "[ParamMap]")
{
    ParamMap pm;
    static constexpr std::string_view keys[16] = {
        "k0","k1","k2","k3","k4","k5","k6","k7",
        "k8","k9","k10","k11","k12","k13","k14","k15"
    };
    for (std::size_t i = 0; i < 16; ++i) {
        pm.set(keys[i], Value{double(i)});
    }
    // Overwriting an existing key on a full map must not throw
    REQUIRE_NOTHROW(pm.set("k0", Value{99.0}));
    REQUIRE(std::get<double>(*pm.get("k0")) == 99.0);
    REQUIRE(pm.size == 16);
}

// ---------------------------------------------------------------------------
// merge() — rhs wins for duplicate keys, no new keys lost
// ---------------------------------------------------------------------------

TEST_CASE("ParamMap::merge rhs value wins for duplicate keys", "[ParamMap]")
{
    ParamMap lhs, rhs;
    lhs.set(keys::kGain, Value{1.0});
    rhs.set(keys::kGain, Value{0.25});

    auto result = ParamMap::merge(lhs, rhs);
    REQUIRE(std::get<double>(*result.get(keys::kGain)) == 0.25);
}

TEST_CASE("ParamMap::merge preserves lhs-only keys", "[ParamMap]")
{
    ParamMap lhs, rhs;
    lhs.set(keys::kS,    Value{std::string{"bd"}});
    lhs.set(keys::kGain, Value{0.8});
    rhs.set(keys::kN,    Value{int64_t{2}});

    auto result = ParamMap::merge(lhs, rhs);
    REQUIRE(std::get<std::string>(*result.get(keys::kS))   == "bd");
    REQUIRE(std::get<double>     (*result.get(keys::kGain)) == 0.8);
    REQUIRE(std::get<int64_t>    (*result.get(keys::kN))    == 2);
}

TEST_CASE("ParamMap::merge of two empty maps returns empty map", "[ParamMap]")
{
    ParamMap lhs, rhs;
    auto result = ParamMap::merge(lhs, rhs);
    REQUIRE(result.size == 0);
}

TEST_CASE("ParamMap::merge rhs-only keys appear in result", "[ParamMap]")
{
    ParamMap lhs, rhs;
    rhs.set(keys::kSpeed, Value{2.0});

    auto result = ParamMap::merge(lhs, rhs);
    REQUIRE(std::get<double>(*result.get(keys::kSpeed)) == 2.0);
}

// ---------------------------------------------------------------------------
// Standard key constants (req 6.5)
// ---------------------------------------------------------------------------

TEST_CASE("Standard key constants have expected values", "[ParamMap][keys]")
{
    REQUIRE(keys::kS     == "s");
    REQUIRE(keys::kN     == "n");
    REQUIRE(keys::kGain  == "gain");
    REQUIRE(keys::kSpeed == "speed");
    REQUIRE(keys::kPan   == "pan");
    REQUIRE(keys::kBegin == "begin");
    REQUIRE(keys::kEnd   == "end");
    REQUIRE(keys::kCut   == "cut");
}
