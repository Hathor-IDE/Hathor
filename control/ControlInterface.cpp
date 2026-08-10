// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ControlInterface.cpp — command dispatch and stdin reader loop.
 *
 * Requirements: 12.1–12.5, 13.5, 14.1–14.6, 15.1–15.3, 16.2, 16.5
 */

#include "ControlInterface.hpp"
#include "Commands.hpp"
#include "ProjectReadFacade.hpp"
#include "WorkerThread.hpp"
#include "ChuckSessionService.hpp"
#include "RenderService.hpp"
#include "SongMutationService.hpp"

// App headers (available when compiled as part of the hathor executable;
// use paths relative to this file (control/) so the includes resolve
// even without app/ in hathor-control's include_directories).
#include "../app/AudioEngineFacade.hpp"
#include "../app/SampleBank.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Global mutex that serialises all stdout writes (defined here, declared in
// Commands.hpp so WorkerThread.cpp can acquire it too).
// ---------------------------------------------------------------------------
std::mutex g_stdoutMutex;

// ---------------------------------------------------------------------------
// Impl — holds the WorkerThread so it stays out of the public header.
// ---------------------------------------------------------------------------
struct ControlInterface::Impl {
    WorkerThread worker;

    explicit Impl(AudioEngineFacade& audio)
        : worker(audio, [](nlohmann::json j) { respond(j); })
    {}
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ControlInterface::ControlInterface(AudioEngineFacade& audio, SampleBank& bank)
    : audio_(audio)
    , bank_(bank)
    , readFacade_(std::make_unique<ProjectReadFacade>(audio, bank))
    , songMutationService_(std::make_unique<SongMutationService>(audio, bank))
    , impl_(new Impl(audio))
{
    // AI-5: Create the ChucK session service if the audio engine supports
    // B4 per-tab ChucK VM operations (hasWorker() indicates the B4-K3
    // architecture is available).
    // The AudioWorkerManager is accessed through the AudioEngineFacade
    // in the real application; for now, we create the service eagerly
    // and it will gracefully handle the case where the worker is not running.
}

ControlInterface::~ControlInterface()
{
    delete impl_;
}

// ---------------------------------------------------------------------------
// Helpers: whitespace trimming and token splitting
// ---------------------------------------------------------------------------

namespace {

/// Returns true if c is ASCII whitespace.
inline bool isWS(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// Trim leading and trailing whitespace from sv.
std::string_view trim(std::string_view sv) noexcept
{
    while (!sv.empty() && isWS(sv.front())) sv.remove_prefix(1);
    while (!sv.empty() && isWS(sv.back()))  sv.remove_suffix(1);
    return sv;
}

/**
 * Split sv on the first run of whitespace.
 *
 * Returns:
 *   first  — the token before the whitespace
 *   second — everything after the whitespace run (may be empty)
 */
std::pair<std::string_view, std::string_view>
splitFirst(std::string_view sv) noexcept
{
    // Find end of first token
    std::size_t i = 0;
    while (i < sv.size() && !isWS(sv[i])) ++i;
    std::string_view first = sv.substr(0, i);

    // Skip whitespace run
    while (i < sv.size() && isWS(sv[i])) ++i;
    std::string_view rest = sv.substr(i);

    return {first, rest};
}

/// Per-thread response sink used by dispatchWithCallback(). When non-empty,
/// every handler routes its JSON result here instead of to stdout, so a caller
/// (e.g. the MCP socket accept loop, or UI eval) can capture the response.
/// Thread-local: a shared global would race between the worker thread pool and
/// the ControlInterface caller, and a member would need its own mutex.
static thread_local std::function<void(nlohmann::json)> g_responseSink;

/// Route @p j to the current thread's response sink, or to stdout when no
/// sink is active (the stdin/stdout CLI path, Req 12.2).
static void emitResponse(const nlohmann::json& j)
{
    if (g_responseSink)
        g_responseSink(j);
    else
        respond(j);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// dispatch() — O(n) command routing
// ---------------------------------------------------------------------------

void ControlInterface::dispatch(std::string_view rawLine)
{
    // Record the time as soon as we start processing the line so that ping
    // can report accurate end-to-end latency (Req 14.6).
    const auto receiveTime = std::chrono::steady_clock::now();

    std::string_view line = trim(rawLine);
    if (line.empty()) return;

    auto [cmd, rest] = splitFirst(line);

    if (cmd == "ping") {
        handlePing(receiveTime);
    } else if (cmd == "play") {
        handlePlay();
    } else if (cmd == "stop") {
        handleStop();
    } else if (cmd == "quit") {
        handleQuit();
    } else if (cmd == "list-patterns") {
        handleListPatterns();
    } else if (cmd == "list-samples") {
        handleListSamples();
    } else if (cmd == "bpm") {
        handleBpm(trim(rest));
    } else if (cmd == "set-gain") {
        handleSetGain(trim(rest));
    } else if (cmd == "set-eq-preset") {
        handleSetEqPreset(trim(rest));
    } else if (cmd == "clear-pattern") {
        handleClearPattern(trim(rest));
    } else if (cmd == "set-pattern") {
        // split rest into slot + notation (everything after second ws token)
        auto [slot, notation] = splitFirst(rest);
        handleSetPattern(slot, notation);
    } else if (cmd == "slot-play") {
        handleSlotPlayStop(trim(rest), /*start=*/true);
    } else if (cmd == "slot-stop") {
        handleSlotPlayStop(trim(rest), /*start=*/false);
    } else if (cmd == "inspect_project" ||
               cmd == "get_current_song" ||
               cmd == "list_assets" ||
               cmd == "list_samples" ||
               cmd == "list_chuck_instruments" ||
               cmd == "get_diagnostics" ||
               cmd == "get_audio_status") {
        // AI-2 read-only introspection commands — route through the canonical
        // ProjectReadFacade service layer (Phase 2.5 H0).
        handleReadOnlyCommand(cmd, rest);
    } else if (cmd == "create_chuck_session" ||
                cmd == "get_chuck_session" ||
                cmd == "compile_chuck" ||
                cmd == "audition_chuck" ||
                cmd == "stop_chuck" ||
                cmd == "get_chuck_job" ||
                cmd == "cancel_chuck_job") {
        // AI-5: ChucK session lifecycle commands.
        handleChuckSessionCommand(cmd, rest);
    } else if (cmd == "render_chuck" ||
                 cmd == "get_job_status" ||
                 cmd == "commit_rendered_asset" ||
                 cmd == "cancel_render_job" ||
                 cmd == "list_render_jobs") {
        // AI-6: Background render with explicit commit boundary.
        handleRenderCommand(cmd, rest);
    } else if (cmd == "edit_song") {
        // AI-7: Structured song mutation (persistent, confirmed).
        // Format: edit_song <songFile> <opsJson>
        auto [songFile, opsJson] = splitFirst(rest);
        handleEditSong(songFile, trim(opsJson));
    } else {
        emitResponse({
            {"ok",    false},
            {"error", "unknown command"},
            {"cmd",   std::string(cmd)}
        });
    }
}

// ---------------------------------------------------------------------------
// enqueueSetPattern() — UI eval path (Req 23.7)
// ---------------------------------------------------------------------------

void ControlInterface::enqueueSetPattern(
    const std::string& slotName,
    const std::string& notation,
    std::function<void(nlohmann::json)> onComplete)
{
    impl_->worker.enqueue(CompileJob{slotName, notation, std::move(onComplete)});
}

// ---------------------------------------------------------------------------
// run() — blocking stdin reader loop (Req 12.1, 16.2)
// ---------------------------------------------------------------------------

void ControlInterface::run()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        dispatch(line);
    }
    // EOF on stdin — clean shutdown (Req 16.2)
    std::exit(0);
}

// ---------------------------------------------------------------------------
// dispatchWithCallback() — non-stdout response delivery (Req 23.7)
// ---------------------------------------------------------------------------

void ControlInterface::dispatchWithCallback(
    std::string_view line,
    std::function<void(nlohmann::json)> onResult)
{
    // Install this thread's response sink so the command handlers deliver
    // their JSON result to onResult instead of stdout.  Restored on exit.
    // For set-pattern the result is delivered later, on the WorkerThread,
    // via the per-job callback captured in handleSetPattern().
    g_responseSink = std::move(onResult);
    dispatch(line);
    g_responseSink = nullptr;
}

// ---------------------------------------------------------------------------
// dispatchSlotPlayStop() — B1 per-tab Play/Stop convenience wrapper
// ---------------------------------------------------------------------------

void ControlInterface::dispatchSlotPlayStop(
    const std::string& slotName,
    bool start,
    std::function<void(nlohmann::json)> onResult)
{
    const std::string cmd = std::string(start ? "slot-play " : "slot-stop ") + slotName;
    dispatchWithCallback(cmd, std::move(onResult));
}

// ---------------------------------------------------------------------------
// handlePing() — Req 14.6
// ---------------------------------------------------------------------------

void ControlInterface::handlePing(std::chrono::steady_clock::time_point receiveTime)
{
    const auto now = std::chrono::steady_clock::now();
    const double latencyMs =
        std::chrono::duration<double, std::milli>(now - receiveTime).count();

    emitResponse({
        {"ok",         true},
        {"cmd",        "ping"},
        {"latency_ms", latencyMs}
    });
}

// ---------------------------------------------------------------------------
// handlePlay() / handleStop() — Req 14.1, 14.2
// ---------------------------------------------------------------------------

void ControlInterface::handlePlay()
{
    audio_.play();
    emitResponse({{"ok", true}, {"cmd", "play"}});
}

void ControlInterface::handleStop()
{
    audio_.stop();
    emitResponse({{"ok", true}, {"cmd", "stop"}});
}

// ---------------------------------------------------------------------------
// handleBpm() — Req 14.3, 14.4
// ---------------------------------------------------------------------------

void ControlInterface::handleBpm(std::string_view arg)
{
    if (arg.empty()) {
        emitResponse({
            {"ok",    false},
            {"cmd",   "bpm"},
            {"error", "missing BPM argument"}
        });
        return;
    }

    // Parse as double — use std::stod via a temporary std::string.
    double bpm = 0.0;
    try {
        std::size_t pos = 0;
        bpm = std::stod(std::string(arg), &pos);
        if (pos != arg.size()) {
            throw std::invalid_argument("trailing characters");
        }
    } catch (...) {
        emitResponse({
            {"ok",    false},
            {"cmd",   "bpm"},
            {"error", "invalid BPM value — expected a number"}
        });
        return;
    }

    if (bpm < 20.0 || bpm > 400.0) {
        emitResponse({
            {"ok",    false},
            {"cmd",   "bpm"},
            {"error", "BPM out of range [20, 400]"},
            {"value", bpm}
        });
        return;
    }

    audio_.setBpm(bpm);
    emitResponse({
        {"ok",  true},
        {"cmd", "bpm"},
        {"bpm", bpm}
    });
}

// ---------------------------------------------------------------------------
// handleSetGain() — Req 26.7, 26.8
// ---------------------------------------------------------------------------

void ControlInterface::handleSetGain(std::string_view arg)
{
    if (arg.empty()) {
        emitResponse({
            {"ok",    false},
            {"cmd",   "set-gain"},
            {"error", "missing gain argument"}
        });
        return;
    }

    // Parse as float. std::from_chars for floating-point is not available on
    // Apple Clang libc++ (only integers); use std::stof instead.
    float val = 0.f;
    try {
        std::size_t pos = 0;
        val = std::stof(std::string(arg), &pos);
        if (pos != arg.size()) {
            throw std::invalid_argument("trailing characters");
        }
    } catch (...) {
        emitResponse({
            {"ok",    false},
            {"cmd",   "set-gain"},
            {"error", "invalid value"}
        });
        return;
    }

    // Clamp to [0.0, 2.0] — out-of-range values are clamped, not rejected (Req 26.8).
    const float clamped = std::clamp(val, 0.f, 2.f);
    audio_.setMasterGain(clamped);

    emitResponse({
        {"ok",   true},
        {"cmd",  "set-gain"},
        {"gain", clamped}
    });
}

// ---------------------------------------------------------------------------
// handleSetEqPreset() — B7-K2 master-bus preset EQ
// ---------------------------------------------------------------------------
//
// Preset names: flat, bass-boost, vocal, bright
// This is called on the control/worker thread (not the audio thread).
// setMasterEqPreset() computes the complete replacement filter state and
// publishes it atomically — no allocation or mutex in the audio callback.
// ---------------------------------------------------------------------------

void ControlInterface::handleSetEqPreset(std::string_view arg)
{
    hathor::EqPreset preset;

    if (arg == "flat") {
        preset = hathor::EqPreset::Flat;
    } else if (arg == "bass-boost") {
        preset = hathor::EqPreset::BassBoost;
    } else if (arg == "vocal") {
        preset = hathor::EqPreset::Vocal;
    } else if (arg == "bright") {
        preset = hathor::EqPreset::Bright;
    } else {
        emitResponse({
            {"ok",    false},
            {"cmd",   "set-eq-preset"},
            {"error", "unknown preset; valid: flat, bass-boost, vocal, bright"},
            {"value", std::string(arg)}
        });
        return;
    }

    audio_.setMasterEqPreset(preset);

    emitResponse({
        {"ok",      true},
        {"cmd",     "set-eq-preset"},
        {"preset",  std::string(arg)}
    });
}

// ---------------------------------------------------------------------------
// handleSetPattern() — Req 11.5, 13.1–13.4
// ---------------------------------------------------------------------------

void ControlInterface::handleSetPattern(std::string_view slot,
                                         std::string_view notation)
{
    if (slot.empty()) {
        emitResponse({
            {"ok",    false},
            {"cmd",   "set-pattern"},
            {"error", "missing slot name"}
        });
        return;
    }

    if (notation.empty()) {
        emitResponse({
            {"ok",    false},
            {"cmd",   "set-pattern"},
            {"slot",  std::string(slot)},
            {"error", "missing notation string"}
        });
        return;
    }

    // Enqueue on the worker thread (non-blocking) — Req 11.5.
    //
    // When a per-thread response sink is active (dispatchWithCallback, e.g.
    // the MCP socket accept loop / UI eval path), pass it as the per-job
    // callback so the worker delivers the result to the caller instead of
    // stdout. Otherwise fall back to the WorkerThread's global onComplete_
    // (the stdin/stdout CLI path).
    if (g_responseSink)
        impl_->worker.enqueue(CompileJob{std::string(slot),
                                          std::string(notation),
                                          g_responseSink});
    else
        impl_->worker.enqueue(CompileJob{std::string(slot),
                                          std::string(notation),
                                          nullptr});
    // Response is sent asynchronously (worker onComplete / g_responseSink).
}

// ---------------------------------------------------------------------------
// handleClearPattern() — Req 15.2, 15.3
// ---------------------------------------------------------------------------

void ControlInterface::handleClearPattern(std::string_view slotSV)
{
    if (slotSV.empty()) {
        emitResponse({
            {"ok",    false},
            {"cmd",   "clear-pattern"},
            {"error", "missing slot name"}
        });
        return;
    }

    const std::string slotName(slotSV);
    const int idx = audio_.findOrAddSlot(slotName);

    // findOrAddSlot returns -1 if the table is full AND the slot is unknown.
    // But if the slot was never registered, it would have been newly added
    // (consuming a slot entry).  The spec says: error if slot doesn't exist.
    //
    // Strategy: if idx < 0 (table full, name unknown) → not found.
    // If idx >= 0 but loadSlot(idx) == nullptr → slot was cleared or never set.
    if (idx < 0 || audio_.loadSlot(idx) == nullptr) {
        emitResponse({
            {"ok",    false},
            {"cmd",   "clear-pattern"},
            {"slot",  slotName},
            {"error", "slot does not exist or is empty"}
        });
        return;
    }

    audio_.clearSlot(idx);
    emitResponse({
        {"ok",   true},
        {"cmd",  "clear-pattern"},
        {"slot", slotName}
    });
}

// ---------------------------------------------------------------------------
// handleListPatterns() — Req 15.1
// ---------------------------------------------------------------------------

void ControlInterface::handleListPatterns()
{
    nlohmann::json patterns = nlohmann::json::array();

    const int count = audio_.slotCount();
    for (int i = 0; i < count; ++i) {
        auto state = audio_.loadSlot(i);
        if (!state) continue; // slot was cleared or never set

        nlohmann::json entry = {
            {"slot",        audio_.slotName(i)},
            {"notation",    state->notation},
            {"event_count", static_cast<int>(state->eventBuffer.size())}
        };
        patterns.push_back(std::move(entry));
    }

    emitResponse({
        {"ok",       true},
        {"cmd",      "list-patterns"},
        {"patterns", std::move(patterns)}
    });
}

// ---------------------------------------------------------------------------
// handleListSamples() — B8-K4 §6: enumerate registered sample names for autocomplete
// ---------------------------------------------------------------------------

void ControlInterface::handleListSamples()
{
    nlohmann::json samples = nlohmann::json::array();

    for (const auto& name : bank_.listNames())
        samples.push_back(name);

    emitResponse({
        {"ok",       true},
        {"cmd",      "list-samples"},
        {"samples",  std::move(samples)}
    });
}

// ---------------------------------------------------------------------------
// handleQuit() — Req 16.5
// ---------------------------------------------------------------------------

void ControlInterface::handleQuit()
{
    respond({{"ok", true}, {"cmd", "quit"}});
    // Flush is handled inside respond(), but call it again to be safe.
    std::fflush(stdout);
    std::exit(0);
}

// ---------------------------------------------------------------------------
// handleSlotPlayStop() — A3 per-slot play/stop
// ---------------------------------------------------------------------------

void ControlInterface::handleSlotPlayStop(std::string_view slotSV, bool start)
{
    const std::string cmdName = start ? "slot-play" : "slot-stop";

    if (slotSV.empty()) {
        emitResponse({
            {"ok",    false},
            {"cmd",   cmdName},
            {"error", "missing slot name"}
        });
        return;
    }

    const std::string slotName(slotSV);
    const int idx = audio_.findOrAddSlot(slotName);

    // findOrAddSlot returns -1 only when the table is full AND the name is
    // unknown.  But per the clear-pattern precedent (which checks loadSlot),
    // a slot that was never set up with pattern data is still addressable
    // for play/stop — we only reject "table full, name unknown".
    if (idx < 0) {
        emitResponse({
            {"ok",    false},
            {"cmd",   cmdName},
            {"slot",  slotName},
            {"error", "no free slot slots available"}
        });
        return;
    }

    if (start)
        audio_.slotPlay(idx);
    else
        audio_.slotStop(idx);

    emitResponse({
        {"ok",   true},
        {"cmd",  cmdName},
        {"slot", slotName}
    });
}

// ---------------------------------------------------------------------------
// AI-2: Read-only introspection command handlers (Phase 2.5 H0)
// ---------------------------------------------------------------------------
// All operations route through ProjectReadFacade — the canonical read-only
// service layer.  No direct filesystem access, no JUCE dependency, no
// mutation of engine state.
// Requirement: AI-2 §1, §12

bool ControlInterface::handleReadOnlyCommand(std::string_view cmd,
                                              std::string_view rest)
{
    if (cmd == "inspect_project") {
        emitResponse(readFacade_->inspectProject());
        return true;
    }

    if (cmd == "list_assets") {
        emitResponse(readFacade_->listAssets());
        return true;
    }

    if (cmd == "get_current_song") {
        emitResponse(readFacade_->getCurrentSong());
        return true;
    }

    if (cmd == "list_samples") {
        emitResponse(readFacade_->listSamples());
        return true;
    }

    if (cmd == "list_chuck_instruments") {
        // Argument: <projectDir>
        const std::string_view arg = trim(rest);
        std::filesystem::path projectDir;
        if (!arg.empty())
            projectDir = std::filesystem::path(std::string(arg));
        else
            projectDir = audio_.currentProjectDir();

        emitResponse(readFacade_->listChuckInstruments(projectDir));
        return true;
    }

    if (cmd == "get_diagnostics") {
        // Argument format: <sourceId> <isChuck: true|false> <content>
        auto [sourceId, afterSource] = splitFirst(rest);
        auto [isChuckStr, contentRaw] = splitFirst(afterSource);

        const std::string sourceIdStr = std::string(sourceId);
        const bool isChuck = (isChuckStr == "true" || isChuckStr == "1" ||
                              isChuckStr == "ck" || isChuckStr == "chuck");

        // Content is everything after the two tokens — it may contain spaces.
        // We need the full remaining string as the source text to diagnose.
        const std::string content = std::string(contentRaw);

        if (sourceIdStr.empty() || content.empty()) {
            emitResponse({
                {"ok",    false},
                {"cmd",   "get_diagnostics"},
                {"error", "usage: get_diagnostics <sourceId> <isChuck: true|false> <content>"}
            });
            return true;
        }

        emitResponse(readFacade_->getDiagnostics(content, sourceIdStr, isChuck));
        return true;
    }

    if (cmd == "get_audio_status") {
        emitResponse(readFacade_->getAudioStatus());
        return true;
    }

    return false;  // not recognised — caller handles the error response
}

// ---------------------------------------------------------------------------
// AI-5: ChucK session lifecycle command handlers
// -----------------------------------------------------------------------

void ControlInterface::handleChuckSessionCommand(
    std::string_view cmd, std::string_view rest)
{
    // Lazy-initialize the ChuckSessionService if not yet created.
    if (!chuckSessionService_)
        chuckSessionService_ = std::make_unique<ChuckSessionService>(audio_);

    if (cmd == "create_chuck_session") {
        handleCreateChuckSession(rest);
    } else if (cmd == "get_chuck_session") {
        handleGetChuckSession(rest);
    } else if (cmd == "compile_chuck") {
        handleCompileChuck(rest);
    } else if (cmd == "audition_chuck") {
        handleAuditionChuck(rest);
    } else if (cmd == "stop_chuck") {
        handleStopChuck(rest);
    } else if (cmd == "get_chuck_job") {
        handleGetChuckJob(rest);
    } else if (cmd == "cancel_chuck_job") {
        handleCancelChuckJob(rest);
    }
}

void ControlInterface::handleCreateChuckSession(std::string_view rest)
{
    // Format: <slotIdx> <source>
    auto [slotStr, sourceStr] = splitFirst(rest);
    if (slotStr.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "create_chuck_session"},
            {"error", "missing slot index"}
        });
        return;
    }

    int slotIdx = 0;
    try {
        slotIdx = std::stoi(std::string(slotStr));
        if (slotIdx < 0 || slotIdx >= 16) {
            emitResponse({
                {"ok", false},
                {"cmd", "create_chuck_session"},
                {"error", "slot index out of range [0, 16)"}
            });
            return;
        }
    } catch (...) {
        emitResponse({
            {"ok", false},
            {"cmd", "create_chuck_session"},
            {"error", "invalid slot index"}
        });
        return;
    }

    const std::string source = std::string(sourceStr);
    auto session = chuckSessionService_->createSession(
        static_cast<uint8_t>(slotIdx), source);

    nlohmann::json result;
    result["ok"] = true;
    result["cmd"] = "create_chuck_session";
    result["session_id"] = session.sessionId;
    result["slot_index"] = slotIdx;
    result["state"] = toString(session.state);
    if (!session.lastError.empty())
        result["error"] = session.lastError;

    emitResponse(result);
}

void ControlInterface::handleGetChuckSession(std::string_view rest)
{
    const std::string_view sessionId = trim(rest);
    if (sessionId.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "get_chuck_session"},
            {"error", "missing session_id"}
        });
        return;
    }

    auto session = chuckSessionService_->getSession(sessionId);

    nlohmann::json result;
    result["ok"] = true;
    result["cmd"] = "get_chuck_session";
    result["session_id"] = session.sessionId;
    if (session.source.empty())
        result["source"] = nullptr;
    else
        result["source"] = session.source;
    result["state"] = toString(session.state);
    result["vm_generation"] = session.vmGeneration;
    result["shred_id"] = session.shredId;
    if (!session.lastError.empty())
        result["last_error"] = session.lastError;

    // Include diagnostics if available.
    if (!session.diagnostics.empty()) {
        nlohmann::json diags = nlohmann::json::array();
        for (const auto& d : session.diagnostics) {
            diags.push_back(nlohmann::json{
                {"severity", d.severity},
                {"code", d.code},
                {"message", d.message},
                {"line", d.line},
                {"column", d.column}
            });
        }
        result["diagnostics"] = diags;
    }

    emitResponse(result);
}

void ControlInterface::handleCompileChuck(std::string_view rest)
{
    // Format: <sessionId> <source>
    auto [sessionIdStr, sourceStr] = splitFirst(rest);
    if (sessionIdStr.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "compile_chuck"},
            {"error", "missing session_id"}
        });
        return;
    }

    const std::string source = std::string(sourceStr);
    if (source.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "compile_chuck"},
            {"error", "missing source code"}
        });
        return;
    }

    // Compile is asynchronous — returns immediately with a job_id.
    const auto handle = chuckSessionService_->compileChuck(
        sessionIdStr, source,
        [](const CompileResult& result) {
            // The completion callback is invoked on the job tracker's
            // worker thread. We cannot emitResponse() here because we don't
            // have access to ControlInterface. The caller must poll
            // get_chuck_job to check completion.
            (void)result;
        });

    emitResponse({
        {"ok", true},
        {"cmd", "compile_chuck"},
        {"session_id", std::string(sessionIdStr)},
        {"job_id", handle.id()},
        {"status", "queued"}
    });
}

void ControlInterface::handleAuditionChuck(std::string_view rest)
{
    const std::string_view sessionId = trim(rest);
    if (sessionId.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "audition_chuck"},
            {"error", "missing session_id"}
        });
        return;
    }

    auto session = chuckSessionService_->auditionSession(sessionId);

    nlohmann::json result;
    result["ok"] = true;
    result["cmd"] = "audition_chuck";
    result["session_id"] = session.sessionId;
    result["state"] = toString(session.state);
    if (!session.lastError.empty())
        result["error"] = session.lastError;

    emitResponse(result);
}

void ControlInterface::handleStopChuck(std::string_view rest)
{
    const std::string_view sessionId = trim(rest);
    if (sessionId.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "stop_chuck"},
            {"error", "missing session_id"}
        });
        return;
    }

    auto session = chuckSessionService_->stopSession(sessionId);

    nlohmann::json result;
    result["ok"] = (session.state == SessionState::Destroyed);
    result["cmd"] = "stop_chuck";
    result["session_id"] = session.sessionId;
    result["state"] = toString(session.state);
    if (!session.lastError.empty())
        result["error"] = session.lastError;

    emitResponse(result);
}

void ControlInterface::handleGetChuckJob(std::string_view rest)
{
    const std::string_view jobIdStr = trim(rest);
    if (jobIdStr.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "get_chuck_job"},
            {"error", "missing job_id"}
        });
        return;
    }

    uint64_t jobId = 0;
    try {
        jobId = std::stoull(std::string(jobIdStr));
    } catch (...) {
        emitResponse({
            {"ok", false},
            {"cmd", "get_chuck_job"},
            {"error", "invalid job_id"}
        });
        return;
    }

    emitResponse(chuckSessionService_->getJobStatus(jobId));
}

void ControlInterface::handleCancelChuckJob(std::string_view rest)
{
    const std::string_view jobIdStr = trim(rest);
    if (jobIdStr.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "cancel_chuck_job"},
            {"error", "missing job_id"}
        });
        return;
    }

    uint64_t jobId = 0;
    try {
        jobId = std::stoull(std::string(jobIdStr));
    } catch (...) {
        emitResponse({
            {"ok", false},
            {"cmd", "cancel_chuck_job"},
            {"error", "invalid job_id"}
        });
        return;
    }

    bool cancelled = chuckSessionService_->cancelJob(jobId);

    emitResponse({
        {"ok", true},
        {"cmd", "cancel_chuck_job"},
        {"job_id", jobId},
        {"cancelled", cancelled}
    });
}

// ---------------------------------------------------------------------------
// AI-6: Rendering command handlers
// ---------------------------------------------------------------------------

void ControlInterface::handleRenderCommand(std::string_view cmd,
                                            std::string_view rest)
{
    // Lazy-initialize services if not yet created.
    if (!chuckSessionService_)
        chuckSessionService_ = std::make_unique<ChuckSessionService>(audio_);
    if (!renderService_)
        renderService_ = std::make_unique<RenderService>(
            audio_, bank_, *chuckSessionService_);

    if (cmd == "render_chuck") {
        handleRenderChuck(rest);
    } else if (cmd == "get_job_status") {
        handleGetJobStatus(rest);
    } else if (cmd == "commit_rendered_asset") {
        handleCommitRenderedAsset(rest);
    } else if (cmd == "cancel_render_job") {
        handleCancelRenderJob(rest);
    } else if (cmd == "list_render_jobs") {
        emitResponse({
            {"ok", true},
            {"cmd", "list_render_jobs"},
            {"jobs", renderService_->listRenderJobs()}
        });
    }
}

void ControlInterface::handleRenderChuck(std::string_view rest)
{
    // Format: <sessionId> <durationBars> <assetName> [target]
    auto [sessionId, afterSession] = splitFirst(rest);
    auto [durStr, afterDur] = splitFirst(afterSession);
    auto [assetName, afterAsset] = splitFirst(trim(afterDur));

    if (sessionId.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "render_chuck"},
            {"error", "missing session_id"}
        });
        return;
    }

    int durationBars = 0;
    try {
        durationBars = std::stoi(std::string(durStr));
    } catch (...) {
        emitResponse({
            {"ok", false},
            {"cmd", "render_chuck"},
            {"error", "invalid duration_bars"}
        });
        return;
    }

    if (assetName.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "render_chuck"},
            {"error", "missing asset_name"}
        });
        return;
    }

    // Optional target.
    hathor::AssetTarget target = hathor::AssetTarget::Studio;
    std::string_view targetStr = trim(afterAsset);
    if (!targetStr.empty()) {
        if (!hathor::parseAssetTarget(targetStr, target)) {
            emitResponse({
                {"ok", false},
                {"cmd", "render_chuck"},
                {"error", "unknown target; valid: studio, live_jam"}
            });
            return;
        }
    }

    uint64_t jobId = renderService_->renderChuck(sessionId, durationBars,
                                                  assetName, target);

    emitResponse({
        {"ok", true},
        {"cmd", "render_chuck"},
        {"job_id", jobId},
        {"status", "queued"}
    });
}

void ControlInterface::handleGetJobStatus(std::string_view rest)
{
    const std::string_view jobIdStr = trim(rest);
    if (jobIdStr.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "get_job_status"},
            {"error", "missing job_id"}
        });
        return;
    }

    uint64_t jobId = 0;
    try {
        jobId = std::stoull(std::string(jobIdStr));
    } catch (...) {
        emitResponse({
            {"ok", false},
            {"cmd", "get_job_status"},
            {"error", "invalid job_id"}
        });
        return;
    }

    nlohmann::json result = renderService_->getJobStatus(jobId);
    result["cmd"] = "get_job_status";
    emitResponse(result);
}

void ControlInterface::handleCommitRenderedAsset(std::string_view rest)
{
    // Format: <jobId> <assetName> [confirm_overwrite]
    auto [jobIdStr, afterJob] = splitFirst(rest);
    auto [assetName, afterAsset] = splitFirst(trim(afterJob));

    if (jobIdStr.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "commit_rendered_asset"},
            {"error", "missing job_id"}
        });
        return;
    }

    uint64_t jobId = 0;
    try {
        jobId = std::stoull(std::string(jobIdStr));
    } catch (...) {
        emitResponse({
            {"ok", false},
            {"cmd", "commit_rendered_asset"},
            {"error", "invalid job_id"}
        });
        return;
    }

    if (assetName.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "commit_rendered_asset"},
            {"error", "missing asset_name"}
        });
        return;
    }

    const std::string_view confirmStr = trim(afterAsset);
    const bool confirmOverwrite = (confirmStr == "true" || confirmStr == "1"
                                    || confirmStr == "confirm");

    nlohmann::json result = renderService_->commitRenderedAsset(
        jobId, assetName, confirmOverwrite);
    result["cmd"] = "commit_rendered_asset";
    emitResponse(result);
}

void ControlInterface::handleCancelRenderJob(std::string_view rest)
{
    const std::string_view jobIdStr = trim(rest);
    if (jobIdStr.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "cancel_render_job"},
            {"error", "missing job_id"}
        });
        return;
    }

    uint64_t jobId = 0;
    try {
        jobId = std::stoull(std::string(jobIdStr));
    } catch (...) {
        emitResponse({
            {"ok", false},
            {"cmd", "cancel_render_job"},
            {"error", "invalid job_id"}
        });
        return;
    }

    bool cancelled = renderService_->cancelJob(jobId);

    emitResponse({
        {"ok", true},
        {"cmd", "cancel_render_job"},
        {"job_id", jobId},
        {"cancelled", cancelled}
    });
}

// ---------------------------------------------------------------------------
// AI-7: Song mutation (edit_song)
// ---------------------------------------------------------------------------

void ControlInterface::handleEditSong(std::string_view songFile,
                                       std::string_view rest)
{
    if (songFile.empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "edit_song"},
            {"error", "missing song file"}
        });
        return;
    }

    if (trim(rest).empty()) {
        emitResponse({
            {"ok", false},
            {"cmd", "edit_song"},
            {"error", "missing operations JSON"}
        });
        return;
    }

    const nlohmann::json result = songMutationService_->handleEditSong(songFile, rest);
    nlohmann::json withCmd = result;
    withCmd["cmd"] = "edit_song";
    emitResponse(withCmd);
}

} // namespace hathor::control
