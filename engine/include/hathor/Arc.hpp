// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_ARC_HPP
#define HATHOR_ARC_HPP

#include "hathor/Rational.hpp"

namespace hathor {

/**
 * Half-open rational time interval [start, end) measured in cycles.
 *
 * Requirement references: 1.2, 1.3
 */
struct Arc {
    Rational start;  ///< inclusive lower bound
    Rational end;    ///< exclusive upper bound

    // -----------------------------------------------------------------------
    // Query helpers
    // -----------------------------------------------------------------------

    /// Returns true if the arc is empty (start >= end).
    bool isEmpty() const noexcept
    {
        return start >= end;
    }

    /// Returns true if t is within [start, end).
    bool contains(Rational t) const noexcept
    {
        return t >= start && t < end;
    }

    /// Duration (end - start). May be negative or zero.
    Rational duration() const noexcept
    {
        return end - start;
    }

    // -----------------------------------------------------------------------
    // Arc intersection
    // -----------------------------------------------------------------------

    /// Returns the intersection of this arc with another.
    /// If there is no overlap, returns an empty arc (start == end).
    Arc intersect(Arc other) const noexcept
    {
        // Intersection is [max(start, other.start), min(end, other.end))
        Rational intStart = (start < other.start) ? other.start : start;
        Rational intEnd   = (end < other.end)     ? end         : other.end;

        // If intStart >= intEnd, the arcs don't overlap → return empty arc.
        if (intStart >= intEnd)
            return Arc{intStart, intStart};  // empty: start == end

        return Arc{intStart, intEnd};
    }
};

} // namespace hathor

#endif // HATHOR_ARC_HPP
