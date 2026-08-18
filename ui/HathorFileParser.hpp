// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace hathor::ui {

/// Front-matter metadata extracted from the optional `[hathor]` block of a .hathor file.
struct FrontMatter {
    std::optional<std::string> slot;   ///< e.g. "d1" — pattern slot identifier
    std::optional<double>      bpm;    ///< global BPM override; valid range [20.0, 400.0]
    std::optional<std::string> bank;   ///< sample-bank path override (stretch goal Phase 2, req Phase 3)
    std::optional<std::string> label;  ///< tab display name, ≤ 64 UTF-8 chars
    std::optional<std::string> color;  ///< CSS hex colour code, e.g. "#e05a5a"
};

/// A parsed .hathor file — optional front-matter metadata plus the mini-notation body.
struct HathorFile {
    FrontMatter front;
    std::string body;  ///< raw mini-notation text (everything after the blank separator)
};

/// Returned by parseHathorFile() when the front-matter block is malformed.
struct ParseFileError {
    int         line;    ///< 1-based line number of the first error
    std::string message;
};

/// Parse a .hathor file from UTF-8 string contents.
///
/// Parsing rules:
///   1. If the first non-empty line is not `[hathor]`, the entire contents is body; front is
///      all-nullopt.
///   2. Otherwise, read key = value lines until the first blank line. The blank line is
///      consumed; the remainder is body.
///   3. Any line in the front-matter block that is neither blank nor `key = value` form is a
///      ParseFileError with the 1-based line number.
///   4. Unknown keys are silently ignored (forward-compatibility).
///   5. `bpm` must parse as a float in [20.0, 400.0]; values outside this range are a
///      ParseFileError.
///
/// Returns HathorFile on success, ParseFileError if front-matter is malformed.
std::variant<HathorFile, ParseFileError> parseHathorFile(std::string_view contents);

/// Serialise a HathorFile back to a UTF-8 string.
///
/// Serialisation rules:
///   - If any front-matter field is present, emit `[hathor]\n`, then one `key = value\n` per
///     present field in declaration order (slot, bpm, bank, label, color), then `\n` blank
///     separator, then body.
///   - If ALL front-matter fields are nullopt, emit body only (no `[hathor]` block, no blank
///     separator).
///   - `bpm` is always formatted with exactly one decimal digit (e.g. `120.0`, `93.5`).
std::string serialiseHathorFile(const HathorFile& file);

} // namespace hathor::ui
