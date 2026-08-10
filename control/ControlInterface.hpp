// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * ControlInterface.hpp — command dispatch and stdin reader loop.
 *
 * ControlInterface reads newline-delimited commands from stdin, dispatches
 * them to the appropriate handlers, and writes JSON responses to stdout via
 * the thread-safe respond() function in Commands.hpp.
 *
 * Requirements: 12.1–12.5, 13.5, 14.1–14.6, 15.1–15.3, 16.2, 16.5
 */

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "ChuckSession.hpp"
#include "ChuckSessionService.hpp"
#include "RenderService.hpp"
#include "SongMutationService.hpp"
#include "AuthoringContext.hpp"

// Forward declarations — full headers are only needed in the .cpp.
class AudioEngineFacade;
class SampleBank;

namespace hathor::language {
struct LanguageMetadata;
struct MetadataCompatibility;
}

namespace hathor::control {

class ProjectReadFacade;

/**
 * ControlInterface — owns the WorkerThread and processes ACP commands.
 *
 * Lifecycle:
 *   1. Construct with AudioEngine& and SampleBank& references.
 *   2. Call run() which blocks until EOF on stdin or a quit command.
 */
class ControlInterface {
public:
    explicit ControlInterface(AudioEngineFacade& audio, SampleBank& bank);
    ~ControlInterface();

    // Non-copyable, non-movable.
    ControlInterface(const ControlInterface&)            = delete;
    ControlInterface& operator=(const ControlInterface&) = delete;
    ControlInterface(ControlInterface&&)                 = delete;
    ControlInterface& operator=(ControlInterface&&)      = delete;

    /**
     * Blocking stdin reader loop.
     *
     * Reads one line at a time; dispatches each to dispatch().
     * Exits via std::exit(0) on EOF or after a "quit" command.
     *
     * Requirement: 12.1, 16.2
     */
    void run();

    /**
     * Parse and dispatch a single command line.
     *
     * Trims leading/trailing whitespace, splits on the first whitespace
     * token to extract the command name, then routes to the appropriate
     * handle*() method.  Unknown commands produce an error JSON response.
     *
     * Requirement: 12.4, 12.5
     */
    void dispatch(std::string_view line);

    /**
     * Enqueue a `set-pattern` job directly on the worker thread with a
     * per-job response callback.  Used by the UI eval path (Req 23.7) so
     * the result can be marshalled to the JUCE message thread without going
     * through stdout.
     *
     * @param slotName   Destination slot name (e.g. "d0").
     * @param notation   Raw mini-notation string.
     * @param onComplete Callback invoked on the worker thread with the JSON
     *                   result.  Must marshal to the message thread if it
     *                   touches JUCE components.
     *
     * Requirement: 23.7
     */
    void enqueueSetPattern(const std::string& slotName,
                           const std::string& notation,
                           std::function<void(nlohmann::json)> onComplete);

    /**
      * UI-facing dispatch: same routing as dispatch(), but instead of writing
      * the JSON response to stdout, the response is delivered to @p onResult
      * called on the worker thread (for async commands like set-pattern) or
      * on the calling thread (for synchronous commands like bpm, set-gain).
      *
      * MUST be called on a worker thread, never on the JUCE message thread
      * (Req 23.7).
      *
      * @param line      The command line (e.g. "set-pattern d0 bd sn").
      * @param onResult  Callback invoked with the JSON response.
      *                  For set-pattern, called on the WorkerThread after
      *                  pattern compilation completes. For other commands,
      *                  called synchronously before this function returns.
      *
      * Requirements: 23.1, 23.3, 23.7
      */
    void dispatchWithCallback(std::string_view line,
                               std::function<void(nlohmann::json)> onResult);

    /**
     * Dispatch a per-slot play/stop command synchronously.
     *
     * Convenience wrapper around dispatchWithCallback for B1 (per-tab Play/Stop).
     * Issues "slot-play <slotName>" or "slot-stop <slotName>" and calls onResult
     * with the JSON response.  For synchronous slot commands the callback fires
     * before this function returns.
     *
     * MUST be called on a worker thread (not the JUCE message thread).
     *
     * @param slotName  Slot name string (e.g. "d0", "d1").
     * @param start     true = slot-play, false = slot-stop.
     * @param onResult  Callback invoked with the JSON response.
     */
    void dispatchSlotPlayStop(const std::string& slotName,
                               bool start,
                               std::function<void(nlohmann::json)> onResult);

    // -----------------------------------------------------------------------
    // AI-2: Read-only introspection commands (Phase 2.5 H0)
    // -----------------------------------------------------------------------
    // These commands route through ProjectReadFacade (the canonical AI-2 service
    // layer) which delegates to the real AudioEngineFacade + SampleBank
    // subsystems.  No MCP business logic is implemented here — each handler
    // simply delegates to the corresponding ProjectReadFacade method.
    //
    // Requirement: AI-2 §12 (MCP routes through canonical service layer)

    /// Handle a read-only introspection command.
    /// @return true if the command was recognised, false if not.
    ///
    /// Supported commands:
    ///   inspect_project       → ProjectReadFacade::inspectProject()
    ///   get_current_song      → ProjectReadFacade::getCurrentSong()
    ///   list_samples          → ProjectReadFacade::listSamples()
    ///   list_chuck_instruments <projectDir> → ProjectReadFacade::listChuckInstruments()
    ///   get_diagnostics <sourceId> <isChuck> <content...>  → ProjectReadFacade::getDiagnostics()
    ///   get_audio_status      → ProjectReadFacade::getAudioStatus()
    bool handleReadOnlyCommand(std::string_view cmd, std::string_view rest);

    /// ProjectReadFacade — the canonical AI-2 read-only service layer.
    /// Constructed in the ControlInterface constructor; all read operations
    /// route through it.
    ProjectReadFacade& readOnly() noexcept { return *readFacade_; }

    // -----------------------------------------------------------------------
    // AI-8: Dynamic authoring context assembly
    // -----------------------------------------------------------------------
    // These methods inject the editor and LSP context providers so that
    // the get-context command can assemble a targeted, dynamic context
    // payload for AI authoring requests (MCP get_context tool).
    //
    // All providers are JUCE-free abstract interfaces; the UI layer supplies
    // concrete implementations that read from JUCE components on the message
    // thread and snapshot state for thread-safe access from the acceptor thread.
    //
    // Requirement references: AI-8 §2, §3, §9

    /// Install the editor context provider (current file, cursor, selection).
    /// May be called at any time; a null provider disables editor context.
    void setEditorContextProvider(EditorContextProvider* provider) noexcept;

    /// Install the LSP context provider (diagnostics, completions, hover).
    /// May be called at any time; a null provider disables LSP context.
    void setLspContextProvider(LspContextProvider* provider) noexcept;

    /// Set the LanguageMetadata + compatibility (AI-3 versioned metadata).
    /// Called after the UI layer loads and validates the metadata JSON.
    /// May be called multiple times (hot-reload); the latest valid set wins.
    void setLanguageMetadata(const hathor::language::LanguageMetadata* metadata,
                             const hathor::language::MetadataCompatibility* compat) noexcept;

    /// Assemble and emit the dynamic authoring context for the given request.
    /// Parses JSON arguments from the socket command line, builds a
    /// ContextRequest, and responds via the thread-local response sink.
    ///
    /// Format: get-context <json-args>
    ///   json-args: {"file":"...","line":N,"character":N,"language":"...",
    ///               "scope":["editor","diagnostics"],"include_content":bool}
     void handleGetContext(std::string_view args);

     /**
      * Assemble the dynamic authoring context directly (without socket I/O).
      * Used by the UI layer (HathorTab) to inject AI-8 context into ghost-text
      * (llm-ls) FIM requests as additional prompt context.
      *
      * @param req  The context request (file, line, language, scope, etc.).
      * @return JSON object with "ok", "sections", "metadata_version", etc.
      *
      * Requirement references: AI-8 §4, §7, AI-G2
      */
     nlohmann::json assembleAuthoringContext(const ContextRequest& req) const;

    // -----------------------------------------------------------------------
    // AI-5: ChucK session lifecycle (read-only + non-destructive execution)
    // -----------------------------------------------------------------------

    /// Handle a create_chuck_session command (non-destructive execution).
    /// Routes through ChuckSessionService → AudioWorkerManager → B4-K3.
    /// Format: create_chuck_session <slotIdx> <source>
    void handleCreateChuckSession(std::string_view rest);

    /// Handle a get_chuck_session command (read-only).
    /// Format: get_chuck_session <sessionId>
    void handleGetChuckSession(std::string_view rest);

    /// Handle a compile_chuck command (non-destructive execution, async).
    /// Format: compile_chuck <sessionId> <source>
    void handleCompileChuck(std::string_view rest);

    /// Handle an audition_chuck command (non-destructive execution).
    /// Format: audition_chuck <sessionId>
    void handleAuditionChuck(std::string_view rest);

    /// Handle a stop_chuck command (non-destructive execution).
    /// Format: stop_chuck <sessionId>
    void handleStopChuck(std::string_view rest);

    /// Handle a get_chuck_job command (read-only).
    /// Format: get_chuck_job <jobId>
    void handleGetChuckJob(std::string_view rest);

     /// Handle a cancel_chuck_job command (non-destructive execution).
     /// Format: cancel_chuck_job <jobId>
     void handleCancelChuckJob(std::string_view rest);

     /// Dispatch an AI-5 ChucK session command.
     /// Routes to the appropriate handle*Chuck* method.
     void handleChuckSessionCommand(std::string_view cmd, std::string_view rest);

     // -----------------------------------------------------------------------
     // AI-6: Rendering service
     // -----------------------------------------------------------------------

     /// Handle a render_chuck command (async, non-blocking).
     /// Format: render_chuck <sessionId> <durationBars> <assetName> [target]
     void handleRenderChuck(std::string_view rest);

     /// Handle a get_job_status command (read-only).
     /// Format: get_job_status <jobId>
     void handleGetJobStatus(std::string_view rest);

     /// Handle a commit_rendered_asset command (mutating — requires confirmation
     /// for overwrites).
     /// Format: commit_rendered_asset <jobId> <assetName> [confirm_overwrite]
     void handleCommitRenderedAsset(std::string_view rest);

     /// Handle a cancel_render_job command.
     /// Format: cancel_render_job <jobId>
     void handleCancelRenderJob(std::string_view rest);

      /// Dispatch an AI-6 render command.
      void handleRenderCommand(std::string_view cmd, std::string_view rest);

      // -----------------------------------------------------------------------
      // AI-7: Song mutation service
      // -----------------------------------------------------------------------

      /// Handle an edit_song command (persistent mutation — requires
      /// confirmation for replace/delete/overwrite).
      /// Format: edit_song <songFile> <opsJson>
      void handleEditSong(std::string_view songFile, std::string_view rest);

private:
    // --- Command handlers ---------------------------------------------------

    /// set-pattern <slot> <notation>  (Req 11.5, 13.1–13.4)
    void handleSetPattern(std::string_view slot, std::string_view notation);

    /// clear-pattern <slot>  (Req 15.2, 15.3)
    void handleClearPattern(std::string_view slot);

    /// slot-play <slot> / slot-stop <slot>  (A3)
    void handleSlotPlayStop(std::string_view slot, bool start);

    /// list-patterns  (Req 15.1)
    void handleListPatterns();

    /// list-samples  (B8-K4 §6) — enumerate all registered sample names.
    void handleListSamples();

    /// bpm <value>  (Req 14.3, 14.4)
    void handleBpm(std::string_view arg);

    /// play  (Req 14.1)
    void handlePlay();

    /// stop  (Req 14.2)
    void handleStop();

    /// ping  (Req 14.6)
    void handlePing(std::chrono::steady_clock::time_point receiveTime);

    /// set-gain <value>  (Req 26.7, 26.8)
    void handleSetGain(std::string_view arg);

    /// set-eq-preset <preset>  (B7-K2)
    /// Preset names: flat, bass-boost, vocal, bright
    void handleSetEqPreset(std::string_view arg);

    /// quit  (Req 16.5)
    void handleQuit();

    // --- Members ------------------------------------------------------------
    AudioEngineFacade& audio_;
    SampleBank&  bank_;

    // AI-2: Read-only introspection service layer (canonical service contract).
    std::unique_ptr<ProjectReadFacade> readFacade_;

    // AI-5: Canonical ChucK session service layer.
    // Constructed when HATHOR_BUILD_APP and the worker is available.
    std::unique_ptr<ChuckSessionService> chuckSessionService_;

    // AI-6: Canonical rendering service layer.
    // Constructed lazily after ChuckSessionService (shares the same session
    // store for source retrieval).
    std::unique_ptr<RenderService> renderService_;

    // AI-7: Canonical song mutation service layer.
    // Constructed eagerly — does not depend on ChuckSessionService.
    std::unique_ptr<SongMutationService> songMutationService_;

    // AI-8: Dynamic authoring context assembler.
    // Created in the constructor with readFacade_; providers are set later
    // by the UI layer via setEditorContextProvider / setLspContextProvider /
    // setLanguageMetadata.  Null if not yet configured.
    std::unique_ptr<AuthoringContext> authoringContext_;

    // WorkerThread is allocated on the heap to keep this header JUCE-free.
    // (WorkerThread.hpp only forward-declares AudioEngine, so it is safe.)
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace hathor::control
