// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_EVENT_HPP
#define HATHOR_EVENT_HPP

#include "hathor/Arc.hpp"

namespace hathor {

/**
 * A time-stamped event with a typed payload.
 *
 * @tparam T  Payload type (e.g. Value, float, std::string).
 *
 * Fields:
 *  - whole   The logical full duration of the event (within the pattern).
 *  - active  The intersection of `whole` with the current query window.
 *  - value   The event's payload.
 *
 * Requirement references: 1.3
 */
template <typename T>
struct Event {
    Arc whole;   ///< full logical arc of the event
    Arc active;  ///< arc clipped to the query window
    T   value;   ///< event payload

    // --- Metadata (B2: now-playing pipeline) ---
    // sourceOffset: byte offset of the originating token in the original
    //   mini-notation source string.  Set at parse time from Token.pos and
    //   threaded through lowerNode → Event.  Survives all combinator copies
    //   because Event is copied by-value throughout the pipeline.  0 when the
    //   event has no parseable source position (e.g. synthetic or lowered).
    // slotId: index of the slot that produced this event, captured in the
    //   audio callback loop.  -1 when no slot identity applies.
    std::size_t sourceOffset = 0; ///< byte offset in source (0 = unknown)
    int8_t      slotId       = -1; ///< originating slot index (-1 = none)
};

} // namespace hathor

#endif // HATHOR_EVENT_HPP
