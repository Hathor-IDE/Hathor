// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * AssetTarget.hpp — Studio vs Live Jam rendering target model (B8-K1).
 *
 * B8-K1 owns: target selection, target representation, target path resolution,
 * and persistence/lifetime semantics.  B8-K2 owns: rendering the ChucK
 * instrument and writing the resulting PCM data into the resolved .wav path.
 *
 * Two explicit destinations exist for ChucK-rendered .wav assets:
 *
 *   - Studio  — permanent production asset, survives project/application
 *               restarts.  Writes to .hathor_assets/chuck_instruments/<name>.wav.
 *               This is the default.
 *
 *   - LiveJam — temporary/session-scoped asset for disposable material
 *               (four-bar risers, one-off transitions, temporary textures).
 *               Cleaned up at session end.  Always an explicit opt-in.
 *
 * The target is selected per-bake and passed explicitly to the render pipeline.
 * It is NEVER inferred from a filename.
 *
 * This header is JUCE-free (plain std::filesystem) so it can be unit-tested
 * and linked by both the engine (JUCE-free) and the UI (JUCE-dependent) layers.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace hathor {

// ---------------------------------------------------------------------------
// AssetTarget — strongly typed render destination
// ---------------------------------------------------------------------------

/// The two supported destinations for ChucK-rendered .wav assets.
///
/// Passed explicitly to the bake/render pipeline (B8-K2).  The renderer never
/// infers the target from a filename, and never silently redirects a failed
/// Studio render into LiveJam or vice versa.
enum class AssetTarget : std::uint8_t {
    /// Permanent production asset.  Written to
    ///   <project>/.hathor_assets/chuck_instruments/<name>.wav
    /// Survives closing/reopening the project, restarting Hathor, and
    /// returning to the project months later.
    Studio = 0,

    /// Temporary session-scoped asset for disposable material.
    /// Written to a session-unique temp directory.  Cleaned up at session end.
    /// Always an explicit opt-in — never the default.
    LiveJam = 1,
};

/// Returns the string label for a target (for logging, JSON responses, UI).
inline constexpr const char* toString(AssetTarget t) noexcept
{
    return t == AssetTarget::Studio ? "studio" : "live_jam";
}

/// Parse a target name ("studio" / "live_jam") into an AssetTarget.
/// Returns false if @p s does not match a known target name.
inline bool parseAssetTarget(std::string_view s, AssetTarget& out) noexcept
{
    if (s == "studio")        { out = AssetTarget::Studio; return true; }
    if (s == "live_jam")      { out = AssetTarget::LiveJam; return true; }
    if (s == "live-jam")      { out = AssetTarget::LiveJam; return true; }
    if (s == "livejam")       { out = AssetTarget::LiveJam; return true; }
    return false;
}

/// The default target when the caller does not explicitly select one.
/// Per PROGRAM.md B8-K1 §4: Studio is ALWAYS the default.  Live Jam must
/// always be an explicit opt-in.
inline constexpr AssetTarget kDefaultAssetTarget = AssetTarget::Studio;

// ---------------------------------------------------------------------------
// Asset naming & sanitisation
// ---------------------------------------------------------------------------

/**
 * Sanitise an instrument name for use as a filesystem path component.
 *
 * Rules:
 *  - Replace path separators ('/', '\\') and NUL with '_'.
 *  - Collapse consecutive separators.
 *  - Reject empty results (returns "unnamed").
 *  - Strip leading dots that could hide the file or escape (.hathor_assets).
 *  - Truncate to a safe length.
 *
 * This prevents path traversal: a name like "../etc/passwd" or "sub/dir/kick"
 * is reduced to a single safe filename component that cannot escape the
 * intended asset directory.
 *
 * @return a sanitised, single-component filename (no path separators).
 */
inline std::string sanitizeAssetName(std::string_view raw)
{
    if (raw.empty())
        return "unnamed";

    std::string out;
    out.reserve(raw.size());

    bool prevSep = false;
    for (char c : raw)
    {
        // Replace any path separator or NUL with a single underscore.
        if (c == '/' || c == '\\' || c == '\0')
        {
            if (!prevSep)
                out.push_back('_');
            prevSep = true;
            continue;
        }

        // Strip leading dots that could make a hidden file or escape.
        if (out.empty() && c == '.')
            continue;

        out.push_back(c);
        prevSep = false;
    }

    // Collapse any remaining leading dots.
    while (!out.empty() && out.front() == '.')
        out.erase(out.begin());

    if (out.empty())
        return "unnamed";

    // Truncate to a safe length (file-system limit for a single component varies,
    // but 200 is well within all supported platforms).
    constexpr std::size_t kMaxNameLen = 200;
    if (out.size() > kMaxNameLen)
        out.resize(kMaxNameLen);

    return out;
}

} // namespace hathor
