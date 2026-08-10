// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ProjectReadFacade.hpp — canonical read-only service layer for AI introspection.
 *
 * AI-2 (Phase 2.5 H0): provides six read-only operations that allow an AI caller
 * to understand the current Hathor project state through semantic application
 * data rather than raw filesystem traversal.
 *
 *   AI caller
 *       ↓
 *   ProjectReadFacade  ← this layer (canonical service contract)
 *       ↓
 *   AudioEngineFacade + SampleBank + engine subsystems
 *
 * All operations are PURELY READ-ONLY:
 *   - No edits to .hathor files
 *   - No edits to .ck files
 *   - No asset creation/deletion/overwrite
 *   - No project configuration mutation
 *   - No play/stop
 *   - No ChucK VM creation/destruction
 *   - No audio rendering
 *   - No SampleBank modification
 *
 * Schemas use the canonical conventions established in AI-1:
 *   - resource_id:  stable string identifier (e.g. "slot:d0", "instrument:acid_bass")
 *   - optional:     omitted key or JSON null when not applicable
 *   - timestamp:    ISO-8601 UTC string
 *   - diagnostics:  array of {severity, code, message, resource_id, source, location}
 *   - errors:       {ok: false, error: "...", code: "..."}
 *   - collections:  arrays with stable element schemas
 *   - lifecycle:    string state names matching the underlying subsystem
 *
 * Requirement references: Phase 2.5 H0, AI-2
 */

#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <string_view>

// Forward declarations — full headers included in the .cpp.
// The nested types (SlotInfo, VmStatus, etc.) are members of
// AudioEngineFacade, so we must include the facade header here.
// It is JUCE-free (only includes SlotState.hpp, MasterEq.hpp, etc.).
#include "../app/AudioEngineFacade.hpp"
#include "../app/SampleBank.hpp"

namespace hathor::control {

/**
 * ProjectReadFacade — JUCE-free, read-only introspection service for AI-2.
 *
 * Constructed with references to the real AudioEngineFacade (which AudioEngine
 * satisfies) and the real SampleBank.  Every operation delegates to the real
 * underlying subsystem; no shadowing cache is maintained.
 *
 * The facade is safe to call from any thread that is not the JUCE audio
 * callback thread (the underlying AudioEngineFacade methods use atomics or
 * lock-free reads, except listChuckInstruments which does filesystem I/O
 * on the main thread).
 */
class ProjectReadFacade {
public:
    ProjectReadFacade(AudioEngineFacade& audio, SampleBank& bank)
        : audio_(audio), bank_(bank) {}

    // -----------------------------------------------------------------------
    // 2. inspect_project — semantic project representation
    // -----------------------------------------------------------------------

    /// Returns a semantic description of the current project:
    ///   {ok, project_name, project_dir, songs, current_song, bpm, active_slots,
    ///    chuck_instruments, samples_count, sample_names}
    ///
    /// "songs" is currently a single-entry list (the open project buffer set).
    /// "current_song" is the active tab's slot name or null.
    nlohmann::json inspectProject() const;

    // -----------------------------------------------------------------------
    // 3. get_current_song — semantic state of the active song
    // -----------------------------------------------------------------------

    /// Returns the semantic state of the currently active song:
    ///   {ok, source, active_patterns, tempo, selection, diagnostics,
    ///    referenced_assets, slot_count}
    nlohmann::json getCurrentSong() const;

    // -----------------------------------------------------------------------
    // 4. list_samples — from the real SampleBank
    // -----------------------------------------------------------------------

     /// Returns all samples from the real SampleBank:
     ///   {ok, samples: [{name, path, duration_seconds, channels, sample_rate, index}]}
     nlohmann::json listSamples() const;

     // -----------------------------------------------------------------------
     // 4b. list_assets — combined project asset inventory
     // -----------------------------------------------------------------------

     /// Returns a combined view of all project assets: songs (.hathor), ChucK
     /// instruments (baked .ck + .wav), and samples from the SampleBank.
     /// This is the canonical Project-namespace asset read endpoint (AI-2).
     ///   {ok, songs: [...], chuck_instruments: [...], samples: [...]}
     nlohmann::json listAssets() const;

    // -----------------------------------------------------------------------
    // 5. list_chuck_instruments — B8-K1/K2/K3/K4 instrument lifecycle
    // -----------------------------------------------------------------------

    /// Returns ChucK instrument inventory with lifecycle state:
    ///   {ok, instruments: [{name, resource_id, source_ck_exists,
    ///    rendered_wav_exists, bound_to_sample_bank, source_path,
    ///    rendered_path, duration_seconds, lifecycle_state}]}
    ///
    /// @param projectDir  The current project directory for Studio asset paths.
    nlohmann::json listChuckInstruments(const std::filesystem::path& projectDir) const;

    // -----------------------------------------------------------------------
    // 6. get_diagnostics — real parser/compiler diagnostics
    // -----------------------------------------------------------------------

    /// Returns structured diagnostics from real language/compiler infrastructure:
    ///   {ok, diagnostics: [{severity, code, message, resource_id, source,
    ///    location: {file, line, column, offset, text}}]}
    ///
    /// @param content   Source text to diagnose.
    /// @param sourceId  Resource identifier (e.g. "slot:d0" or "file:bd.hathor").
    /// @param isChuck   true if content is ChucK (.ck), false if mini-notation.
    nlohmann::json getDiagnostics(
        std::string_view content,
        std::string_view sourceId,
        bool isChuck) const;

    // -----------------------------------------------------------------------
    // 7. get_audio_status — current playback/runtime state
    // -----------------------------------------------------------------------

    /// Returns the current audio/runtime state:
    ///   {ok, transport: {running, bpm, sample_rate, master_gain, eq_preset,
    ///    sample_clock, device_open, active_renders},
    ///    slots: [{slot_index, slot_name, running, has_pattern, notation}],
    ///    worker: {alive, generation, status}}
    nlohmann::json getAudioStatus() const;

private:
    AudioEngineFacade& audio_;
    SampleBank& bank_;

    // Helper: produce a diagnostic JSON object.
    static nlohmann::json makeDiagnostic(
        std::string_view severity,   // "error" | "warning" | "info"
        std::string_view code,       // e.g. "PARSE_ERROR", "TOKENIZER_ERROR"
        std::string_view message,
        std::string_view resourceId,
        std::string_view source);
};

} // namespace hathor::control
