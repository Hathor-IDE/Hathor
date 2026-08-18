// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "HathorFileParser.hpp"

#include <cerrno>
#include <cstdlib>
#include <sstream>
#include <iomanip>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Trim leading and trailing ASCII whitespace (space, tab, CR) from a string_view.
std::string_view trim(std::string_view s) noexcept {
    const auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r';
    };
    while (!s.empty() && isSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))  s.remove_suffix(1);
    return s;
}

/// Returns true if the string_view is entirely whitespace (or empty).
bool isBlankLine(std::string_view line) noexcept {
    for (char c : line) {
        if (c != ' ' && c != '\t' && c != '\r') return false;
    }
    return true;
}

/// Try to parse a key = value pair from a line.
/// A valid line matches: [a-zA-Z][a-zA-Z0-9_]* \s* = \s* .*
/// Returns true and sets key/value on success; returns false if the line doesn't match.
bool parseKeyValue(std::string_view line,
                   std::string_view& outKey,
                   std::string_view& outValue) noexcept {
    // Trim the whole line first.
    line = trim(line);
    if (line.empty()) return false;

    // Key must start with [a-zA-Z].
    if (!std::isalpha(static_cast<unsigned char>(line[0]))) return false;

    // Find the '=' separator.
    const auto eqPos = line.find('=');
    if (eqPos == std::string_view::npos) return false;

    std::string_view rawKey   = line.substr(0, eqPos);
    std::string_view rawValue = line.substr(eqPos + 1);

    std::string_view key = trim(rawKey);

    // Validate key characters: [a-zA-Z][a-zA-Z0-9_]*
    if (key.empty()) return false;
    if (!std::isalpha(static_cast<unsigned char>(key[0]))) return false;
    for (std::size_t i = 1; i < key.size(); ++i) {
        const char c = key[i];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }

    outKey   = key;
    outValue = trim(rawValue);
    return true;
}

/// Format a double with exactly one decimal place (e.g. 120.0, 93.5).
/// Uses snprintf as a fallback that works for both C++17 and C++20.
std::string formatBpm(double bpm) {
    // Use std::ostringstream with fixed precision — portable C++17.
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << bpm;
    return oss.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// parseHathorFile
// ---------------------------------------------------------------------------

std::variant<HathorFile, ParseFileError> parseHathorFile(std::string_view contents) {
    HathorFile result;

    // Split contents into lines, tracking 1-based line numbers.
    // We iterate manually to avoid constructing a vector<string>.

    // --- Step 1: Find the first non-empty line ---
    // Walk through lines looking for the first non-blank one.
    std::size_t pos = 0;
    int lineNumber  = 0;

    const auto nextLine = [&](std::string_view& line) -> bool {
        if (pos >= contents.size()) return false;
        ++lineNumber;
        const auto nl = contents.find('\n', pos);
        if (nl == std::string_view::npos) {
            line = contents.substr(pos);
            pos  = contents.size();
        } else {
            line = contents.substr(pos, nl - pos);
            pos  = nl + 1;
        }
        return true;
    };

    // Find first non-empty line.
    std::string_view firstNonEmpty;
    {
        std::string_view line;
        while (nextLine(line)) {
            if (!isBlankLine(line)) {
                firstNonEmpty = line;
                break;
            }
        }
    }

    // If no content at all (or all blank), entire contents is body.
    if (firstNonEmpty.empty()) {
        result.body = std::string(contents);
        return result;
    }

    // --- Step 2: Check for [hathor] header ---
    const std::string_view trimmedFirst = trim(firstNonEmpty);
    if (trimmedFirst != "[hathor]") {
        // No front-matter — entire contents is body, front stays all-nullopt.
        result.body = std::string(contents);
        return result;
    }

    // --- Step 3: Read key = value lines until first blank line ---
    // Track whether we actually found the closing blank line (Req 24.8).
    bool foundClosingBlank = false;
    {
        std::string_view line;
        while (nextLine(line)) {
            if (isBlankLine(line)) {
                // Blank line consumed — body is everything that remains.
                foundClosingBlank = true;
                break;
            }

            std::string_view key, value;
            if (!parseKeyValue(line, key, value)) {
                // Non-blank, non-key=value line inside front-matter block → error.
                return ParseFileError{
                    lineNumber,
                    "Malformed front-matter line: expected 'key = value'"
                };
            }

            // Dispatch known keys; unknown keys are silently ignored.
            if (key == "slot") {
                result.front.slot = std::string(value);
            } else if (key == "bpm") {
                // Parse float. std::from_chars for double is not available on Apple Clang
                // (libc++ on macOS does not implement the floating-point overloads of
                // from_chars as of Xcode 15). Fall back to strtod, which is equally
                // fast for this non-hot-path use. We construct a temporary null-terminated
                // string because strtod requires it.
                double bpmVal = 0.0;
                {
                    // Copy to a null-terminated temporary so strtod can work.
                    const std::string tmp(value);
                    char* endPtr = nullptr;
                    errno = 0;
                    bpmVal = std::strtod(tmp.c_str(), &endPtr);

                    // Verify the entire string was consumed and no error occurred.
                    const bool consumedAll = (endPtr == tmp.c_str() + tmp.size());
                    if (!consumedAll || errno == ERANGE) {
                        return ParseFileError{
                            lineNumber,
                            "Invalid bpm value: '" + std::string(value) + "'"
                        };
                    }
                    if (endPtr == tmp.c_str()) {
                        // No digits consumed — strtod returned 0 with no progress.
                        return ParseFileError{
                            lineNumber,
                            "Invalid bpm value: '" + std::string(value) + "'"
                        };
                    }
                }

                if (bpmVal < 20.0 || bpmVal > 400.0) {
                    return ParseFileError{
                        lineNumber,
                        "bpm value out of range [20.0, 400.0]: " + std::string(value)
                    };
                }

                result.front.bpm = bpmVal;
            } else if (key == "bank") {
                result.front.bank = std::string(value);
            } else if (key == "label") {
                result.front.label = std::string(value);
            } else if (key == "color") {
                result.front.color = std::string(value);
            }
            // else: unknown key — silently ignore (forward-compatibility, Req 24.3 rule 4).
        }
    }

    // Req 24.8: a [hathor] block without a closing blank line is malformed.
    if (!foundClosingBlank) {
        return ParseFileError{
            lineNumber,
            "Missing closing blank line after front-matter block"
        };
    }

    // Everything from the current position onwards is the body.
    result.body = std::string(contents.substr(pos));

    return result;
}

// ---------------------------------------------------------------------------
// serialiseHathorFile
// ---------------------------------------------------------------------------

std::string serialiseHathorFile(const HathorFile& file) {
    const FrontMatter& fm = file.front;

    // Determine whether any field is present.
    const bool hasFrontMatter =
        fm.slot.has_value()  ||
        fm.bpm.has_value()   ||
        fm.bank.has_value()  ||
        fm.label.has_value() ||
        fm.color.has_value();

    if (!hasFrontMatter) {
        // No front-matter — emit body only (Req 24.2, serialisation rule).
        return file.body;
    }

    std::string out;
    // Reserve a reasonable initial capacity to reduce reallocations.
    out.reserve(128 + file.body.size());

    out += "[hathor]\n";

    // Emit fields in declaration order: slot, bpm, bank, label, color.
    if (fm.slot.has_value()) {
        out += "slot = ";
        out += *fm.slot;
        out += '\n';
    }

    if (fm.bpm.has_value()) {
        out += "bpm = ";
        out += formatBpm(*fm.bpm);
        out += '\n';
    }

    if (fm.bank.has_value()) {
        out += "bank = ";
        out += *fm.bank;
        out += '\n';
    }

    if (fm.label.has_value()) {
        out += "label = ";
        out += *fm.label;
        out += '\n';
    }

    if (fm.color.has_value()) {
        out += "color = ";
        out += *fm.color;
        out += '\n';
    }

    // Blank separator line.
    out += '\n';

    // Body.
    out += file.body;

    return out;
}

} // namespace hathor::ui
