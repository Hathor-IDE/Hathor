// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef HATHOR_PARAMMAP_HPP
#define HATHOR_PARAMMAP_HPP

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string_view>

#include "hathor/Value.hpp"

namespace hathor {

// ---------------------------------------------------------------------------
// Standard parameter key constants
// ---------------------------------------------------------------------------
// All keys are string_view literals with static storage duration — no heap.
//
// Requirement references: 6.3, 6.5

namespace keys {

/// Sample folder name (string).  e.g. "bd", "sn"
inline constexpr std::string_view kS     = "s";

/// Sample index within the folder (int64_t).  e.g. 0, 1, 2
inline constexpr std::string_view kN     = "n";

/// Output gain multiplier (double).  1.0 = unity gain
inline constexpr std::string_view kGain  = "gain";

/// Playback speed multiplier (double).  1.0 = normal speed
inline constexpr std::string_view kSpeed = "speed";

/// Stereo pan position (double).  -1.0 = hard left, 0.0 = centre, 1.0 = hard right
inline constexpr std::string_view kPan   = "pan";

/// Start offset within the sample, in seconds (double).
inline constexpr std::string_view kBegin = "begin";

/// End offset within the sample, in seconds (double).  0.0 means play to end.
inline constexpr std::string_view kEnd   = "end";

/// Cut group identifier (int64_t).  Voices sharing a cut group mute each other.
inline constexpr std::string_view kCut   = "cut";

} // namespace keys

// ---------------------------------------------------------------------------
// ParamMap — fixed-capacity flat parameter container
// ---------------------------------------------------------------------------
// Storage is entirely inline (std::array); no heap allocation occurs at
// query time or during merge.  Keys are string_view values pointing into
// compile-time string literals (or a long-lived arena).  std::string values
// (e.g. sample folder name) rely on SSO for short names, keeping them on the
// stack for typical usage.
//
// Requirement references: 6.2, 6.4, 6.6

struct ParamMap {
    /// Maximum number of key/value pairs that a single ParamMap can hold.
    static constexpr std::size_t kMaxEntries = 16;

    /// A single key/value pair stored inline in the entries array.
    struct Entry {
        std::string_view key;  ///< points into a compile-time literal or arena
        Value            val;
    };

    std::array<Entry, kMaxEntries> entries{};
    std::size_t                    size = 0;

    // -----------------------------------------------------------------------
    // Constructors
    // -----------------------------------------------------------------------

    /// Default-constructs an empty ParamMap.
    ParamMap() noexcept : size(0) {}

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    /**
     * Returns a pointer to the value associated with @p key, or nullptr if
     * the key is not present.  Performs an O(kMaxEntries) linear scan.
     *
     * Requirement references: 6.2, 6.3
     */
    const Value* get(std::string_view key) const noexcept
    {
        for (std::size_t i = 0; i < size; ++i) {
            if (entries[i].key == key) {
                return &entries[i].val;
            }
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Mutation
    // -----------------------------------------------------------------------

    /**
     * Sets @p key to @p val.  If the key already exists its value is
     * overwritten in place.  If the key does not exist a new entry is
     * appended.
     *
     * @throws std::overflow_error if the map is already at capacity
     *         (size == kMaxEntries) and the key is not present.
     *
     * Requirement references: 6.2, 6.6
     */
    void set(std::string_view key, Value val)
    {
        // Overwrite existing entry if found.
        for (std::size_t i = 0; i < size; ++i) {
            if (entries[i].key == key) {
                entries[i].val = std::move(val);
                return;
            }
        }

        // Key not found — append if there is room.
        if (size == kMaxEntries) {
            throw std::overflow_error(
                "hathor::ParamMap is full (kMaxEntries = 16)");
        }

        entries[size].key = key;
        entries[size].val = std::move(val);
        ++size;
    }

    // -----------------------------------------------------------------------
    // Merge
    // -----------------------------------------------------------------------

    /**
     * Returns a new ParamMap that contains all entries from @p lhs and
     * @p rhs.  When both maps contain the same key the value from @p rhs
     * takes precedence.  No heap allocation is performed.
     *
     * @throws std::overflow_error (forwarded from set()) if the combined
     *         unique key count would exceed kMaxEntries.
     *
     * Requirement references: 6.4
     */
    static ParamMap merge(const ParamMap& lhs, const ParamMap& rhs)
    {
        ParamMap result = lhs;          // copy all lhs entries
        for (std::size_t i = 0; i < rhs.size; ++i) {
            result.set(rhs.entries[i].key, rhs.entries[i].val);  // rhs wins
        }
        return result;
    }
};

} // namespace hathor

#endif // HATHOR_PARAMMAP_HPP
