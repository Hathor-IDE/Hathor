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
#include "CompletionContextProvider.hpp"
#include "IntentPlanner.hpp"

// Forward declarations — full headers are only needed in the .cpp.
class AudioEngineFacade;
class SampleBank;

namespace hathor::language {
struct LanguageMetadata;
struct MetadataCompatibility;
class ProjectSymbolIndex;
}

namespace hathor::control {

class ProjectReadFacade;
class AgenticWorkflow;

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

    /// Set the few-shot example corpus (AI-G4). The provider holds a non-owning
    /// pointer; the caller (UI layer) owns the loaded corpus lifetime.
    void setFewShotCorpus(const hathor::language::FewShotCorpus* corpus) noexcept;

    /// Set the J-5 ProjectSymbolIndex for project-aware code completion.
    /// The ControlInterface holds a non-owning pointer; the caller owns the
    /// index lifetime. May be called multiple times (e.g. after hot-reload).
    void setProjectSymbolIndex(hathor::language::ProjectSymbolIndex* index) noexcept;

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
    // AI-G3: Hathor-specific authoring-context provider for llm-ls FIM
    // -----------------------------------------------------------------------
    // Assembles a compact, location-aware, bounded context for the llm-ls ghost-
    // writing (FIM) request. Reuses the SAME providers as AI-8 (no second
    // context model). The resulting JSON is injected as fim.prefix.
    //
    // Requirement references: AI-G3, AI-G1, AI-G2
     nlohmann::json assembleCompletionContext(const CompletionRequest& req) const;

     /// Set the default bounds for the completion context provider (AI-G3).
     /// Called by the UI layer to configure the context size budget.
     void setCompletionBounds(const ContextBounds& bounds) noexcept;

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

    // -----------------------------------------------------------------------
    // AI-10: Agentic musical workflow
    // -----------------------------------------------------------------------

    /// Dispatch an AI-10 workflow command.
    /// Routes to the appropriate handle*Workflow* method.
    void handleWorkflowCommand(std::string_view cmd, std::string_view rest);

     /// Handle a workflow_plan command (AI-10.1: generate a structured plan
     /// from a natural-language request BEFORE heavy execution).
     /// Format: workflow_plan <intent> [json-kwargs]
     ///   intent:  natural-language request (e.g. "dark 8-bar acid bassline")
     ///   json-kwargs: {"target_slot":"d1","asset_name":"acid_bass"}
     void handleWorkflowPlan(std::string_view rest);

     /// Handle a workflow_repair command (AI-10.5: creative repair from
     /// conversational feedback).
     /// Format: workflow_repair <json-args>
     ///   json-args: {"feedback":"make it darker","intent_context":"bass"}
      void handleWorkflowRepair(std::string_view rest);

      /// Handle a workflow_replan command (AI-10.6: restart workflow with new request).
      /// Format: workflow_replan <json-args>
      ///   json-args: {"intent":"...","target_slot":"d1","notation":"...",
      ///               "ck_source":"...","asset_name":"...","duration_bars":8,
      ///               "dry_run":false}
      void handleWorkflowReplan(std::string_view rest);

     /// Handle a workflow_start command (async — starts the agentic workflow).
    /// Format: workflow_start <json-args>
    ///   json-args: {"intent":"...","target_slot":"d1","notation":"...",
    ///               "ck_source":"...","asset_name":"...","duration_bars":8,
    ///               "dry_run":false,"plan":{...}}
    void handleWorkflowStart(std::string_view rest);

    /// Handle a workflow_cancel command.
    /// Format: workflow_cancel
    void handleWorkflowCancel(std::string_view rest);

    /// Handle a workflow_status command (read-only query).
    /// Format: workflow_status
    void handleWorkflowStatus(std::string_view rest);

     /// Handle a workflow_approve command (respond to confirmation).
     /// Format: workflow_approve <request_id>
     void handleWorkflowApprove(std::string_view rest, bool approved);

     // -----------------------------------------------------------------------
     // AI-10.2: Conversational memory / working set
     // -----------------------------------------------------------------------

     /// Handle a working_set command (read-only query of conversational memory).
     /// Format: working_set
     void handleWorkingSet(std::string_view rest);

     /// Handle a resolve_reference command (resolve a conversational reference).
     /// Format: resolve_reference <phrase> [intent_context]
     void handleResolveReference(std::string_view rest);

     /// Handle a revert_change command (undo the last reversible change).
     /// Format: revert_change [change_id]
     void handleRevertChange(std::string_view rest);

     /// Handle a clear_working_set command (clear session-scoped memory).
     /// Format: clear_working_set
     void handleClearWorkingSet(std::string_view rest);

     // -----------------------------------------------------------------------
     // AI-10.3: First-class diff / preview / undo for AI changes
     // -----------------------------------------------------------------------

     /// Handle a changeset_status command (query the active change-set).
     /// Format: changeset_status
     void handleChangeSetStatus(std::string_view rest);

     /// Handle a changeset_preview command (human-readable diff of changes).
     /// Format: changeset_preview
     void handleChangeSetPreview(std::string_view rest);

     /// Handle a changeset_accept command (finalise the active change-set).
     /// Format: changeset_accept
     void handleChangeSetAccept(std::string_view rest);

     /// Handle a changeset_reject command (revert the entire pending change-set).
     /// Format: changeset_reject [confirm]
     void handleChangeSetReject(std::string_view rest);

     /// Handle a changeset_undo command (revert an accepted change-set).
     /// Format: changeset_undo <change_set_id> [confirm]
     void handleChangeSetUndo(std::string_view rest);

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

    /// index_project <projectDir>  (J-5)
    /// Trigger a project symbol index refresh for project-aware completion.
    void handleIndexProject(std::string_view projectDir);

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

    // AI-10: Agentic musical workflow orchestration.
    // Lazily constructed on the first workflow_start command (after the
    // ChuckSessionService and RenderService have been initialized).  Owns
    // a background thread for the workflow; only one workflow may run at a
    // time (start() returns false if already active).
    std::unique_ptr<AgenticWorkflow> agenticWorkflow_;

    // AI-G3: Hathor-specific authoring-context provider for llm-ls FIM.
    // Created in the constructor with readFacade_; shares the same providers
    // as AuthoringContext (set via the AI-8 setters below, forwarded here).
    // Null if not yet configured.
    std::unique_ptr<CompletionContextProvider> completionContext_;

    // WorkerThread is allocated on the heap to keep this header JUCE-free.
    // (WorkerThread.hpp only forward-declares AudioEngine, so it is safe.)
    struct Impl;
    Impl* impl_{nullptr};
};

} // namespace hathor::control
