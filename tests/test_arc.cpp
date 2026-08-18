// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"

using namespace hathor;

TEST_CASE("Arc - construction", "[arc][construction]") {
    SECTION("normal positive arc") {
        Arc a{Rational{0}, Rational{4}};
        REQUIRE(a.start == Rational{0});
        REQUIRE(a.end == Rational{4});
        REQUIRE_FALSE(a.isEmpty());
    }

    SECTION("zero-length arc") {
        Arc a{Rational{2}, Rational{2}};
        REQUIRE(a.start == Rational{2});
        REQUIRE(a.end == Rational{2});
        REQUIRE(a.isEmpty());
    }

    SECTION("negative-duration arc") {
        Arc a{Rational{4}, Rational{1}};
        REQUIRE(a.start == Rational{4});
        REQUIRE(a.end == Rational{1});
        REQUIRE(a.isEmpty());
    }

    SECTION("both endpoints zero") {
        Arc a{Rational{0}, Rational{0}};
        REQUIRE(a.isEmpty());
    }

    SECTION("negative range arc") {
        Arc a{Rational{-3}, Rational{-1}};
        REQUIRE_FALSE(a.isEmpty());
        REQUIRE(a.start == Rational{-3});
        REQUIRE(a.end == Rational{-1});
    }

    SECTION("mixed sign arc") {
        Arc a{Rational{-2}, Rational{2}};
        REQUIRE_FALSE(a.isEmpty());
        REQUIRE(a.start == Rational{-2});
        REQUIRE(a.end == Rational{2});
    }

    SECTION("rational endpoints") {
        Arc a{Rational{1, 2}, Rational{7, 2}};
        REQUIRE_FALSE(a.isEmpty());
        REQUIRE(a.start == Rational{1, 2});
        REQUIRE(a.end == Rational{7, 2});
    }
}

TEST_CASE("Arc - isEmpty", "[arc][isEmpty]") {
    REQUIRE_FALSE(Arc{Rational{0}, Rational{4}}.isEmpty());
    REQUIRE(Arc{Rational{2}, Rational{2}}.isEmpty());
    REQUIRE(Arc{Rational{4}, Rational{1}}.isEmpty());
    REQUIRE(Arc{Rational{0}, Rational{0}}.isEmpty());
    REQUIRE_FALSE(Arc{Rational{-5}, Rational{5}}.isEmpty());
    REQUIRE(Arc{Rational{3}, Rational{3, 2}}.isEmpty());
}

TEST_CASE("Arc - duration", "[arc][duration]") {
    REQUIRE(Arc{Rational{0}, Rational{4}}.duration() == Rational{4});
    REQUIRE(Arc{Rational{2}, Rational{2}}.duration() == Rational{0});
    REQUIRE(Arc{Rational{4}, Rational{1}}.duration() == Rational{-3});
    REQUIRE(Arc{Rational{0}, Rational{0}}.duration() == Rational{0});
    REQUIRE(Arc{Rational{-3}, Rational{-1}}.duration() == Rational{2});
    REQUIRE(Arc{Rational{1, 2}, Rational{3, 2}}.duration() == Rational{1});
}

TEST_CASE("Arc - contains", "[arc][contains]") {
    Arc a{Rational{0}, Rational{4}};

    SECTION("strictly inside") {
        REQUIRE(a.contains(Rational{2}));
        REQUIRE(a.contains(Rational{1}));
        REQUIRE(a.contains(Rational{3}));
    }

    SECTION("on start boundary — inclusive") {
        REQUIRE(a.contains(Rational{0}));
    }

    SECTION("on end boundary — exclusive") {
        REQUIRE_FALSE(a.contains(Rational{4}));
    }

    SECTION("outside before") {
        REQUIRE_FALSE(a.contains(Rational{-1}));
    }

    SECTION("outside after") {
        REQUIRE_FALSE(a.contains(Rational{5}));
    }

    SECTION("fractional positions") {
        REQUIRE(a.contains(Rational{1, 2}));
        REQUIRE(a.contains(Rational{7, 2}));
        REQUIRE_FALSE(a.contains(Rational{9, 2}));
    }

    SECTION("empty arc contains nothing") {
        Arc empty{Rational{2}, Rational{2}};
        REQUIRE_FALSE(empty.contains(Rational{2}));
        REQUIRE_FALSE(empty.contains(Rational{1}));
        REQUIRE_FALSE(empty.contains(Rational{3}));
    }

    SECTION("negative range arc") {
        Arc neg{Rational{2}, Rational{0}};
        REQUIRE(neg.isEmpty());
        REQUIRE_FALSE(neg.contains(Rational{1}));
        REQUIRE_FALSE(neg.contains(Rational{2}));
    }

    SECTION("mixed sign arc") {
        Arc mixed{Rational{-2}, Rational{2}};
        REQUIRE(mixed.contains(Rational{-2}));
        REQUIRE(mixed.contains(Rational{0}));
        REQUIRE(mixed.contains(Rational{1, 2}));
        REQUIRE_FALSE(mixed.contains(Rational{2}));
        REQUIRE_FALSE(mixed.contains(Rational{-3}));
    }
}

TEST_CASE("Arc - intersect overlapping arcs", "[arc][intersect]") {
    SECTION("simple overlap") {
        Arc a{Rational{0}, Rational{4}};
        Arc b{Rational{2}, Rational{6}};
        Arc r = a.intersect(b);
        REQUIRE(r.start == Rational{2});
        REQUIRE(r.end == Rational{4});
        REQUIRE_FALSE(r.isEmpty());
    }

    SECTION("identical arcs") {
        Arc a{Rational{0}, Rational{4}};
        Arc r = a.intersect(a);
        REQUIRE(r.start == Rational{0});
        REQUIRE(r.end == Rational{4});
        REQUIRE_FALSE(r.isEmpty());
    }

    SECTION("containment — other inside this") {
        Arc a{Rational{0}, Rational{4}};
        Arc b{Rational{1}, Rational{3}};
        Arc r = a.intersect(b);
        REQUIRE(r.start == Rational{1});
        REQUIRE(r.end == Rational{3});
        REQUIRE_FALSE(r.isEmpty());
    }

    SECTION("containment — this inside other") {
        Arc a{Rational{1}, Rational{3}};
        Arc b{Rational{0}, Rational{4}};
        Arc r = a.intersect(b);
        REQUIRE(r.start == Rational{1});
        REQUIRE(r.end == Rational{3});
        REQUIRE_FALSE(r.isEmpty());
    }

    SECTION("partial overlap at start") {
        Arc a{Rational{0}, Rational{4}};
        Arc b{Rational{0}, Rational{2}};
        Arc r = a.intersect(b);
        REQUIRE(r.start == Rational{0});
        REQUIRE(r.end == Rational{2});
        REQUIRE_FALSE(r.isEmpty());
    }

    SECTION("partial overlap at end") {
        Arc a{Rational{0}, Rational{4}};
        Arc b{Rational{2}, Rational{4}};
        Arc r = a.intersect(b);
        REQUIRE(r.start == Rational{2});
        REQUIRE(r.end == Rational{4});
        REQUIRE_FALSE(r.isEmpty());
    }

    SECTION("fractional overlap") {
        Arc a{Rational{0}, Rational{2}};
        Arc b{Rational{1, 2}, Rational{3, 2}};
        Arc r = a.intersect(b);
        REQUIRE(r.start == Rational{1, 2});
        REQUIRE(r.end == Rational{3, 2});
        REQUIRE_FALSE(r.isEmpty());
    }
}

TEST_CASE("Arc - intersect touching and disjoint arcs", "[arc][intersect]") {
    SECTION("touching at boundary — empty result") {
        Arc a{Rational{0}, Rational{2}};
        Arc b{Rational{2}, Rational{4}};
        Arc r = a.intersect(b);
        REQUIRE(r.isEmpty());
        REQUIRE(r.start == r.end);
    }

    SECTION("touching reversed order") {
        Arc a{Rational{2}, Rational{4}};
        Arc b{Rational{0}, Rational{2}};
        Arc r = a.intersect(b);
        REQUIRE(r.isEmpty());
    }

    SECTION("disjoint with gap") {
        Arc a{Rational{0}, Rational{2}};
        Arc b{Rational{5}, Rational{7}};
        Arc r = a.intersect(b);
        REQUIRE(r.isEmpty());
    }

    SECTION("disjoint reversed") {
        Arc a{Rational{5}, Rational{7}};
        Arc b{Rational{0}, Rational{2}};
        Arc r = a.intersect(b);
        REQUIRE(r.isEmpty());
    }

    SECTION("one completely after the other") {
        Arc a{Rational{0}, Rational{1}};
        Arc b{Rational{10}, Rational{11}};
        Arc r = a.intersect(b);
        REQUIRE(r.isEmpty());
    }
}

TEST_CASE("Arc - intersect empty arcs", "[arc][intersect]") {
    Arc empty1{Rational{2}, Rational{2}};
    Arc empty2{Rational{5}, Rational{5}};
    Arc nonempty{Rational{0}, Rational{4}};

    SECTION("empty intersect empty") {
        Arc r = empty1.intersect(empty2);
        REQUIRE(r.isEmpty());
    }

    SECTION("empty intersect non-empty") {
        Arc r = empty1.intersect(nonempty);
        REQUIRE(r.isEmpty());
    }

    SECTION("non-empty intersect empty") {
        Arc r = nonempty.intersect(empty1);
        REQUIRE(r.isEmpty());
    }

    SECTION("empty intersect self") {
        Arc r = empty1.intersect(empty1);
        REQUIRE(r.isEmpty());
    }
}

TEST_CASE("Arc - intersect negative and mixed arcs", "[arc][intersect]") {
    SECTION("both negative ranges overlapping") {
        Arc a{Rational{-5}, Rational{-2}};
        Arc b{Rational{-3}, Rational{0}};
        Arc r = a.intersect(b);
        REQUIRE(r.start == Rational{-3});
        REQUIRE(r.end == Rational{-2});
        REQUIRE_FALSE(r.isEmpty());
    }

    SECTION("negative and positive overlapping") {
        Arc a{Rational{-3}, Rational{1}};
        Arc b{Rational{-1}, Rational{3}};
        Arc r = a.intersect(b);
        REQUIRE(r.start == Rational{-1});
        REQUIRE(r.end == Rational{1});
        REQUIRE_FALSE(r.isEmpty());
    }

    SECTION("touching across zero") {
        Arc a{Rational{-2}, Rational{0}};
        Arc b{Rational{0}, Rational{2}};
        Arc r = a.intersect(b);
        REQUIRE(r.isEmpty());
    }
}

TEST_CASE("Arc - boundary and degenerate edge cases", "[arc][edge]") {
    SECTION("unit arc contains only start") {
        Arc a{Rational{0}, Rational{1}};
        REQUIRE(a.contains(Rational{0}));
        REQUIRE_FALSE(a.contains(Rational{1}));
    }

    SECTION("single rational width") {
        Arc a{Rational{0}, Rational{1, 2}};
        REQUIRE(a.contains(Rational{0}));
        REQUIRE_FALSE(a.contains(Rational{1, 2}));
        REQUIRE(a.contains(Rational{1, 4}));
    }

    SECTION("adjacent non-overlapping arcs share boundary") {
        Arc a{Rational{0}, Rational{1}};
        Arc b{Rational{1}, Rational{2}};
        REQUIRE_FALSE(a.contains(Rational{1}));
        REQUIRE(b.contains(Rational{1}));
        REQUIRE(a.intersect(b).isEmpty());
    }

    SECTION("nested arcs — outer intersects inner yields inner") {
        Arc outer{Rational{0}, Rational{10}};
        Arc inner{Rational{3}, Rational{7}};
        Arc r = outer.intersect(inner);
        REQUIRE(r.start == Rational{3});
        REQUIRE(r.end == Rational{7});
    }

    SECTION("arc crossing zero contains zero") {
        Arc a{Rational{-1}, Rational{1}};
        REQUIRE(a.contains(Rational{0}));
        REQUIRE(a.contains(Rational{-1}));
        REQUIRE_FALSE(a.contains(Rational{1}));
    }
}
