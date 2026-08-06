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
};

} // namespace hathor

#endif // HATHOR_EVENT_HPP
