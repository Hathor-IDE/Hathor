// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * SongMutationService.hpp — AI-7 structured, safe song mutation service.
 *
 * Provides a canonical application-layer service for structured mutation of
 * .hathor song files through a single `edit_song` entry point.  The service
 * applies a batch of structured operations atomically: all validations run
 * first, and only if every operation passes validation is the file written.
 * On any failure during the write, the original file is restored from a
 * backup (rollback).
 *
 * Operations:
 *   replace_pattern  — replace the body's mini-notation (validated via parseMini)
 *   insert           — insert/prepend/extend the body's mini-notation
 *   set_meta         — modify front-matter (slot, bpm, bank, label, color)
 *   clear_pattern    — clear the body notation and clear the runtime slot
 *   delete_song      — delete the .hathor file (requires confirmation)
 *
 * Capability model (AI-1):
 *   - Persistent mutation: writes to .hathor files.
 *   - Confirmation required for: replace_pattern (when slot has existing
 *     pattern), clear_pattern, delete_song.
 *   - Audit logging to stderr on every mutation.
 *   - Never performs arbitrary filesystem writes — only .hathor files within
 *     the project directory.
 *
 * Transactional model:
 *   1. Read + parse the target .hathor file.
 *   2. Validate every operation against the parsed model (parseMini for patterns).
 *   3. Apply all operations to the in-memory HathorFile model.
 *   4. Serialise and atomically write (temp file + rename).
 *   5. On any failure during step 4, restore the backup.
 *   6. Update runtime state (slots, bpm) after successful persistence.
 *
 * Requirement references: AI-1 §1, AI-7 §1–§12, PROGRAM.md Phase 2.5
 */

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

#include "../app/AudioEngineFacade.hpp"
#include "../app/SampleBank.hpp"

namespace hathor::control {

/**
 * SongMutationService — canonical structured mutation service for .hathor songs.
 *
 * Constructed with references to the real AudioEngineFacade and SampleBank.
 * All filesystem operations are confined to the project directory and the
 * .hathor file extension.
 */
class SongMutationService {
public:
    /**
     * Construct the song mutation service.
     *
     * @param audio  AudioEngineFacade — provides slot management, BPM, playback.
     * @param bank   SampleBank — for referenced asset lookups.
     */
    SongMutationService(AudioEngineFacade& audio, SampleBank& bank) noexcept
        : audio_(audio), bank_(bank) {}

    ~SongMutationService() = default;
    SongMutationService(const SongMutationService&)            = delete;
    SongMutationService& operator=(const SongMutationService&) = delete;

    // -----------------------------------------------------------------------
    // AI-7 §2: edit_song — structured, transactional song mutation
    // -----------------------------------------------------------------------

    /**
     * Apply a batch of structured operations to a .hathor song file.
     *
     * All operations are validated before any mutation occurs.  If any
     * operation fails validation, no changes are applied and an error JSON
     * is returned.
     *
     * @param songFile  Song file path (resolved relative to the project dir
     *                   if it is a bare filename or a path without directory
     *                   separators).
     * @param ops       JSON array of operation objects.  Each object has an
     *                   "op" field selecting the operation, plus operation-
     *                   specific fields.
     * @return JSON result: {ok, ...} on success or {ok:false, error, code, ...}
     *         on failure.  Includes "applied" array with per-op results.
     *
     * Operations:
     *   replace_pattern  — {op, slot?, notation, confirm?}
     *   insert           — {op, notation, position, confirm?}
     *   set_meta         — {op, slot?, bpm?, label?, color?, bank?, confirm?}
     *   clear_pattern    — {op, slot?, confirm?}
     *   delete_song      — {op, confirm}
     */
    nlohmann::json editSong(std::string_view songFile, const nlohmann::json& ops);

    // -----------------------------------------------------------------------
    // AI-7 §4: handleEditSong — CLI/command dispatch entry point
    // -----------------------------------------------------------------------

    /**
     * Handle an `edit_song` command line.
     *
     * Format: edit_song <songFile> <opsJson>
     *
     * @param songFile  The song file name or path.
     * @param rest      Everything after the song file name — parsed as a JSON
     *                   array of operation objects.
     */
    nlohmann::json handleEditSong(std::string_view songFile,
                                  std::string_view rest);

private:
    // -----------------------------------------------------------------------
    // Operation handlers (validate + apply to in-memory model)
    // -----------------------------------------------------------------------

    struct OpResult {
        bool               ok      = true;
        std::string        error;
        std::string        code;
        nlohmann::json     extra;  // operation-specific result data
    };

    struct OpContext {
        HathorFile&        file;       // the in-memory song model being mutated
        const std::string& songName;   // display name for responses
        bool               hasExistingPattern;  // body was non-empty before this op
        bool               needsConfirm;  // set true if a confirmation-gated op was applied
    };

    /// Validate and apply a single replace_pattern operation.
    OpResult applyReplacePattern(const nlohmann::json& op, OpContext& ctx);

    /// Validate and apply a single insert operation.
    OpResult applyInsert(const nlohmann::json& op, OpContext& ctx);

    /// Validate and apply a single set_meta operation.
    OpResult applySetMeta(const nlohmann::json& op, OpContext& ctx);

    /// Validate and apply a single clear_pattern operation.
    OpResult applyClearPattern(const nlohmann::json& op, OpContext& ctx);

    /// Validate a delete_song operation (the actual deletion happens after
    /// all ops in the batch are validated successfully).
    OpResult applyDeleteSong(const nlohmann::json& op, OpContext& ctx);

    // -----------------------------------------------------------------------
    // Path resolution
    // -----------------------------------------------------------------------

    /**
     * Resolve a song file path.  If @p songFile is a bare filename (no path
     * separators), it is resolved relative to the project directory.
     * If it contains separators, it is treated as relative to the project
     * directory as well, but path traversal ("..") is rejected.
     *
     * @return A pair of {resolved_path, error_message}.  error_message is
     *         empty on success.
     */
    std::pair<std::filesystem::path, std::string>
    resolveSongPath(std::string_view songFile) const noexcept;

    // -----------------------------------------------------------------------
    // Atomic file I/O with rollback
    // -----------------------------------------------------------------------

    /**
     * Read and parse a .hathor file from disk.
     * Returns {HathorFile, ""} on success, {"", error_message} on failure.
     */
    std::pair<std::string, std::string>
    readSongFile(const std::filesystem::path& path) const noexcept;

    /**
     * Atomically write a .hathor file with backup + rollback.
     *
     * 1. Copy existing file to <path>.bak (if it exists).
     * 2. Write new content to <path>.tmp.
     * 3. Verify the temp file by re-parsing it.
     * 4. Rename temp → final.
     * 5. On any failure, restore from backup.
     *
     * @return true on success, false on failure.
     */
    bool atomicWriteHathorFile(const std::filesystem::path& path,
                               const std::string& content) const noexcept;

    // -----------------------------------------------------------------------
    // Runtime state updates
    // -----------------------------------------------------------------------

    /**
     * After a successful persisted edit, update the runtime slot state
     * for any slot referenced by the song file's front matter.
     */
    void updateRuntimeSlot(const HathorFile& file) noexcept;

    /**
     * Apply BPM from front-matter to the audio engine (if changed).
     */
    void applyBpm(double bpm) noexcept;

    // -----------------------------------------------------------------------
    // Validation helpers
    // -----------------------------------------------------------------------

    /// Validate a slot name (e.g. "d0", "d1").  Returns empty string on OK.
    static std::string validateSlotName(std::string_view name) noexcept;

    /// Validate a BPM value.  Returns empty string on OK.
    static std::string validateBpm(double bpm) noexcept;

    /// Validate a label (≤ 64 chars, non-empty).  Returns empty string on OK.
    static std::string validateLabel(std::string_view label) noexcept;

    /// Validate a color (CSS hex like "#e05a5a").  Returns empty string on OK.
    static std::string validateColor(std::string_view color) noexcept;

    /// Validate that an ops array is non-empty and well-formed.
    static std::string validateOpsArray(const nlohmann::json& ops) noexcept;

    // -----------------------------------------------------------------------
    // Audit logging
    // -----------------------------------------------------------------------

    /// Write an audit entry to stderr (canonical pattern from RenderService).
    void auditLog(std::string_view action,
                  std::string_view target,
                  bool success,
                  std::string_view detail = "") const noexcept;

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    AudioEngineFacade& audio_;
    SampleBank&       bank_;
};

} // namespace hathor::control
