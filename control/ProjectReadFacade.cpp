// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ProjectReadFacade.cpp — implementation of the AI-2 read-only service layer.
 *
 * Requirement references: Phase 2.5 H0, AI-2 §2–§7
 */

#include "ProjectReadFacade.hpp"

#include "hathor/MiniParser.hpp"
#include "hathor/MiniTokeniser.hpp"
#include "ChuckDiagnostics.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Schema helpers
// ---------------------------------------------------------------------------

namespace {

/// Produce an ISO-8601 UTC timestamp for diagnostic metadata.
std::string isoTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// makeDiagnostic
// ---------------------------------------------------------------------------

nlohmann::json ProjectReadFacade::makeDiagnostic(
    std::string_view severity,
    std::string_view code,
    std::string_view message,
    std::string_view resourceId,
    std::string_view source)
{
    return nlohmann::json{
        {"severity",     std::string(severity)},
        {"code",         std::string(code)},
        {"message",      std::string(message)},
        {"resource_id",  std::string(resourceId)},
        {"source",       std::string(source)},
        {"timestamp",    isoTimestamp()}
    };
}

// ---------------------------------------------------------------------------
// 2. inspect_project
// ---------------------------------------------------------------------------

nlohmann::json ProjectReadFacade::inspectProject() const
{
    const auto slots = audio_.listSlots();
    const auto samples = audio_.listSamples();

    // Determine "active" slots — those with a stored SlotState.
    nlohmann::json activeSlots = nlohmann::json::array();
    int currentSongSlot = -1;
    for (const auto& s : slots) {
        if (s.active) {
            activeSlots.push_back(nlohmann::json{
                {"resource_id", "slot:" + s.slotName},
                {"slot_index",  s.slotIndex},
                {"slot_name",   s.slotName},
                {"running",     s.running},
                {"event_count", s.eventCount},
                {"notation",    s.notation}
            });
            if (s.running && currentSongSlot == -1)
                currentSongSlot = s.slotIndex;
        }
    }

    // ChucK instruments — query via the Studio instruments directory.
    const std::string projectDir = audio_.currentProjectDir().string();
    const auto instruments = audio_.listChuckInstruments(
        audio_.currentProjectDir());

    nlohmann::json instrJson = nlohmann::json::array();
    for (const auto& inst : instruments) {
        std::string lifecycle;
        if (inst.renderedWavExists && inst.boundToSampleBank)
            lifecycle = "bound";        // rendered + registered in SampleBank
        else if (inst.renderedWavExists)
            lifecycle = "rendered";     // rendered .wav exists but not registered
        else if (inst.sourceCkExists)
            lifecycle = "source_only";  // .ck source exists, not yet baked
        else
            lifecycle = "unknown";

        instrJson.push_back(nlohmann::json{
            {"resource_id", "instrument:" + inst.name},
            {"name",             inst.name},
            {"source_ck_exists",   inst.sourceCkExists},
            {"rendered_wav_exists", inst.renderedWavExists},
            {"bound_to_sample_bank", inst.boundToSampleBank},
            {"lifecycle_state",    lifecycle}
        });
    }

    // "Songs" in Hathor's model = the set of open editor tabs, each identified
    // by its slot name.  There is no separate song-file abstraction yet.
    nlohmann::json songs = nlohmann::json::array();
    for (const auto& s : slots) {
        if (!s.slotName.empty()) {
            songs.push_back(nlohmann::json{
                {"resource_id", "slot:" + s.slotName},
                {"slot_index",  s.slotIndex},
                {"slot_name",   s.slotName},
                {"has_pattern", s.active}
            });
        }
    }

    std::string currentSong;
    if (currentSongSlot >= 0)
        currentSong = "slot:" + slots[static_cast<std::size_t>(currentSongSlot)].slotName;

    // Derive project name from the project directory filename.
    // Falls back to "hathor-project" when no meaningful directory is available.
    std::string projectName = "hathor-project";
    if (const auto dir = audio_.currentProjectDir(); !dir.empty()) {
        const auto stem = dir.filename().string();
        if (!stem.empty() && stem != "." && stem != "/")
            projectName = stem;
    }

    return nlohmann::json{
        {"ok",                 true},
        {"project_name",       projectName},
        {"project_dir",        projectDir},
        {"songs",              std::move(songs)},
        {"current_song",       currentSong.empty() ? nullptr : nlohmann::json(currentSong)},
        {"bpm",                audio_.getBpm()},
        {"active_slots",       std::move(activeSlots)},
        {"chuck_instruments",  std::move(instrJson)},
        {"samples_count",      static_cast<int>(samples.size())},
        {"sample_names",       samples},
        {"timestamp",          isoTimestamp()}
    };
}

// ---------------------------------------------------------------------------
// 3. get_current_song
// ---------------------------------------------------------------------------

nlohmann::json ProjectReadFacade::getCurrentSong() const
{
    const auto slots = audio_.listSlots();

    // Find the active slot that is "running" — this is the current song.
    // If none is running, fall back to the first active slot with a pattern.
    int currentIdx = -1;
    for (const auto& s : slots) {
        if (s.active && s.running) {
            currentIdx = s.slotIndex;
            break;
        }
    }
    if (currentIdx < 0) {
        for (const auto& s : slots) {
            if (s.active) {
                currentIdx = s.slotIndex;
                break;
            }
        }
    }

    nlohmann::json result;

    if (currentIdx < 0) {
        // No active song — return an empty state.
        result = nlohmann::json{
            {"ok",              true},
            {"source",          nullptr},
            {"active_patterns", nlohmann::json::array()},
            {"tempo",           audio_.getBpm()},
            {"selection",       nullptr},
            {"diagnostics",     nlohmann::json::array()},
            {"referenced_assets", nlohmann::json::array()},
            {"slot_count",      0},
            {"timestamp",       isoTimestamp()}
        };
        return result;
    }

    const auto& slot = slots[static_cast<std::size_t>(currentIdx)];
    const std::string slotId = "slot:" + slot.slotName;

    // Active patterns — the current slot's notation.
    nlohmann::json patterns = nlohmann::json::array();
    patterns.push_back(nlohmann::json{
        {"resource_id", slotId},
        {"slot_index",  slot.slotIndex},
        {"slot_name",   slot.slotName},
        {"notation",    slot.notation},
        {"event_count", slot.eventCount},
        {"running",     slot.running}
    });

    // Diagnostics for the current song's notation.
    nlohmann::json diagnostics = nlohmann::json::array();

    // Use the real parseMini() to get language diagnostics.
    auto parseResult = hathor::parseMini(slot.notation);
    if (std::holds_alternative<hathor::ParseError>(parseResult)) {
        const auto& err = std::get<hathor::ParseError>(parseResult);
        diagnostics.push_back(makeDiagnostic(
            "error", "PARSE_ERROR", err.message,
            slotId, "miniparser"));
    }
    // If parse succeeds, there are no language diagnostics for the notation.

    // Referenced assets — extract sample names from the pattern notation.
    // In mini-notation, samples are referenced as `s "name"` or bare atoms.
    // We use parseMini's tokeniser to find sample references without re-parsing.
    nlohmann::json referencedAssets = nlohmann::json::array();
    const auto tokens = hathor::tokenise(slot.notation);
    for (const auto& tok : tokens) {
        if (tok.kind == hathor::TokenKind::TK_ATOM) {
            // In Tidal/Hathor mini-notation, sample references are bare atoms
            // that match registered sample names.  Cross-reference with the
            // real SampleBank.
            const std::string atomStr(tok.text);
            const auto sampleNames = bank_.listNames();
            for (const auto& name : sampleNames) {
                if (name == atomStr) {
                    referencedAssets.push_back(nlohmann::json{
                        {"resource_id", "sample:" + name},
                        {"name",        name},
                        {"type",        "sample"}
                    });
                    break;
                }
            }
        }
    }

    result = nlohmann::json{
        {"ok",                 true},
        {"source",             slotId},
        {"active_patterns",    std::move(patterns)},
        {"tempo",              audio_.getBpm()},
        {"selection",          slot.slotName},
        {"diagnostics",        std::move(diagnostics)},
        {"referenced_assets",  std::move(referencedAssets)},
        {"slot_index",         slot.slotIndex},
        {"slot_count",         static_cast<int>(slots.size())},
        {"timestamp",          isoTimestamp()}
    };

    return result;
}

// ---------------------------------------------------------------------------
// 4. list_samples — from the real SampleBank
// ---------------------------------------------------------------------------

nlohmann::json ProjectReadFacade::listSamples() const
{
    nlohmann::json samples = nlohmann::json::array();

    // Source ALL sample information from the real SampleBank.
    // snapshotEntries() returns the full entry list (read-only, lock-free read
    // from the registration mutex-free path — safe on the control thread).
    const auto entries = bank_.snapshotEntries();

    for (const auto& entry : entries) {
        double duration = 0.0;
        if (entry.sampleRate > 0.0 && entry.numChannels > 0)
            duration = static_cast<double>(entry.data.size()) /
                       static_cast<double>(entry.numChannels) /
                       entry.sampleRate;

        samples.push_back(nlohmann::json{
            {"resource_id", "sample:" + entry.name + ":" + std::to_string(entry.index)},
            {"name",        entry.name},
            {"index",       entry.index},
            {"path",        entry.sourcePath.empty() ? nullptr
                                                      : nlohmann::json(entry.sourcePath)},
            {"duration_seconds", duration},
            {"channels",    entry.numChannels},
            {"sample_rate", entry.sampleRate}
        });
    }

    return nlohmann::json{
        {"ok",      true},
        {"samples", std::move(samples)},
        {"timestamp", isoTimestamp()}
    };
}

// ---------------------------------------------------------------------------
// 5. list_chuck_instruments — B8-K1/K2/K3/K4 instrument lifecycle
// ---------------------------------------------------------------------------

nlohmann::json ProjectReadFacade::listChuckInstruments(
    const std::filesystem::path& projectDir) const
{
    const auto instruments = audio_.listChuckInstruments(projectDir);

    nlohmann::json result = nlohmann::json::array();

    for (const auto& inst : instruments) {
        std::string lifecycle;
        if (inst.renderedWavExists && inst.boundToSampleBank)
            lifecycle = "bound";
        else if (inst.renderedWavExists)
            lifecycle = "rendered";
        else if (inst.sourceCkExists)
            lifecycle = "source_only";
        else
            lifecycle = "unknown";

        result.push_back(nlohmann::json{
            {"resource_id",      "instrument:" + inst.name},
            {"name",              inst.name},
            {"source_ck_exists",   inst.sourceCkExists},
            {"rendered_wav_exists", inst.renderedWavExists},
            {"bound_to_sample_bank", inst.boundToSampleBank},
            {"source_path",       inst.sourcePath.empty() ? nullptr
                                                          : nlohmann::json(inst.sourcePath)},
            {"rendered_path",     inst.renderedPath.empty() ? nullptr
                                                           : nlohmann::json(inst.renderedPath)},
            {"duration_seconds",  inst.durationSeconds},
            {"lifecycle_state",   lifecycle}
        });
    }

    return nlohmann::json{
        {"ok",            true},
        {"instruments",   std::move(result)},
        {"timestamp",     isoTimestamp()}
    };
}

// ---------------------------------------------------------------------------
// 6. get_diagnostics — real parser/compiler diagnostics
// ---------------------------------------------------------------------------

nlohmann::json ProjectReadFacade::getDiagnostics(
    std::string_view content,
    std::string_view sourceId,
    bool isChuck) const
{
    nlohmann::json diagnostics = nlohmann::json::array();

    if (isChuck) {
        // Route through the REAL ChucK compiler diagnostic path:
        // validateChuckSource() is the same function called by
        // ChuckCompiler::dispatcherLoop() during ck_compile (B4-K4).
        // When libchuck is linked, it will be replaced by ck.compileCode().
        const hathor::audio_worker::ChuckDiagnostic diag =
            hathor::audio_worker::validateChuckSource(
                std::string(content));

        if (!diag.ok) {
            diagnostics.push_back(nlohmann::json{
                {"severity",      "error"},
                {"code",          "CK_COMPILE_ERROR"},
                {"message",       diag.message},
                {"resource_id",   std::string(sourceId)},
                {"source",        "chuck_compiler"},
                {"timestamp",     isoTimestamp()},
                {"location", {
                    {"line",   diag.errorLine},
                    {"column", diag.errorColumn},
                    {"offset", 0},
                    {"text",   nullptr}
                }}
            });
        } else {
            diagnostics.push_back(makeDiagnostic(
                "info", "CK_OK", "ChucK source passed validation",
                sourceId, "chuck_compiler"));
        }
    } else {
        // Route through the REAL mini-notation parser:
        // parseMini() returns either CompiledPattern or ParseError.
        const auto result = hathor::parseMini(content);

        if (std::holds_alternative<hathor::ParseError>(result)) {
            const auto& err = std::get<hathor::ParseError>(result);
            diagnostics.push_back(nlohmann::json{
                {"severity",      "error"},
                {"code",          "PARSE_ERROR"},
                {"message",       err.message},
                {"resource_id",   std::string(sourceId)},
                {"source",        "miniparser"},
                {"timestamp",     isoTimestamp()},
                {"location", {
                    {"line",   0},
                    {"column", 0},
                    {"offset", static_cast<int64_t>(err.position)},
                    {"text",   nullptr}
                }}
            });
        } else {
            // Also check the tokeniser for TK_ERROR tokens (unrecognised chars).
            const auto tokens = hathor::tokenise(content);
            for (const auto& tok : tokens) {
                if (tok.kind == hathor::TokenKind::TK_ERROR) {
                    diagnostics.push_back(nlohmann::json{
                        {"severity",      "error"},
                        {"code",          "TOKENIZER_ERROR"},
                        {"message",       std::string("unrecognised character '") +
                                          std::string(tok.text) + "'"},
                        {"resource_id",   std::string(sourceId)},
                        {"source",        "minitokeniser"},
                        {"timestamp",     isoTimestamp()},
                        {"location", {
                            {"line",   0},
                            {"column", 0},
                            {"offset", static_cast<int64_t>(tok.pos)},
                            {"text",   std::string(tok.text)}
                        }}
                    });
                }
            }

            if (diagnostics.empty()) {
                diagnostics.push_back(makeDiagnostic(
                    "info", "MINI_PARSE_OK", "Mini-notation parsed successfully",
                    sourceId, "miniparser"));
            }
        }
    }

    return nlohmann::json{
        {"ok",           true},
        {"diagnostics",  std::move(diagnostics)},
        {"timestamp",    isoTimestamp()}
    };
}

// ---------------------------------------------------------------------------
// 7. get_audio_status
// ---------------------------------------------------------------------------

nlohmann::json ProjectReadFacade::getAudioStatus() const
{
    const auto audioStatus = audio_.getAudioStatus();
    const auto slotPlayback = audio_.listSlotPlayback();

    // Transport state
    nlohmann::json transport = {
        {"running",       audioStatus.running},
        {"bpm",           audioStatus.bpm},
        {"sample_rate",   audioStatus.sampleRate},
        {"master_gain",   audioStatus.masterGain},
        {"eq_preset",     audioStatus.eqPreset},
        {"sample_clock",  audioStatus.sampleClock},
        {"device_open",   audioStatus.deviceOpen},
        {"active_renders", audioStatus.activeRenders}
    };

    // Per-slot playback state
    nlohmann::json slotsJson = nlohmann::json::array();
    for (const auto& sp : slotPlayback) {
        if (sp.slotName.empty() && !sp.hasPattern)
            continue;  // skip unregistered slots

        nlohmann::json slotJson = {
            {"slot_index",  sp.slotIndex},
            {"slot_name",   sp.slotName},
            {"running",     sp.running},
            {"has_pattern", sp.hasPattern}
        };
        if (!sp.notation.empty())
            slotJson["notation"] = sp.notation;

        slotsJson.push_back(std::move(slotJson));
    }

    // Worker runtime status (B4-K3)
    const auto vmStatus = audio_.getVmStatus(0);
    nlohmann::json worker = {
        {"alive",       vmStatus.hasWorker},
        {"generation",  vmStatus.generation}
    };

    return nlohmann::json{
        {"ok",         true},
        {"transport",  std::move(transport)},
        {"slots",      std::move(slotsJson)},
        {"worker",     std::move(worker)},
        {"timestamp",  isoTimestamp()}
    };
}

} // namespace hathor::control
