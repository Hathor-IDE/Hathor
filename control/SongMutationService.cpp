// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * SongMutationService.cpp — AI-7 implementation.
 *
 * Requirement references: AI-1 §1, AI-7 §1–§12, PROGRAM.md Phase 2.5
 */

#include "SongMutationService.hpp"
#include "HathorFileParser.hpp"

#include "hathor/MiniParser.hpp"
#include "hathor/MiniTokeniser.hpp"
#include "hathor/PrettyPrinter.hpp"
#include "hathor/PatternCompiler.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace hathor::control {

using hathor::ui::FrontMatter;
using hathor::ui::HathorFile;
using hathor::ui::ParseFileError;
using hathor::ui::parseHathorFile;
using hathor::ui::serialiseHathorFile;

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

namespace {

/// ISO-8601 UTC timestamp for audit / response metadata.
std::string isoTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

/// Trim leading/trailing whitespace (space, tab, CR).
std::string_view trimSV(std::string_view sv) noexcept
{
    auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!sv.empty() && isSpace(sv.front())) sv.remove_prefix(1);
    while (!sv.empty() && isSpace(sv.back()))  sv.remove_suffix(1);
    return sv;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Validation helpers
// ---------------------------------------------------------------------------

std::string SongMutationService::validateSlotName(std::string_view name) noexcept
{
    if (name.empty())
        return "slot name is empty";

    // Slot names: d0–d15, or any alphanumeric string up to 32 chars.
    // Must start with a letter or 'd' for the conventional d0–d15 form.
    if (name.size() > 32)
        return "slot name too long (max 32 chars)";

    const bool startsWithD = (name.size() >= 2 && name[0] == 'd');
    if (startsWithD) {
        // Conventional d0..d15 form.
        if (name.size() != 2 || name[1] < '0' || name[1] > '9')
            return "invalid conventional slot name '" + std::string(name)
                   + "' (expected d0–d9)";
    } else {
        // Generic alphanumeric slot name.
        for (char c : name) {
            if (!std::isalnum(static_cast<unsigned char>(c)))
                return "slot name contains invalid characters";
        }
    }
    return {};
}

std::string SongMutationService::validateBpm(double bpm) noexcept
{
    if (bpm < 20.0 || bpm > 400.0)
        return "bpm out of range [20.0, 400.0]";
    return {};
}

std::string SongMutationService::validateLabel(std::string_view label) noexcept
{
    if (label.empty())
        return "label is empty";
    if (label.size() > 64)
        return "label exceeds 64 characters";
    return {};
}

std::string SongMutationService::validateColor(std::string_view color) noexcept
{
    // CSS hex: #rgb or #rrggbb (case-insensitive).
    if (color.empty())
        return "color is empty";
    if (color.front() != '#')
        return "color must start with '#'";
    if (color.size() != 4 && color.size() != 7)
        return "color must be #rgb or #rrggbb";
    for (std::size_t i = 1; i < color.size(); ++i) {
        const char c = color[i];
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return "color contains invalid hex digits";
    }
    return {};
}

std::string SongMutationService::validateOpsArray(const nlohmann::json& ops) noexcept
{
    if (!ops.is_array())
        return "ops must be a JSON array";
    if (ops.empty())
        return "ops array is empty";
    return {};
}

// ---------------------------------------------------------------------------
// Audit logging
// ---------------------------------------------------------------------------

void SongMutationService::auditLog(std::string_view action,
                                    std::string_view target,
                                    bool success,
                                    std::string_view detail) const noexcept
{
    std::ostringstream log;
    log << "[AI-7 AUDIT] action=" << action
        << " target=" << target
        << " success=" << (success ? "true" : "false")
        << " timestamp=" << isoTimestamp();
    if (!detail.empty())
        log << " detail=" << detail;
    std::fprintf(stderr, "%s\n", log.str().c_str());
}

// ---------------------------------------------------------------------------
// Path resolution
// ---------------------------------------------------------------------------

std::pair<std::filesystem::path, std::string>
SongMutationService::resolveSongPath(std::string_view songFile) const noexcept
{
    const auto trimmed = trimSV(songFile);
    if (trimmed.empty())
        return {{}, "song file name is empty"};

    const std::string str(trimmed);

    // Check for path traversal
    if (str.find("..") != std::string::npos)
        return {{}, "path traversal is not allowed"};

    // Must end with .hathor
    {
        std::string lower = str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        const std::string ext = ".hathor";
        if (lower.size() < ext.size() || lower.compare(lower.size() - ext.size(), ext.size(), ext) != 0)
            return {{}, "song file must have .hathor extension"};
    }

    // Resolve relative to project directory
    const auto projectDir = audio_.currentProjectDir();
    if (projectDir.empty())
        return {{}, "project directory is not set"};

    const auto resolved = projectDir / str;

    // Must be inside the project directory (after canonicalisation)
    std::error_code ec;
    const auto canonicalProject = std::filesystem::weakly_canonical(projectDir, ec);
    if (ec)
        return {{}, "cannot canonicalise project directory"};

    const auto canonicalResolved = std::filesystem::weakly_canonical(resolved, ec);
    if (ec)
        return {{}, "cannot canonicalise song file path"};

    const auto rel = std::filesystem::relative(canonicalResolved, canonicalProject, ec);
    if (ec || rel.empty())
        return {{}, "song file must be inside the project directory"};

    // Check that the relative path doesn't escape upward (..).
    std::string relStr = rel.string();
    if (relStr == ".." || relStr.starts_with(".." + std::string(1, std::filesystem::path::preferred_separator)))
        return {{}, "song file must be inside the project directory"};

    return {resolved, {}};
}

// ---------------------------------------------------------------------------
// Atomic file I/O
// ---------------------------------------------------------------------------

std::pair<std::string, std::string>
SongMutationService::readSongFile(const std::filesystem::path& path) const noexcept
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return {{}, "song file does not exist: " + path.string()};

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs)
        return {{}, "cannot open song file: " + path.string()};

    std::string contents{std::istreambuf_iterator<char>(ifs),
                         std::istreambuf_iterator<char>()};
    return {contents, {}};
}

bool SongMutationService::atomicWriteHathorFile(
    const std::filesystem::path& path,
    const std::string& content) const noexcept
{
    std::error_code ec;

    // Step 1: Backup existing file
    const auto backupPath = path;
    // backup: <name>.bak
    auto bakPath = path;
    bakPath.replace_filename(path.stem().string() + ".bak.hathor");

    bool hadExisting = false;
    if (std::filesystem::exists(path, ec)) {
        hadExisting = true;
        std::filesystem::copy_file(path, bakPath,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            auditLog("write_backup_failed", path.filename().string(), false, ec.message());
            return false;
        }
    }

    // Step 2: Write to temp file
    const auto tmpPath = path;
    auto tmpFile = path;
    tmpFile.replace_filename(path.stem().string() + ".tmp.hathor");

    {
        std::ofstream f(tmpFile, std::ios::binary | std::ios::trunc);
        if (!f.is_open() ||
            !f.write(content.data(), static_cast<std::streamsize>(content.size()))) {
            f.close();
            std::filesystem::remove(tmpFile, ec);
            auditLog("write_temp_failed", path.filename().string(), false, strerror(errno));
            return false;
        }
        f.close();
    }

    // Step 3: Verify temp file by re-parsing
    {
        std::ifstream vfs(tmpFile, std::ios::binary);
        if (!vfs) {
            std::filesystem::remove(tmpFile, ec);
            if (hadExisting) std::filesystem::remove(bakPath, ec);
            auditLog("verify_open_failed", path.filename().string(), false, strerror(errno));
            return false;
        }
        std::string verifyContent{std::istreambuf_iterator<char>(vfs),
                                  std::istreambuf_iterator<char>()};
        const auto parseResult = parseHathorFile(verifyContent);
        if (std::holds_alternative<ParseFileError>(parseResult)) {
            std::filesystem::remove(tmpFile, ec);
            if (hadExisting) std::filesystem::remove(bakPath, ec);
            auditLog("verify_parse_failed", path.filename().string(), false, "serialised file did not parse");
            return false;
        }
    }

    // Step 4: Rename temp → final (atomic on POSIX)
    std::error_code renameEc;
    std::filesystem::rename(tmpFile, path, renameEc);
    if (renameEc) {
        std::filesystem::remove(tmpFile, ec);
        if (hadExisting) std::filesystem::remove(bakPath, ec);
        auditLog("rename_failed", path.filename().string(), false, renameEc.message());
        return false;
    }

    // Clean up backup
    if (hadExisting)
        std::filesystem::remove(bakPath, ec);

    return true;
}

// ---------------------------------------------------------------------------
// Runtime state updates
// ---------------------------------------------------------------------------

void SongMutationService::updateRuntimeSlot(const HathorFile& file) noexcept
{
    if (!file.front.slot)
        return;

    const std::string slotName = *file.front.slot;
    const auto err = validateSlotName(slotName);
    if (!err.empty())
        return;

    // Find or add the slot
    const int idx = audio_.findOrAddSlot(slotName);
    if (idx < 0)
        return;

    // Parse the body to compile it
    const auto parseResult = hathor::parseMini(file.body);
    if (std::holds_alternative<hathor::ParseError>(parseResult))
        return; // body didn't parse — don't update runtime

    const auto& compiled = std::get<hathor::CompiledPattern>(parseResult);
    const std::string canonicalNotation = hathor::printMini(compiled);

    // Lower to ParamMap pattern
    auto paramPattern = hathor::lowerToParamMap(compiled.pattern);
    const std::size_t maxEvents = paramPattern.maxEventsPerCycle();

    // Allocate SlotState
    auto slotState = std::make_shared<SlotState>();
    slotState->pattern = std::make_shared<hathor::Pattern<hathor::ParamMap>>(
        std::move(paramPattern));

    // Pre-allocate event buffer (same pattern as WorkerThread)
    {
        const hathor::Rational zero{0, 1};
        const hathor::Arc      zeroArc{zero, zero};
        const hathor::Event<hathor::ParamMap> dummy{zeroArc, zeroArc, {}};
        slotState->eventBuffer.assign(maxEvents, dummy);
    }
    slotState->notation = canonicalNotation;

    audio_.storeSlot(idx, std::move(slotState));
}

void SongMutationService::applyBpm(double bpm) noexcept
{
    const auto err = validateBpm(bpm);
    if (!err.empty())
        return;
    audio_.setBpm(bpm);
}

// ---------------------------------------------------------------------------
// Operation handlers
// ---------------------------------------------------------------------------

SongMutationService::OpResult
SongMutationService::applyReplacePattern(const nlohmann::json& op, OpContext& ctx)
{
    OpResult result;

    // Must have "notation" field
    if (!op.contains("notation") || !op["notation"].is_string()) {
        result.ok = false;
        result.error = "replace_pattern requires 'notation' string field";
        result.code = "INVALID_ARGUMENT";
        return result;
    }

    const std::string notation = op["notation"].get<std::string>();

    // Validate notation with the real parser
    const auto parseResult = hathor::parseMini(notation);
    if (std::holds_alternative<hathor::ParseError>(parseResult)) {
        const auto& err = std::get<hathor::ParseError>(parseResult);
        result.ok = false;
        result.error = err.message;
        result.code = "PARSE_ERROR";
        result.extra["position"] = static_cast<int>(err.position);
        return result;
    }

    // Check confirmation requirement: if the file already has a pattern and
    // slot differs from what would be replaced, require confirmation.
    if (ctx.hasExistingPattern && !op.value("confirm", false)) {
        ctx.needsConfirm = true;
    }

    // Get canonical notation
    const auto& compiled = std::get<hathor::CompiledPattern>(parseResult);
    ctx.file.body = hathor::printMini(compiled);

    // Handle optional slot override in the operation
    if (op.contains("slot") && op["slot"].is_string()) {
        const std::string slotName = op["slot"].get<std::string>();
        const auto slotErr = validateSlotName(slotName);
        if (!slotErr.empty()) {
            result.ok = false;
            result.error = slotErr;
            result.code = "INVALID_SLOT";
            return result;
        }
        ctx.file.front.slot = slotName;
    }

    result.extra["canonical_notation"] = ctx.file.body;
    return result;
}

SongMutationService::OpResult
SongMutationService::applyInsert(const nlohmann::json& op, OpContext& ctx)
{
    OpResult result;

    if (!op.contains("notation") || !op["notation"].is_string()) {
        result.ok = false;
        result.error = "insert requires 'notation' string field";
        result.code = "INVALID_ARGUMENT";
        return result;
    }

    const std::string notation = op["notation"].get<std::string>();

    // Validate notation
    const auto parseResult = hathor::parseMini(notation);
    if (std::holds_alternative<hathor::ParseError>(parseResult)) {
        const auto& err = std::get<hathor::ParseError>(parseResult);
        result.ok = false;
        result.error = err.message;
        result.code = "PARSE_ERROR";
        result.extra["position"] = static_cast<int>(err.position);
        return result;
    }

    const auto& compiled = std::get<hathor::CompiledPattern>(parseResult);
    const std::string canonical = hathor::printMini(compiled);

    // Check confirmation: if body already has content, insertion modifies it
    if (ctx.hasExistingPattern && !op.value("confirm", false)) {
        ctx.needsConfirm = true;
    }

    // Position: "prepend" (default), "append", or "replace"
    const std::string position = op.value("position", "append");

    if (position == "prepend") {
        if (ctx.file.body.empty())
            ctx.file.body = canonical;
        else
            ctx.file.body = canonical + " " + ctx.file.body;
    } else if (position == "append") {
        if (ctx.file.body.empty())
            ctx.file.body = canonical;
        else
            ctx.file.body = ctx.file.body + " " + canonical;
    } else if (position == "replace") {
        ctx.file.body = canonical;
    } else {
        result.ok = false;
        result.error = "insert position must be 'prepend', 'append', or 'replace'";
        result.code = "INVALID_ARGUMENT";
        return result;
    }

    result.extra["canonical_notation"] = ctx.file.body;
    return result;
}

SongMutationService::OpResult
SongMutationService::applySetMeta(const nlohmann::json& op, OpContext& ctx)
{
    OpResult result;

    // Validate all fields before applying any of them (transactional)
    const bool hasBpm   = op.contains("bpm");
    const bool hasLabel = op.contains("label");
    const bool hasColor = op.contains("color");
    const bool hasSlot  = op.contains("slot");
    const bool hasBank  = op.contains("bank");

    if (!hasBpm && !hasLabel && !hasColor && !hasSlot && !hasBank) {
        result.ok = false;
        result.error = "set_meta requires at least one of: bpm, label, color, slot, bank";
        result.code = "INVALID_ARGUMENT";
        return result;
    }

    // Validate BPM
    if (hasBpm) {
        if (!op["bpm"].is_number()) {
            result.ok = false;
            result.error = "bpm must be a number";
            result.code = "INVALID_ARGUMENT";
            return result;
        }
        const double bpmVal = op["bpm"].get<double>();
        const auto err = validateBpm(bpmVal);
        if (!err.empty()) {
            result.ok = false;
            result.error = err;
            result.code = "INVALID_BPM";
            return result;
        }
    }

    // Validate label
    if (hasLabel) {
        if (!op["label"].is_string()) {
            result.ok = false;
            result.error = "label must be a string";
            result.code = "INVALID_ARGUMENT";
            return result;
        }
        const auto err = validateLabel(op["label"].get<std::string>());
        if (!err.empty()) {
            result.ok = false;
            result.error = err;
            result.code = "INVALID_LABEL";
            return result;
        }
    }

    // Validate color
    if (hasColor) {
        if (!op["color"].is_string()) {
            result.ok = false;
            result.error = "color must be a string";
            result.code = "INVALID_ARGUMENT";
            return result;
        }
        const auto err = validateColor(op["color"].get<std::string>());
        if (!err.empty()) {
            result.ok = false;
            result.error = err;
            result.code = "INVALID_COLOR";
            return result;
        }
    }

    // Validate slot
    if (hasSlot) {
        if (!op["slot"].is_string()) {
            result.ok = false;
            result.error = "slot must be a string";
            result.code = "INVALID_ARGUMENT";
            return result;
        }
        const auto err = validateSlotName(op["slot"].get<std::string>());
        if (!err.empty()) {
            result.ok = false;
            result.error = err;
            result.code = "INVALID_SLOT";
            return result;
        }
    }

    // Validate bank
    if (hasBank) {
        if (!op["bank"].is_string()) {
            result.ok = false;
            result.error = "bank must be a string";
            result.code = "INVALID_ARGUMENT";
            return result;
        }
    }

    // Check confirmation: changing slot when a pattern already exists
    // requires confirmation (it changes which runtime slot is targeted).
    if (hasSlot && ctx.hasExistingPattern && !op.value("confirm", false)) {
        ctx.needsConfirm = true;
    }

    // Apply all validated fields
    if (hasBpm)
        ctx.file.front.bpm = op["bpm"].get<double>();
    if (hasLabel)
        ctx.file.front.label = op["label"].get<std::string>();
    if (hasColor)
        ctx.file.front.color = op["color"].get<std::string>();
    if (hasSlot)
        ctx.file.front.slot = op["slot"].get<std::string>();
    if (hasBank)
        ctx.file.front.bank = op["bank"].get<std::string>();

    nlohmann::json meta;
    if (hasBpm)   meta["bpm"]   = *ctx.file.front.bpm;
    if (hasLabel) meta["label"] = *ctx.file.front.label;
    if (hasColor) meta["color"] = *ctx.file.front.color;
    if (hasSlot)  meta["slot"]  = *ctx.file.front.slot;
    if (hasBank)  meta["bank"]  = *ctx.file.front.bank;
    result.extra["meta"] = meta;

    return result;
}

SongMutationService::OpResult
SongMutationService::applyClearPattern(const nlohmann::json& op, OpContext& ctx)
{
    OpResult result;

    // Confirmation required if body is non-empty
    if (ctx.hasExistingPattern && !op.value("confirm", false)) {
        ctx.needsConfirm = true;
    }

    ctx.file.body.clear();

    result.extra["cleared"] = true;
    return result;
}

SongMutationService::OpResult
SongMutationService::applyDeleteSong(const nlohmann::json& op, OpContext& ctx)
{
    OpResult result;

    // delete_song always requires confirmation
    if (!op.value("confirm", false)) {
        result.ok = false;
        result.error = "delete_song requires 'confirm: true'";
        result.code = "REQUIRES_CONFIRMATION";
        result.extra["action"] = "delete_song";
        result.extra["song"] = ctx.songName;
        return result;
    }

    ctx.needsConfirm = true;

    nlohmann::json extra;
    extra["action"] = "delete_song";
    extra["song"] = ctx.songName;
    result.extra = extra;
    return result;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

nlohmann::json SongMutationService::editSong(std::string_view songFile,
                                               const nlohmann::json& ops)
{
    const std::string songName = std::string(songFile);

    // Validate ops array
    {
        const auto err = validateOpsArray(ops);
        if (!err.empty()) {
            auditLog("edit_song", songName, false, err);
            return {
                {"ok", false},
                {"cmd", "edit_song"},
                {"error", err},
                {"code", "INVALID_ARGUMENT"}
            };
        }
    }

    // Resolve song path
    auto [resolvedPath, pathErr] = resolveSongPath(songFile);
    if (!pathErr.empty()) {
        auditLog("edit_song", songName, false, pathErr);
        return {
            {"ok", false},
            {"cmd", "edit_song"},
            {"error", pathErr},
            {"code", "INVALID_PATH"}
        };
    }

    // Read + parse the file
    auto [contents, readErr] = readSongFile(resolvedPath);
    if (!readErr.empty()) {
        auditLog("edit_song", songName, false, readErr);
        return {
            {"ok", false},
            {"cmd", "edit_song"},
            {"error", readErr},
            {"code", "FILE_NOT_FOUND"}
        };
    }

    const auto parseResult = parseHathorFile(contents);
    HathorFile file;
    if (const auto* hf = std::get_if<HathorFile>(&parseResult)) {
        file = *hf;
    } else if (const auto* err = std::get_if<ParseFileError>(&parseResult)) {
        auditLog("edit_song", songName, false, "parse error at line " + std::to_string(err->line));
        return {
            {"ok", false},
            {"cmd", "edit_song"},
            {"error", "front-matter parse error: " + err->message},
            {"code", "PARSE_ERROR"},
            {"line", err->line}
        };
    } else {
        // Treat raw contents as body
        file.body = contents;
    }

    // Remember original state for rollback + confirmation checks
    const HathorFile originalFile = file;
    const bool hasExistingPattern = !file.body.empty();

    // -----------------------------------------------------------------------
    // Phase 1: Validate ALL operations before applying any
    // -----------------------------------------------------------------------

    // Make a copy for applying operations
    HathorFile workingCopy = file;

    OpContext ctx{
        workingCopy,
        songName,
        hasExistingPattern,
        false  // needsConfirm
    };

    nlohmann::json appliedOps = nlohmann::json::array();
    bool hasDelete = false;
    bool anyConfirmationRequired = false;
    bool anyConfirmationMissing = false;

    for (const auto& op : ops) {
        if (!op.is_object() || !op.contains("op") || !op["op"].is_string()) {
            auditLog("edit_song", songName, false, "operation missing 'op' field");
            return {
                {"ok", false},
                {"cmd", "edit_song"},
                {"error", "each operation must be an object with an 'op' string field"},
                {"code", "INVALID_ARGUMENT"}
            };
        }

        const std::string opName = op["op"].get<std::string>();
        OpResult opResult;

        // Reset per-op confirmation tracking
        ctx.needsConfirm = false;

        if (opName == "replace_pattern") {
            opResult = applyReplacePattern(op, ctx);
        } else if (opName == "insert") {
            opResult = applyInsert(op, ctx);
        } else if (opName == "set_meta") {
            opResult = applySetMeta(op, ctx);
        } else if (opName == "clear_pattern") {
            opResult = applyClearPattern(op, ctx);
        } else if (opName == "delete_song") {
            opResult = applyDeleteSong(op, ctx);
            if (opResult.ok)
                hasDelete = true;
        } else {
            opResult.ok = false;
            opResult.error = "unknown operation: " + opName;
            opResult.code = "UNKNOWN_OPERATION";
        }

        if (!opResult.ok) {
            auditLog("edit_song", songName, false,
                     "op '" + opName + "' failed: " + opResult.error);
            return {
                {"ok", false},
                {"cmd", "edit_song"},
                {"error", opResult.error},
                {"code", opResult.code},
                {"op", opName},
                {"applied", appliedOps},
                {"timestamp", isoTimestamp()}
            };
        }

        if (ctx.needsConfirm) {
            anyConfirmationRequired = true;
            // Check if confirmation was provided in the op
            if (op.contains("confirm") && op["confirm"].get<bool>()) {
                // Confirmation provided — OK
            } else {
                anyConfirmationMissing = true;
            }
        }

        // Record this operation in the applied list
        nlohmann::json opEntry = {
            {"op", opName},
            {"ok", true}
        };
        if (opResult.extra.is_object())
            opEntry.merge_patch(opResult.extra);
        appliedOps.push_back(std::move(opEntry));
    }

    // -----------------------------------------------------------------------
    // Phase 2: Check confirmation requirements
    // -----------------------------------------------------------------------

    if (anyConfirmationRequired && anyConfirmationMissing) {
        auditLog("edit_song", songName, false, "confirmation required but not provided");
        return {
            {"ok", false},
            {"cmd", "edit_song"},
            {"error", "confirmation required for this operation"},
            {"code", "REQUIRES_CONFIRMATION"},
            {"applied", appliedOps},
            {"timestamp", isoTimestamp()}
        };
    }

    // -----------------------------------------------------------------------
    // Phase 3: Persist the mutation atomically
    // -----------------------------------------------------------------------

    // If delete_song was requested, handle it specially
    if (hasDelete) {
        // Delete: remove the file
        std::error_code ec;
        if (!std::filesystem::remove(resolvedPath, ec)) {
            auditLog("edit_song", songName, false, "delete failed: " + ec.message());
            return {
                {"ok", false},
                {"cmd", "edit_song"},
                {"error", "failed to delete song file: " + ec.message()},
                {"code", "DELETE_FAILED"},
                {"applied", appliedOps},
                {"timestamp", isoTimestamp()}
            };
        }

        // Clear runtime slot if the file had one
        if (file.front.slot) {
            const auto slotErr = validateSlotName(*file.front.slot);
            if (slotErr.empty()) {
                const int idx = audio_.findOrAddSlot(*file.front.slot);
                if (idx >= 0) {
                    audio_.clearSlot(idx);
                }
            }
        }

        auditLog("edit_song", songName, true, "delete_song");

        return {
            {"ok", true},
            {"cmd", "edit_song"},
            {"song", songName},
            {"applied", appliedOps},
            {"timestamp", isoTimestamp()}
        };
    }

    // Normal mutation: write the file
    const std::string serialized = serialiseHathorFile(workingCopy);

    if (!atomicWriteHathorFile(resolvedPath, serialized)) {
        auditLog("edit_song", songName, false, "atomic write failed");
        return {
            {"ok", false},
            {"cmd", "edit_song"},
            {"error", "failed to write song file (rollback attempted)"},
            {"code", "WRITE_FAILED"},
            {"applied", appliedOps},
            {"timestamp", isoTimestamp()}
        };
    }

    // -----------------------------------------------------------------------
    // Phase 4: Update runtime state
    // -----------------------------------------------------------------------

    // If BPM changed, apply it
    if (workingCopy.front.bpm && (!file.front.bpm || *workingCopy.front.bpm != *file.front.bpm)) {
        applyBpm(*workingCopy.front.bpm);
    }

    // If slot or body changed, update the runtime slot
    const bool slotChanged = workingCopy.front.slot != file.front.slot;
    const bool bodyChanged = workingCopy.body != file.body;
    if ((slotChanged || bodyChanged) && workingCopy.front.slot) {
        updateRuntimeSlot(workingCopy);
    } else if (bodyChanged && !workingCopy.front.slot) {
        // No slot in front-matter but body changed — nothing to update in runtime
    }

    // If clear_pattern was applied and no slot in front-matter
    if (bodyChanged && workingCopy.body.empty() && file.front.slot) {
        const int idx = audio_.findOrAddSlot(*file.front.slot);
        if (idx >= 0) {
            audio_.clearSlot(idx);
        }
    }

    auditLog("edit_song", songName, true,
             "ops=" + std::to_string(ops.size()));

    nlohmann::json frontMatter;
    frontMatter["slot"]  = workingCopy.front.slot.has_value()
                             ? nlohmann::json(*workingCopy.front.slot)
                             : nlohmann::json(nullptr);
    frontMatter["bpm"]   = workingCopy.front.bpm.has_value()
                             ? nlohmann::json(*workingCopy.front.bpm)
                             : nlohmann::json(nullptr);
    frontMatter["label"] = workingCopy.front.label.has_value()
                             ? nlohmann::json(*workingCopy.front.label)
                             : nlohmann::json(nullptr);
    frontMatter["color"] = workingCopy.front.color.has_value()
                             ? nlohmann::json(*workingCopy.front.color)
                             : nlohmann::json(nullptr);
    frontMatter["bank"]  = workingCopy.front.bank.has_value()
                             ? nlohmann::json(*workingCopy.front.bank)
                             : nlohmann::json(nullptr);

    // Build referenced samples list from the body notation (cross-reference
    // with the real SampleBank, same pattern as ProjectReadFacade::getCurrentSong).
    nlohmann::json referencedSamples = nlohmann::json::array();
    {
        const auto tokens = hathor::tokenise(workingCopy.body);
        const auto sampleNames = bank_.listNames();
        std::set<std::string> seen(sampleNames.begin(), sampleNames.end());
        for (const auto& tok : tokens) {
            if (tok.kind == hathor::TokenKind::TK_ATOM) {
                const std::string atomStr(tok.text);
                if (seen.count(atomStr)) {
                    referencedSamples.push_back(nlohmann::json{
                        {"resource_id", "sample:" + atomStr},
                        {"name", atomStr},
                        {"type", "sample"}
                    });
                }
            }
        }
    }

    return {
        {"ok", true},
        {"cmd", "edit_song"},
        {"song", songName},
        {"file", resolvedPath.string()},
        {"applied", appliedOps},
        {"front_matter", frontMatter},
        {"body", workingCopy.body},
        {"referenced_samples", std::move(referencedSamples)},
        {"timestamp", isoTimestamp()}
    };
}

// ---------------------------------------------------------------------------
// handleEditSong — CLI dispatch entry point
// ---------------------------------------------------------------------------

nlohmann::json SongMutationService::handleEditSong(std::string_view songFile,
                                                     std::string_view rest)
{
    const auto trimmedRest = trimSV(rest);
    if (trimmedRest.empty()) {
        return {
            {"ok", false},
            {"cmd", "edit_song"},
            {"error", "missing operations JSON"}
        };
    }

    nlohmann::json ops;
    try {
        ops = nlohmann::json::parse(trimmedRest);
    } catch (const nlohmann::json::parse_error& e) {
        return {
            {"ok", false},
            {"cmd", "edit_song"},
            {"error", "invalid JSON in operations: " + std::string(e.what())},
            {"code", "JSON_PARSE_ERROR"}
        };
    }

    return editSong(songFile, ops);
}

// ---------------------------------------------------------------------------
// AI-10.3: change-set restore support (reuses AI-7 transactional I/O)
// ---------------------------------------------------------------------------

nlohmann::json SongMutationService::readSongContent(std::string_view songFile) const
{
    const std::string songName = std::string(songFile);

    auto [resolvedPath, pathErr] = resolveSongPath(songFile);
    if (!pathErr.empty())
        return {{"ok", false}, {"cmd", "read_song"}, {"error", pathErr},
                {"code", "INVALID_PATH"}};

    auto [contents, readErr] = readSongFile(resolvedPath);
    if (!readErr.empty())
        return {{"ok", false}, {"cmd", "read_song"}, {"error", readErr},
                {"code", "FILE_NOT_FOUND"}};

    return {{"ok", true}, {"cmd", "read_song"}, {"song", songName},
            {"content", contents}};
}

nlohmann::json SongMutationService::restoreSongFile(std::string_view songFile,
                                                    std::string_view content)
{
    const std::string songName = std::string(songFile);

    auditLog("restore_song", songName, true, "change-set rollback");

    // The restore content must itself parse — we never write a broken file
    // back over a good one (same safety bar as the edit_song verification).
    const auto parseResult = parseHathorFile(content);
    if (std::holds_alternative<ParseFileError>(parseResult)) {
        const auto* err = std::get_if<ParseFileError>(&parseResult);
        auditLog("restore_song", songName, false, "restore content did not parse");
        return {
            {"ok", false},
            {"cmd", "restore_song"},
            {"error", "restore content did not parse: " +
                      (err ? err->message : std::string("unknown error"))},
            {"code", "PARSE_ERROR"}
        };
    }

    auto [resolvedPath, pathErr] = resolveSongPath(songFile);
    if (!pathErr.empty())
        return {{"ok", false}, {"cmd", "restore_song"}, {"error", pathErr},
                {"code", "INVALID_PATH"}};

    // Reuse AI-7's own transactional atomic write with backup + rollback.
    if (!atomicWriteHathorFile(resolvedPath, std::string(content))) {
        auditLog("restore_song", songName, false, "atomic write failed");
        return {
            {"ok", false},
            {"cmd", "restore_song"},
            {"error", "failed to restore song file (rollback attempted)"},
            {"code", "WRITE_FAILED"}
        };
    }

    // Restore runtime state to match the restored content.
    HathorFile restored;
    if (const auto* hf = std::get_if<HathorFile>(&parseResult))
        restored = *hf;

    if (restored.front.bpm)
        applyBpm(*restored.front.bpm);

    if (restored.front.slot)
        updateRuntimeSlot(restored);
    else if (restored.body.empty() && !restored.front.slot) {
        // No slot, no body — nothing to restore in runtime.
    }

    auditLog("restore_song", songName, true, "restored");
    return {{"ok", true}, {"cmd", "restore_song"}, {"song", songName},
            {"file", resolvedPath.string()}};
}

} // namespace hathor::control
