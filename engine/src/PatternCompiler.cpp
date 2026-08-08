// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hathor/PatternCompiler.hpp"

#include "hathor/Arc.hpp"
#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"
#include "hathor/Pattern.hpp"
#include "hathor/Rational.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hathor {

// Maximum number of events per cycle before a warning is emitted.
static constexpr std::size_t kMaxEventsPerCycleLimit = 512;

// ---------------------------------------------------------------------------
// InnerBuffer — pre-allocated storage for Event<std::string>
// ---------------------------------------------------------------------------
// Event<std::string> is not default-constructible because Rational (used in
// Arc) has no default constructor. We work around this by storing the buffer
// as raw aligned bytes and constructing/destroying elements manually.
// ---------------------------------------------------------------------------

struct InnerBuffer {
    std::vector<unsigned char> storage;  // raw byte backing store
    std::size_t                capacity; // number of Event<std::string> slots

    explicit InnerBuffer(std::size_t cap)
        : storage(cap * sizeof(Event<std::string>),
                  static_cast<unsigned char>(0))
        , capacity(cap)
    {
        // Alignment check: std::vector<unsigned char> provides at least
        // alignof(char) == 1. Event<std::string> may need stricter alignment.
        // We validate this at compile time:
        static_assert(alignof(Event<std::string>) <= alignof(std::max_align_t),
                      "Event<std::string> alignment exceeds max_align_t; "
                      "use aligned_storage or aligned_alloc instead.");
    }

    // Returns a pointer to the i-th slot (uninitialized unless written).
    Event<std::string>* slot(std::size_t i) noexcept
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return reinterpret_cast<Event<std::string>*>(storage.data()) + i;
    }

    // Returns a span of 'count' initialized slots (written by the inner query).
    std::span<Event<std::string>> span(std::size_t count) noexcept
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return {reinterpret_cast<Event<std::string>*>(storage.data()), count};
    }

    // Returns a writable span of all 'capacity' slots for use as output buffer.
    std::span<Event<std::string>> outputSpan() noexcept
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        return {reinterpret_cast<Event<std::string>*>(storage.data()), capacity};
    }
};

// ---------------------------------------------------------------------------
// lowerToParamMap
// ---------------------------------------------------------------------------

Pattern<ParamMap> lowerToParamMap(const Pattern<std::string>& src)
{
    const std::size_t maxEvts = src.maxEventsPerCycle();

    if (maxEvts > kMaxEventsPerCycleLimit) {
        std::cerr << "[hathor] PatternCompiler warning: maxEventsPerCycle ("
                  << maxEvts
                  << ") exceeds the recommended limit of "
                  << kMaxEventsPerCycleLimit
                  << ". Consider reducing pattern complexity.\n";
    }

    // Pre-allocate the intermediate Event<std::string> buffer on the worker
    // thread at compile/set time. This shared_ptr is captured by value in the
    // closure so the same heap block is reused on every query call, keeping
    // the hot path allocation-free.
    auto innerBuf = std::make_shared<InnerBuffer>(maxEvts);

    // Capture src by value via a shared_ptr so the closure owns a copy.
    auto srcPtr = std::make_shared<Pattern<std::string>>(src);

    auto fn = [srcPtr, innerBuf](Arc arc, std::span<Event<ParamMap>> outBuffer) -> std::size_t
    {
        // Query the source pattern into the pre-allocated inner buffer.
        std::span<Event<std::string>> innerSpan = innerBuf->outputSpan();
        const std::size_t count = srcPtr->query(arc, innerSpan);

        // Map each Event<std::string> → Event<ParamMap>.
        // B2: propagate sourceOffset from the string-event payload.
        const std::size_t outCount = (count < outBuffer.size()) ? count : outBuffer.size();
        for (std::size_t i = 0; i < outCount; ++i) {
            const Event<std::string>& src_event = *innerBuf->slot(i);
            ParamMap pm;
            pm.set(keys::kS, src_event.value);       // "s" = sample folder name
            pm.set(keys::kN, int64_t{0});             // "n" = default sample index
            outBuffer[i] = Event<ParamMap>{src_event.whole, src_event.active,
                                           std::move(pm),
                                           src_event.sourceOffset,
                                           src_event.slotId};
        }

        return outCount;
    };

    return Pattern<ParamMap>{std::move(fn), maxEvts};
}

} // namespace hathor
