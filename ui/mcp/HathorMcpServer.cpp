// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * HathorMcpServer.cpp -- standalone hathor-mcp executable.
 *
 * Speaks MCP JSON-RPC stdio to the agent (MCP server role) and forwards
 * tool calls over a Unix domain socket to the Hathor process.
 *
 * Deliberately links NO JUCE modules (Req 31.1, 31.2).
 *
 * Requirements: 32.7
 */

#include <nlohmann/json.hpp>

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Unix socket helpers
// ---------------------------------------------------------------------------

/** Connect to the Hathor Unix domain socket at @p path.
 *  Returns the file descriptor on success, or -1 on failure (prints to stderr). */
static int connectUnixSocket(const char* path)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::fprintf(stderr, "hathor-mcp: socket() failed: %s\n", std::strerror(errno));
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::fprintf(stderr, "hathor-mcp: connect(%s) failed: %s\n", path, std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

/** Send a newline-terminated command to the Unix socket.
 *  Returns false on write error. */
static bool sendCommand(int fd, const std::string& cmd)
{
    std::string line = cmd + "\n";
    const char* ptr = line.c_str();
    std::size_t remaining = line.size();
    while (remaining > 0)
    {
        ssize_t written = ::write(fd, ptr, remaining);
        if (written <= 0)
            return false;
        ptr += static_cast<std::size_t>(written);
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

/** Read one newline-terminated line from @p fd with a @p timeoutMs millisecond timeout.
 *  Returns the line (without trailing newline) on success, or an empty string on
 *  timeout / error.  Sets @p timedOut to true if the timeout expired. */
static std::string readLine(int fd, int timeoutMs, bool& timedOut)
{
    timedOut = false;
    std::string result;

    struct pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    while (true)
    {
        int ret = ::poll(&pfd, 1, timeoutMs);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            return {};
        }
        if (ret == 0)
        {
            timedOut = true;
            return {};
        }

        char ch = '\0';
        ssize_t n = ::read(fd, &ch, 1);
        if (n <= 0)
            return {}; // EOF or error
        if (ch == '\n')
            return result;
        result += ch;
    }
}

// ---------------------------------------------------------------------------
// MCP tools/list response
// ---------------------------------------------------------------------------

static json makeToolsList()
{
    json setPatternSchema;
    setPatternSchema["type"] = "object";
    setPatternSchema["properties"]["slot"]["type"] = "string";
    setPatternSchema["properties"]["slot"]["description"] = "Slot name, e.g. d1";
    setPatternSchema["properties"]["notation"]["type"] = "string";
    setPatternSchema["properties"]["notation"]["description"] = "Mini-notation pattern string";
    setPatternSchema["required"] = json::array({"slot", "notation"});

    json bpmSchema;
    bpmSchema["type"] = "object";
    bpmSchema["properties"]["value"]["type"] = "number";
    bpmSchema["properties"]["value"]["description"] = "BPM value (20-400)";
    bpmSchema["required"] = json::array({"value"});

    json noArgSchema;
    noArgSchema["type"] = "object";
    noArgSchema["properties"] = json::object();

    json gainSchema;
    gainSchema["type"] = "object";
    gainSchema["properties"]["value"]["type"] = "number";
    gainSchema["properties"]["value"]["description"] = "Gain value (0.0-2.0)";
    gainSchema["required"] = json::array({"value"});

    // get_context: dynamic authoring context (AI-8)
    // Assembles targeted editor/language/project/runtime context for AI requests.
    // Parameters are all optional — absent parameters default to the current
    // editor state and auto-detected relevance.
    json getContextSchema;
    getContextSchema["type"] = "object";
    getContextSchema["properties"]["file"] = json::object({
        {"type", "string"},
        {"description", "File path or URI to focus on (e.g. 'song.hathor'). If omitted, uses the current active editor tab."}
    });
    getContextSchema["properties"]["line"] = json::object({
        {"type", "integer"},
        {"description", "0-based cursor line number. If omitted, uses the current cursor position."}
    });
    getContextSchema["properties"]["character"] = json::object({
        {"type", "integer"},
        {"description", "0-based cursor character column. If omitted, uses the current cursor position."}
    });
    getContextSchema["properties"]["language"] = json::object({
        {"type", "string"},
        {"description", "Language hint: 'mininotation' or 'chuck'. If omitted, inferred from file extension."}
    });
    getContextSchema["properties"]["selected_text"] = json::object({
        {"type", "string"},
        {"description", "Currently selected text, if any."}
    });
    getContextSchema["properties"]["scope"] = json::object({
        {"type", "array"},
        {"description", "Which context sections to include. If omitted, auto-determined from file type. Values: 'editor', 'diagnostics', 'metadata', 'runtime', 'samples', 'instruments', 'lsp', 'project'."},
        {"items", json::object({{"type", "string"}})}
    });
    getContextSchema["properties"]["include_content"] = json::object({
        {"type", "boolean"},
        {"description", "Include the full file content in the response (default: false to keep context targeted)."}
    });
    getContextSchema["properties"]["max_content_length"] = json::object({
        {"type", "integer"},
        {"description", "Maximum content length in bytes if include_content is true (default: 8192). 0 = no limit."},
        {"default", 8192}
    });

    json editSongOpsSchema;
    editSongOpsSchema["type"] = "object";
    editSongOpsSchema["properties"]["op"]["type"] = "string";
    editSongOpsSchema["properties"]["op"]["description"] = "Operation type: replace_pattern | insert | set_meta | clear_pattern | delete_song";
    editSongOpsSchema["required"] = json::array({"op"});
    editSongOpsSchema["additionalProperties"] = true;

    json editSongSchema;
    editSongSchema["type"] = "object";
    editSongSchema["properties"]["song_file"]["type"] = "string";
    editSongSchema["properties"]["song_file"]["description"] = "Song file name (relative to project dir)";
    editSongSchema["properties"]["ops"]["type"] = "array";
    editSongSchema["properties"]["ops"]["description"] = "Array of operation objects";
    editSongSchema["properties"]["ops"]["items"] = editSongOpsSchema;
    editSongSchema["required"] = json::array({"song_file", "ops"});

    json slotPlaySchema;
    slotPlaySchema["type"] = "object";
    slotPlaySchema["properties"]["slot"]["type"] = "string";
    slotPlaySchema["properties"]["slot"]["description"] = "Slot name, e.g. d1";
    slotPlaySchema["required"] = json::array({"slot"});

    json slotStopSchema;
    slotStopSchema["type"] = "object";
    slotStopSchema["properties"]["slot"]["type"] = "string";
    slotStopSchema["properties"]["slot"]["description"] = "Slot name, e.g. d1";
    slotStopSchema["required"] = json::array({"slot"});

    json setEqPresetSchema;
    setEqPresetSchema["type"] = "object";
    setEqPresetSchema["properties"]["preset"]["type"] = "string";
    setEqPresetSchema["properties"]["preset"]["description"] = "EQ preset name (flat, bass-boost, vocal, bright)";
    setEqPresetSchema["required"] = json::array({"preset"});

    json clearPatternSchema;
    clearPatternSchema["type"] = "object";
    clearPatternSchema["properties"]["slot"]["type"] = "string";
    clearPatternSchema["properties"]["slot"]["description"] = "Slot name to clear (optional)";

    json tools = json::array();

    json setPattern;
    setPattern["name"] = "set_pattern";
    setPattern["description"] = "Set the mini-notation pattern for a named slot";
    setPattern["inputSchema"] = setPatternSchema;
    tools.push_back(setPattern);

    json bpm;
    bpm["name"] = "bpm";
    bpm["description"] = "Set the global BPM";
    bpm["inputSchema"] = bpmSchema;
    tools.push_back(bpm);

    json play;
    play["name"] = "play";
    play["description"] = "Start pattern playback";
    play["inputSchema"] = noArgSchema;
    tools.push_back(play);

    json stop;
    stop["name"] = "stop";
    stop["description"] = "Stop pattern playback";
    stop["inputSchema"] = noArgSchema;
    tools.push_back(stop);

    json setGain;
    setGain["name"] = "set_gain";
    setGain["description"] = "Set master output gain (0.0-2.0)";
    setGain["inputSchema"] = gainSchema;
    tools.push_back(setGain);

    // get_context: AI-8 dynamic authoring context
    json getContext;
    getContext["name"] = "get_context";
    getContext["description"] = "Assemble targeted editor/language/project/runtime context for AI authoring. "
        "Returns a compact JSON payload with sections relevant to the current file "
        "and cursor position. Language intelligence comes from the Strudel LSP; "
        "Hathor-specific facts come from versioned supported-surface metadata (AI-3). "
        "All parameters are optional — omit them to use the current editor state.";
    getContext["inputSchema"] = getContextSchema;
    tools.push_back(getContext);

    json editSong;
    editSong["name"] = "edit_song";
    editSong["description"] = "Apply structured operations to a .hathor song file (replace_pattern, insert, set_meta, clear_pattern, delete_song)";
    editSong["inputSchema"] = editSongSchema;
    tools.push_back(editSong);

    json slotPlay;
    slotPlay["name"] = "slot-play";
    slotPlay["description"] = "Start playback on a named slot";
    slotPlay["inputSchema"] = slotPlaySchema;
    tools.push_back(slotPlay);

    json slotStop;
    slotStop["name"] = "slot-stop";
    slotStop["description"] = "Stop playback on a named slot";
    slotStop["inputSchema"] = slotStopSchema;
    tools.push_back(slotStop);

    json setEqPreset;
    setEqPreset["name"] = "set-eq-preset";
    setEqPreset["description"] = "Set the master EQ preset";
    setEqPreset["inputSchema"] = setEqPresetSchema;
    tools.push_back(setEqPreset);

    json clearPattern;
    clearPattern["name"] = "clear-pattern";
    clearPattern["description"] = "Clear the pattern for a slot (or all patterns if no slot given)";
    clearPattern["inputSchema"] = clearPatternSchema;
    tools.push_back(clearPattern);

    json listPatterns;
    listPatterns["name"] = "list-patterns";
    listPatterns["description"] = "List all active patterns";
    listPatterns["inputSchema"] = noArgSchema;
    tools.push_back(listPatterns);

    json listSamplesLegacy;
    listSamplesLegacy["name"] = "list-samples";
    listSamplesLegacy["description"] = "List registered sample names";
    listSamplesLegacy["inputSchema"] = noArgSchema;
    tools.push_back(listSamplesLegacy);

    // -----------------------------------------------------------------------
    // AI-2 read-only introspection tools (Phase 3B: MCP Tool Expansion)
    // These map 1:1 to ControlInterface::handleReadOnlyCommand dispatch
    // commands (AI-2 §1, §12 — ProjectReadFacade service layer).
    // -----------------------------------------------------------------------

    json inspectProject;
    inspectProject["name"] = "inspect_project";
    inspectProject["description"] = "Inspect the current Hathor project: project directory, songs, tempo, active pattern slots, ChucK instruments, and registered samples";
    inspectProject["inputSchema"] = noArgSchema;
    tools.push_back(inspectProject);

    json getCurrentSong;
    getCurrentSong["name"] = "get_current_song";
    getCurrentSong["description"] = "Get the semantic state of the currently active song: pattern notation, tempo, slot name, parse diagnostics, and referenced sample assets";
    getCurrentSong["inputSchema"] = noArgSchema;
    tools.push_back(getCurrentSong);

    json listAssets;
    listAssets["name"] = "list_assets";
    listAssets["description"] = "List all project assets: songs (.hathor files), ChucK instruments with lifecycle state, and samples registered in the SampleBank";
    listAssets["inputSchema"] = noArgSchema;
    tools.push_back(listAssets);

    json listSamples;
    listSamples["name"] = "list_samples";
    listSamples["description"] = "List all registered samples in the SampleBank with full metadata (path, duration, channels, sample rate)";
    listSamples["inputSchema"] = noArgSchema;
    tools.push_back(listSamples);

    json listChuckInstrumentsSchema;
    listChuckInstrumentsSchema["type"] = "object";
    listChuckInstrumentsSchema["properties"]["project_dir"] = json::object({
        {"type", "string"},
        {"description", "Optional project directory path to scan for ChucK instruments. If omitted, the current project directory is used."}
    });

    json listChuckInstruments;
    listChuckInstruments["name"] = "list_chuck_instruments";
    listChuckInstruments["description"] = "List ChucK instruments (.ck sources and rendered .wav files) in the project with lifecycle state (source_only, rendered, bound)";
    listChuckInstruments["inputSchema"] = listChuckInstrumentsSchema;
    tools.push_back(listChuckInstruments);

    json getDiagnosticsSchema;
    getDiagnosticsSchema["type"] = "object";
    getDiagnosticsSchema["properties"]["source_id"] = json::object({
        {"type", "string"},
        {"description", "Resource identifier for diagnostics (e.g. 'slot:d0' or 'file:bd.hathor')"}
    });
    getDiagnosticsSchema["properties"]["is_chuck"] = json::object({
        {"type", "boolean"},
        {"description", "true if content is ChucK (.ck) code; false if mini-notation"}
    });
    getDiagnosticsSchema["properties"]["content"] = json::object({
        {"type", "string"},
        {"description", "Source text to analyze for compile/parse errors and warnings"}
    });
    getDiagnosticsSchema["required"] = json::array({"source_id", "is_chuck", "content"});

    json getDiagnostics;
    getDiagnostics["name"] = "get_diagnostics";
    getDiagnostics["description"] = "Run language diagnostics on a source text: ChucK compiler validation for .ck code, or mini-notation parser/tokeniser for patterns";
    getDiagnostics["inputSchema"] = getDiagnosticsSchema;
    tools.push_back(getDiagnostics);

    json getAudioStatus;
    getAudioStatus["name"] = "get_audio_status";
    getAudioStatus["description"] = "Get current audio engine status: transport state (running, BPM, sample rate, gain), per-slot playback, worker thread status, and active voice count";
    getAudioStatus["inputSchema"] = noArgSchema;
    tools.push_back(getAudioStatus);

    // -----------------------------------------------------------------------
    // AI-5: ChucK session lifecycle tools (Phase 3C: MCP Tool Expansion)
    // These map 1:1 to ControlInterface::handleChuckSessionCommand dispatch
    // commands (AI-5 §1–§18 — ChuckSessionService service layer + async JobTracker).
    // -----------------------------------------------------------------------

    json createChuckSessionSchema;
    createChuckSessionSchema["type"] = "object";
    createChuckSessionSchema["properties"]["slot_index"] = json::object({
        {"type", "integer"},
        {"minimum", 0},
        {"maximum", 15},
        {"description", "Tab/slot index [0-15] for the ChucK session"}
    });
    createChuckSessionSchema["properties"]["source"] = json::object({
        {"type", "string"},
        {"description", "ChucK source code (.ck)"}
    });
    createChuckSessionSchema["required"] = json::array({"slot_index", "source"});

    json createChuckSession;
    createChuckSession["name"] = "create_chuck_session";
    createChuckSession["description"] = "Create a ChucK session for a tab slot. "
        "Creates the session metadata without activating a live VM; the VM is "
        "allocated only when audition_chuck is called. "
        "Command maps to ControlInterface::handleCreateChuckSession.";
    createChuckSession["inputSchema"] = createChuckSessionSchema;
    tools.push_back(createChuckSession);

    json getChuckSessionSchema;
    getChuckSessionSchema["type"] = "object";
    getChuckSessionSchema["properties"]["session_id"] = json::object({
        {"type", "string"},
        {"description", "Session ID (e.g. 'ck:3') returned by create_chuck_session"}
    });
    getChuckSessionSchema["required"] = json::array({"session_id"});

    json getChuckSession;
    getChuckSession["name"] = "get_chuck_session";
    getChuckSession["description"] = "Get the current state of a ChucK session "
        "(session ID, source, VM state, diagnostics). "
        "Command maps to ControlInterface::handleGetChuckSession.";
    getChuckSession["inputSchema"] = getChuckSessionSchema;
    tools.push_back(getChuckSession);

    json compileChuckSchema;
    compileChuckSchema["type"] = "object";
    compileChuckSchema["properties"]["session_id"] = json::object({
        {"type", "string"},
        {"description", "Session ID (e.g. 'ck:3') to compile into"}
    });
    compileChuckSchema["properties"]["source"] = json::object({
        {"type", "string"},
        {"description", "ChucK source code to compile"}
    });
    compileChuckSchema["required"] = json::array({"session_id", "source"});

    json compileChuck;
    compileChuck["name"] = "compile_chuck";
    compileChuck["description"] = "Asynchronously compile ChucK source code for a session. "
        "Returns immediately with a job_id (status: queued); poll with get_chuck_job to "
        "check completion. Routes through ChuckSessionService::compileChuck → "
        "JobTracker → AudioEngine::startAsyncCkCompile() on the audio worker thread.";
    compileChuck["inputSchema"] = compileChuckSchema;
    tools.push_back(compileChuck);

    json auditionChuckSchema;
    auditionChuckSchema["type"] = "object";
    auditionChuckSchema["properties"]["session_id"] = json::object({
        {"type", "string"},
        {"description", "Session ID (e.g. 'ck:3') to audition"}
    });
    auditionChuckSchema["required"] = json::array({"session_id"});

    json auditionChuck;
    auditionChuck["name"] = "audition_chuck";
    auditionChuck["description"] = "Activate (audition) a ChucK session's VM. "
        "Activates only the specified session's VM; other sessions are unaffected. "
        "Command maps to ControlInterface::handleAuditionChuck.";
    auditionChuck["inputSchema"] = auditionChuckSchema;
    tools.push_back(auditionChuck);

    json stopChuckSchema;
    stopChuckSchema["type"] = "object";
    stopChuckSchema["properties"]["session_id"] = json::object({
        {"type", "string"},
        {"description", "Session ID (e.g. 'ck:3') to stop"}
    });
    stopChuckSchema["required"] = json::array({"session_id"});

    json stopChuck;
    stopChuck["name"] = "stop_chuck";
    stopChuck["description"] = "Stop (destroy) a ChucK session's VM. "
        "Destroys only the specified session's VM; other sessions are unaffected. "
        "Command maps to ControlInterface::handleStopChuck.";
    stopChuck["inputSchema"] = stopChuckSchema;
    tools.push_back(stopChuck);

    json jobIdSchema;
    jobIdSchema["type"] = "object";
    jobIdSchema["properties"]["job_id"] = json::object({
        {"type", "integer"},
        {"minimum", 0},
        {"description", "Job ID returned by compile_chuck"}
    });
    jobIdSchema["required"] = json::array({"job_id"});

    json getChuckJob;
    getChuckJob["name"] = "get_chuck_job";
    getChuckJob["description"] = "Query the status of an asynchronous ChucK compile job. "
        "Returns the canonical JobTracker schema: {ok, job_id, status, success, "
        "result.diagnostics, error}. Routes through ChuckSessionService::getJobStatus → "
        "JobTracker::queryJob.";
    getChuckJob["inputSchema"] = jobIdSchema;
    tools.push_back(getChuckJob);

    json cancelChuckJob;
    cancelChuckJob["name"] = "cancel_chuck_job";
    cancelChuckJob["description"] = "Cancel an in-flight asynchronous ChucK compile job. "
        "Routes through ChuckSessionService::cancelJob → JobTracker::cancelJob + "
        "AudioEngine::cancelCkJob (real backend cancellation via ck_cancel control plane).";
    cancelChuckJob["inputSchema"] = jobIdSchema;
    tools.push_back(cancelChuckJob);

    // -----------------------------------------------------------------------
    // AI-10 / J-5: Agentic workflow, working-set, changeset, indexing, and
    // lifecycle tools (Phase 3F: MCP Tool Expansion).
    // These map 1:1 to ControlInterface::dispatch() commands for the
    // working-set, changeset, project-indexing, and lifecycle subsystems.
    // -----------------------------------------------------------------------

    // working_set — inspect conversational working memory (AI-10.2)
    json workingSet;
    workingSet["name"] = "working_set";
    workingSet["description"] = "Inspect the conversational working set: tracked items, recorded changes, aliases, last intent, and active slot for the current agentic session";
    workingSet["inputSchema"] = noArgSchema;
    tools.push_back(workingSet);

    // resolve_reference — resolve a conversational reference to a project entity
    json resolveReferenceSchema;
    resolveReferenceSchema["type"] = "object";
    resolveReferenceSchema["properties"]["phrase"] = json::object({
        {"type", "string"},
        {"description", "The conversational reference text (e.g. \"it\", \"that bass\", \"the last change\", \"d1\")"}
    });
    resolveReferenceSchema["properties"]["intent_context"] = json::object({
        {"type", "string"},
        {"description", "Optional intent keyword hint for disambiguation (e.g. \"darker\", \"simpler\")"}
    });
    resolveReferenceSchema["required"] = json::array({"phrase"});

    json resolveReference;
    resolveReference["name"] = "resolve_reference";
    resolveReference["description"] = "Resolve a conversational reference phrase against the working set. Handles pronouns, aliases, named references, and slot references. Returns found/ambiguous/resolved status with candidates if ambiguous";
    resolveReference["inputSchema"] = resolveReferenceSchema;
    tools.push_back(resolveReference);

    // revert_change — revert the last reversible change
    json revertChangeSchema;
    revertChangeSchema["type"] = "object";
    revertChangeSchema["properties"]["change_id"] = json::object({
        {"type", "integer"},
        {"description", "Optional change id to revert; must match the last reversible change. If omitted, returns revert info without executing"}
    });

    json revertChange;
    revertChange["name"] = "revert_change";
    revertChange["description"] = "Get revert information for the last reversible change, or revert a specific change by id. When no change_id is given, returns revert info (not executed). When a matching change_id is given, the canonical revert is dispatched through the existing changeset/working-set backend";
    revertChange["inputSchema"] = revertChangeSchema;
    tools.push_back(revertChange);

    // clear_working_set — clear conversational memory
    json clearWorkingSet;
    clearWorkingSet["name"] = "clear_working_set";
    clearWorkingSet["description"] = "Clear the conversational working set (session-scoped memory). Removes all tracked items, changes, aliases, and context from the current agentic session";
    clearWorkingSet["inputSchema"] = noArgSchema;
    tools.push_back(clearWorkingSet);

    // changeset_status — query the active change-set
    json changesetStatus;
    changesetStatus["name"] = "changeset_status";
    changesetStatus["description"] = "Query the current change-set state: whether changes exist, operations staged/pending, status, and relevant identifiers/metadata. Returns null change-set if none is active";
    changesetStatus["inputSchema"] = noArgSchema;
    tools.push_back(changesetStatus);

    // changeset_preview — human-readable preview of pending changes
    json changesetPreview;
    changesetPreview["name"] = "changeset_preview";
    changesetPreview["description"] = "Get a human-readable structured preview of the active change-set (diff/preview). Preview-only — does not apply or modify any changes. Shows intent, status, and per-operation before/after summaries";
    changesetPreview["inputSchema"] = noArgSchema;
    tools.push_back(changesetPreview);

    // changeset_accept — finalise the active change-set
    json changesetAccept;
    changesetAccept["name"] = "changeset_accept";
    changesetAccept["description"] = "Accept (finalise) the active pending change-set. The mutations were already applied by the workflow; this transitions the change-set to accepted status without reapplying or writing files. Refuses to accept a change-set from a failed/cancelled workflow";
    changesetAccept["inputSchema"] = noArgSchema;
    tools.push_back(changesetAccept);

    // changeset_reject — reject/revert the entire pending change-set
    json changesetRejectSchema;
    changesetRejectSchema["type"] = "object";
    changesetRejectSchema["properties"]["confirm"] = json::object({
        {"type", "boolean"},
        {"description", "Must be true to authorize destructive revert actions. Preview does not grant authorization"}
    });

    json changesetReject;
    changesetReject["name"] = "changeset_reject";
    changesetReject["description"] = "Reject the active pending change-set: revert the ENTIRE change-set to pre-change state through the canonical AI-7/AI-6 revert paths. If destructive actions are involved, returns requires_confirmation without confirm=true. Does NOT silently accept or revert";
    changesetReject["inputSchema"] = changesetRejectSchema;
    tools.push_back(changesetReject);

    // changeset_undo — undo a previously accepted change-set
    json changesetUndoSchema;
    changesetUndoSchema["type"] = "object";
    changesetUndoSchema["properties"]["change_set_id"] = json::object({
        {"type", "integer"},
        {"description", "The id of the accepted change-set to undo"}
    });
    changesetUndoSchema["properties"]["confirm"] = json::object({
        {"type", "boolean"},
        {"description", "Must be true to authorize destructive revert actions"}
    });
    changesetUndoSchema["required"] = json::array({"change_set_id"});

    json changesetUndo;
    changesetUndo["name"] = "changeset_undo";
    changesetUndo["description"] = "Undo a previously accepted change-set: revert it to pre-change state through the canonical AI-7/AI-6 revert paths. Requires a change_set_id and confirmation for destructive actions";
    changesetUndo["inputSchema"] = changesetUndoSchema;
    tools.push_back(changesetUndo);

    // index_project — trigger project symbol index refresh (J-5)
    json indexProjectSchema;
    indexProjectSchema["type"] = "object";
    indexProjectSchema["properties"]["project_dir"] = json::object({
        {"type", "string"},
        {"description", "Optional project directory to reindex. If omitted, the current project directory is used"}
    });

    json indexProject;
    indexProject["name"] = "index_project";
    indexProject["description"] = "Trigger a project symbol index refresh for project-aware code completion (J-5). Routes through the canonical ProjectSymbolIndex via the ControlInterface";
    indexProject["inputSchema"] = indexProjectSchema;
    tools.push_back(indexProject);

    // ping — health/liveness check (Req 14.6)
    json ping;
    ping["name"] = "ping";
    ping["description"] = "Health/liveness check. Returns ok, command name, and end-to-end latency in milliseconds";
    ping["inputSchema"] = noArgSchema;
    tools.push_back(ping);

    // quit — signal clean shutdown (Req 16.5)
    json quit;
    quit["name"] = "quit";
    quit["description"] = "Signal clean shutdown of the Hathor process. The MCP server itself is NOT terminated — only the Hathor backend exits, following the existing ControlInterface lifecycle semantics";
    quit["inputSchema"] = noArgSchema;
    tools.push_back(quit);

    // -----------------------------------------------------------------------
    // AI-10: Agentic musical workflow orchestration (Phase 3E: MCP Tool Expansion)
    // These eight tools expose the AgenticWorkflow lifecycle through MCP.
    // Each maps directly to a ControlInterface::handleWorkflow* method.
    // The MCP layer forwards commands to ControlInterface which owns the
    // single authoritative AgenticWorkflow instance — MCP maintains no
    // competing workflow state machine.
    // -----------------------------------------------------------------------

    // workflow_start — schema
    json workflowStartSchema;
    workflowStartSchema["type"] = "object";
    workflowStartSchema["properties"]["intent"] = json::object({
        {"type", "string"},
        {"description", "Natural-language description of the desired musical result (e.g. 'dark 8-bar acid bassline')"}
    });
    workflowStartSchema["properties"]["target_slot"] = json::object({
        {"type", "string"},
        {"description", "Target pattern slot name (e.g. 'd1')"}
    });
    workflowStartSchema["properties"]["notation"] = json::object({
        {"type", "string"},
        {"description", "Pre-generated mini-notation pattern string (bypasses IntentPlanner generation)"}
    });
    workflowStartSchema["properties"]["ck_source"] = json::object({
        {"type", "string"},
        {"description", "Pre-written ChucK source code (bypasses IntentPlanner generation, uses ChucK workflow)"}
    });
    workflowStartSchema["properties"]["asset_name"] = json::object({
        {"type", "string"},
        {"description", "Asset name for rendering (e.g. 'acid_bass')"}
    });
    workflowStartSchema["properties"]["duration_bars"] = json::object({
        {"type", "integer"},
        {"description", "Render duration in bars"},
        {"default", 8}
    });
    workflowStartSchema["properties"]["dry_run"] = json::object({
        {"type", "boolean"},
        {"description", "If true, skip all persistent mutations (no confirmation boundary)"},
        {"default", false}
    });
    workflowStartSchema["properties"]["plan"] = json::object({
        {"type", "object"},
        {"description", "Optional pre-determined plan steps from workflow_plan"}
    });
    workflowStartSchema["required"] = json::array({"intent"});

    // workflow_approve / workflow_reject — shared schema
    json workflowConfirmSchema;
    workflowConfirmSchema["type"] = "object";
    workflowConfirmSchema["properties"]["request_id"] = json::object({
        {"type", "integer"},
        {"description", "Confirmation request ID (informational; backend tracks a single pending confirmation)"}
    });

    // workflow_plan — schema
    json workflowPlanSchema;
    workflowPlanSchema["type"] = "object";
    workflowPlanSchema["properties"]["intent"] = json::object({
        {"type", "string"},
        {"description", "Natural-language intent to plan (e.g. 'dark 8-bar acid bassline')"}
    });
    workflowPlanSchema["properties"]["target_slot"] = json::object({
        {"type", "string"},
        {"description", "Optional target slot name"}
    });
    workflowPlanSchema["properties"]["asset_name"] = json::object({
        {"type", "string"},
        {"description", "Optional asset name for rendering"}
    });
    workflowPlanSchema["required"] = json::array({"intent"});

    // workflow_repair — schema
    json workflowRepairSchema;
    workflowRepairSchema["type"] = "object";
    workflowRepairSchema["properties"]["feedback"] = json::object({
        {"type", "string"},
        {"description", "Conversational creative feedback (e.g. 'make it darker', 'too busy')"}
    });
    workflowRepairSchema["properties"]["intent_context"] = json::object({
        {"type", "string"},
        {"description", "Optional intent keyword hint for disambiguation (e.g. 'bass')"}
    });
    workflowRepairSchema["properties"]["dry_run"] = json::object({
        {"type", "boolean"},
        {"description", "If true, skip all persistent mutations"},
        {"default", false}
    });
    workflowRepairSchema["required"] = json::array({"feedback"});

    // workflow_replan — schema
    json workflowReplanSchema;
    workflowReplanSchema["type"] = "object";
    workflowReplanSchema["properties"]["intent"] = json::object({
        {"type", "string"},
        {"description", "New natural-language intent for the replanned workflow"}
    });
    workflowReplanSchema["properties"]["target_slot"] = json::object({
        {"type", "string"},
        {"description", "Target pattern slot name"}
    });
    workflowReplanSchema["properties"]["notation"] = json::object({
        {"type", "string"},
        {"description", "Pre-generated mini-notation pattern string"}
    });
    workflowReplanSchema["properties"]["ck_source"] = json::object({
        {"type", "string"},
        {"description", "Pre-written ChucK source code"}
    });
    workflowReplanSchema["properties"]["asset_name"] = json::object({
        {"type", "string"},
        {"description", "Asset name for rendering"}
    });
    workflowReplanSchema["properties"]["duration_bars"] = json::object({
        {"type", "integer"},
        {"description", "Render duration in bars"}
    });
    workflowReplanSchema["properties"]["dry_run"] = json::object({
        {"type", "boolean"},
        {"description", "If true, skip all persistent mutations"},
        {"default", false}
    });
    workflowReplanSchema["required"] = json::array({"intent"});

    // workflow_start tool entry
    json workflowStart;
    workflowStart["name"] = "workflow_start";
    workflowStart["description"] = "Start an agentic musical workflow: plan from intent, inspect project/song/assets, generate/modify pattern, validate, compile, audition, repair, render, bind asset, and update song. Routes through the canonical service layer (IntentPlanner, ProjectReadFacade, ChuckSessionService, RenderService, SongMutationService); persistent mutations pause at the confirmation boundary for explicit approval. Returns the initial workflow state; subsequent progress is streamed via the control channel";
    workflowStart["inputSchema"] = workflowStartSchema;
    tools.push_back(workflowStart);

    // workflow_cancel tool entry
    json workflowCancel;
    workflowCancel["name"] = "workflow_cancel";
    workflowCancel["description"] = "Cancel the currently running workflow. Sets a stop flag that the workflow thread checks between steps; active async jobs (compile, render) are cancelled through their canonical cancellation paths. Rejects if no workflow is running";
    workflowCancel["inputSchema"] = noArgSchema;
    tools.push_back(workflowCancel);

    // workflow_status tool entry
    json workflowStatus;
    workflowStatus["name"] = "workflow_status";
    workflowStatus["description"] = "Query the current workflow state: state, current step, completed steps, diagnostics, render status, pending confirmation request, applied changes, and error. Returns idle state if no workflow is active";
    workflowStatus["inputSchema"] = noArgSchema;
    tools.push_back(workflowStatus);

    // workflow_approve tool entry
    json workflowApprove;
    workflowApprove["name"] = "workflow_approve";
    workflowApprove["description"] = "Approve the pending confirmation request for a destructive operation (e.g. commit_rendered_asset, edit_song). Resumes the workflow after the confirmation boundary. The request_id is optional and informational — the backend tracks a single pending confirmation";
    workflowApprove["inputSchema"] = workflowConfirmSchema;
    tools.push_back(workflowApprove);

    // workflow_reject tool entry
    json workflowReject;
    workflowReject["name"] = "workflow_reject";
    workflowReject["description"] = "Reject the pending confirmation request for a destructive operation. Fails the current step and transitions the workflow to Failed. The request_id is optional and informational";
    workflowReject["inputSchema"] = workflowConfirmSchema;
    tools.push_back(workflowReject);

    // workflow_plan tool entry
    json workflowPlan;
    workflowPlan["name"] = "workflow_plan";
    workflowPlan["description"] = "Generate a structured plan from a natural-language request before heavy execution (AI-10.1 IntentPlanner). Returns the plan JSON that can be passed to workflow_start via the 'plan' parameter. Does not start a workflow";
    workflowPlan["inputSchema"] = workflowPlanSchema;
    tools.push_back(workflowPlan);

    // workflow_repair tool entry
    json workflowRepair;
    workflowRepair["name"] = "workflow_repair";
    workflowRepair["description"] = "Start a creative-repair workflow from conversational feedback (AI-10.5). Resolves the target against the working set, classifies the feedback, and applies the smallest targeted mutation through the canonical services. Requires a feedback string";
    workflowRepair["inputSchema"] = workflowRepairSchema;
    tools.push_back(workflowRepair);

    // workflow_replan tool entry
    json workflowReplan;
    workflowReplan["name"] = "workflow_replan";
    workflowReplan["description"] = "Restart the active workflow with a new request (AI-10.6). Swaps the current request, cancels the active change-set, and signals the workflow thread to restart from planning via the WaitingForUser state. Rejected if the workflow is idle or terminal";
    workflowReplan["inputSchema"] = workflowReplanSchema;
    tools.push_back(workflowReplan);

    json result;
    result["tools"] = tools;
    return result;
}

// ---------------------------------------------------------------------------
// Tool call -> Hathor command string
// ---------------------------------------------------------------------------

/** Build the plain-text Hathor command for a tools/call request.
 *  Returns an empty string if the tool name is unknown or arguments are missing. */
static std::string buildHathorCommand(const std::string& toolName, const json& args)
{
    if (toolName == "set_pattern")
    {
        if (!args.contains("slot") || !args.contains("notation"))
            return {};
        const std::string slot     = args["slot"].get<std::string>();
        const std::string notation = args["notation"].get<std::string>();
        return "set-pattern " + slot + " " + notation;
    }
    if (toolName == "bpm")
    {
        if (!args.contains("value"))
            return {};
        double v = args["value"].get<double>();
        char buf[64];
        if (v == static_cast<double>(static_cast<long>(v)))
            std::snprintf(buf, sizeof(buf), "bpm %ld", static_cast<long>(v));
        else
            std::snprintf(buf, sizeof(buf), "bpm %g", v);
        return buf;
    }
    if (toolName == "play")
        return "play";
    if (toolName == "stop")
        return "stop";
    if (toolName == "set_gain")
    {
        if (!args.contains("value"))
            return {};
        double v = args["value"].get<double>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "set-gain %g", v);
        return buf;
    }
    if (toolName == "edit_song")
    {
        if (!args.contains("song_file") || !args.contains("ops"))
            return {};
        const std::string songFile = args["song_file"].get<std::string>();
        const std::string opsJson = args["ops"].dump();
        return "edit_song " + songFile + " " + opsJson;
    }
    if (toolName == "get_context")
    {
        // Forward the arguments as a JSON object to the Hathor process.
        // The ControlInterface parses this JSON into a ContextRequest.
        // We pass the raw args JSON (minus any empty fields to keep the
        // command line compact).
        json filtered = json::object();
        if (args.contains("file") && args["file"].is_string())
            filtered["file"] = args["file"];
        if (args.contains("line") && args["line"].is_number_integer())
            filtered["line"] = args["line"];
        if (args.contains("character") && args["character"].is_number_integer())
            filtered["character"] = args["character"];
        if (args.contains("language") && args["language"].is_string())
            filtered["language"] = args["language"];
        if (args.contains("selected_text") && args["selected_text"].is_string())
            filtered["selected_text"] = args["selected_text"];
        if (args.contains("scope") && args["scope"].is_array())
            filtered["scope"] = args["scope"];
        if (args.contains("include_content") && args["include_content"].is_boolean())
            filtered["include_content"] = args["include_content"];
        if (args.contains("max_content_length") && args["max_content_length"].is_number_integer())
            filtered["max_content_length"] = args["max_content_length"];

        return "get-context " + filtered.dump();
    }
    if (toolName == "slot-play")
    {
        if (!args.contains("slot"))
            return {};
        const std::string slot = args["slot"].get<std::string>();
        return "slot-play " + slot;
    }
    if (toolName == "slot-stop")
    {
        if (!args.contains("slot"))
            return {};
        const std::string slot = args["slot"].get<std::string>();
        return "slot-stop " + slot;
    }
    if (toolName == "set-eq-preset")
    {
        if (!args.contains("preset"))
            return {};
        const std::string preset = args["preset"].get<std::string>();
        return "set-eq-preset " + preset;
    }
    if (toolName == "clear-pattern")
    {
        if (args.contains("slot"))
            return "clear-pattern " + args["slot"].get<std::string>();
        return "clear-pattern";
    }
    if (toolName == "list-patterns")
        return "list-patterns";
    if (toolName == "list-samples")
        return "list-samples";
    if (toolName == "inspect_project")
        return "inspect_project";
    if (toolName == "get_current_song")
        return "get_current_song";
    if (toolName == "list_assets")
        return "list_assets";
    if (toolName == "list_samples")
        return "list_samples";
    if (toolName == "list_chuck_instruments")
    {
        if (args.contains("project_dir") && args["project_dir"].is_string())
            return "list_chuck_instruments " + args["project_dir"].get<std::string>();
        return "list_chuck_instruments";
    }
    if (toolName == "get_diagnostics")
    {
        if (!args.contains("source_id") || !args["source_id"].is_string()
            || !args.contains("is_chuck") || !args["is_chuck"].is_boolean()
            || !args.contains("content") || !args["content"].is_string())
            return {};
        const std::string sourceId = args["source_id"].get<std::string>();
        const bool isChuck = args["is_chuck"].get<bool>();
        const std::string content = args["content"].get<std::string>();
        return "get_diagnostics " + sourceId + " " + (isChuck ? "true" : "false") + " " + content;
    }
    if (toolName == "get_audio_status")
        return "get_audio_status";

    // -----------------------------------------------------------------------
    // AI-5: ChucK session lifecycle (Phase 3C: MCP Tool Expansion)
    // Maps directly to ControlInterface::handleChuckSessionCommand dispatch.
    // -----------------------------------------------------------------------

    if (toolName == "create_chuck_session")
    {
        if (!args.contains("slot_index") || !args["slot_index"].is_number_integer()
            || !args.contains("source") || !args["source"].is_string())
            return {};
        int slotIdx = args["slot_index"].get<int>();
        const std::string source = args["source"].get<std::string>();
        return "create_chuck_session " + std::to_string(slotIdx) + " " + source;
    }
    if (toolName == "get_chuck_session")
    {
        if (!args.contains("session_id") || !args["session_id"].is_string())
            return {};
        const std::string sessionId = args["session_id"].get<std::string>();
        return "get_chuck_session " + sessionId;
    }
    if (toolName == "compile_chuck")
    {
        if (!args.contains("session_id") || !args["session_id"].is_string()
            || !args.contains("source") || !args["source"].is_string())
            return {};
        const std::string sessionId = args["session_id"].get<std::string>();
        const std::string source = args["source"].get<std::string>();
        return "compile_chuck " + sessionId + " " + source;
    }
    if (toolName == "audition_chuck")
    {
        if (!args.contains("session_id") || !args["session_id"].is_string())
            return {};
        const std::string sessionId = args["session_id"].get<std::string>();
        return "audition_chuck " + sessionId;
    }
    if (toolName == "stop_chuck")
    {
        if (!args.contains("session_id") || !args["session_id"].is_string())
            return {};
        const std::string sessionId = args["session_id"].get<std::string>();
        return "stop_chuck " + sessionId;
    }
    if (toolName == "get_chuck_job")
    {
        if (!args.contains("job_id") || !args["job_id"].is_number_integer())
            return {};
        uint64_t jobId = args["job_id"].get<uint64_t>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "get_chuck_job %llu",
                      static_cast<unsigned long long>(jobId));
        return buf;
    }
    if (toolName == "cancel_chuck_job")
    {
        if (!args.contains("job_id") || !args["job_id"].is_number_integer())
            return {};
        uint64_t jobId = args["job_id"].get<uint64_t>();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "cancel_chuck_job %llu",
                      static_cast<unsigned long long>(jobId));
        return buf;
    }

    // -----------------------------------------------------------------------
    // AI-10 / J-5: Agentic workflow, working-set, changeset, indexing, and
    // lifecycle tools (Phase 3F: MCP Tool Expansion)
    // These map 1:1 to ControlInterface::dispatch() commands.
    // -----------------------------------------------------------------------

    if (toolName == "working_set")
        return "working_set";
    if (toolName == "resolve_reference")
    {
        if (!args.contains("phrase") || !args["phrase"].is_string())
            return {};
        std::string cmd = "resolve_reference " + args["phrase"].get<std::string>();
        if (args.contains("intent_context") && args["intent_context"].is_string())
            cmd += " " + args["intent_context"].get<std::string>();
        return cmd;
    }
    if (toolName == "revert_change")
    {
        if (args.contains("change_id") && args["change_id"].is_number_integer())
            return "revert_change " + std::to_string(args["change_id"].get<int>());
        return "revert_change";
    }
    if (toolName == "clear_working_set")
        return "clear_working_set";
    if (toolName == "changeset_status")
        return "changeset_status";
    if (toolName == "changeset_preview")
        return "changeset_preview";
    if (toolName == "changeset_accept")
        return "changeset_accept";
    if (toolName == "changeset_reject")
    {
        std::string cmd = "changeset_reject";
        if (args.contains("confirm") && args["confirm"].is_boolean() && args["confirm"].get<bool>())
            cmd += " confirm";
        return cmd;
    }
    if (toolName == "changeset_undo")
    {
        if (!args.contains("change_set_id") || !args["change_set_id"].is_number_integer())
            return {};
        std::string cmd = "changeset_undo " + std::to_string(args["change_set_id"].get<int>());
        if (args.contains("confirm") && args["confirm"].is_boolean() && args["confirm"].get<bool>())
            cmd += " confirm";
        return cmd;
    }
    if (toolName == "index_project")
    {
        std::string cmd = "index_project";
        if (args.contains("project_dir") && args["project_dir"].is_string())
            cmd += " " + args["project_dir"].get<std::string>();
        return cmd;
    }
    if (toolName == "ping")
        return "ping";
    if (toolName == "quit")
        return "quit";

    // -----------------------------------------------------------------------
    // AI-10: Agentic musical workflow orchestration (Phase 3E: MCP Tool Expansion)
    // Each branch maps the MCP tool call to the canonical ControlInterface
    // workflow command string (workflow_<verb>). The backend owns all
    // workflow state — MCP only forwards the request.
    // -----------------------------------------------------------------------

    if (toolName == "workflow_start")
    {
        if (!args.contains("intent") || !args["intent"].is_string())
            return {};
        return "workflow_start " + args.dump();
    }
    if (toolName == "workflow_cancel")
        return "workflow_cancel";
    if (toolName == "workflow_status")
        return "workflow_status";
    if (toolName == "workflow_approve" || toolName == "workflow_reject")
    {
        // Both approve and reject route through handleWorkflowApprove(rest, approved).
        // The request_id is optional and informational — the backend validates it
        // against the single pending confirmation, then calls respondToConfirmation.
        const std::string cmd = (toolName == "workflow_approve")
            ? "workflow_approve" : "workflow_reject";
        if (args.contains("request_id") && args["request_id"].is_number_integer())
            return cmd + " " + std::to_string(args["request_id"].get<int>());
        return cmd;
    }
    if (toolName == "workflow_plan")
    {
        // Format: workflow_plan <intent> [json-kwargs]
        // The intent is a free-form string (may contain spaces); the optional
        // JSON kwargs carry target_slot and asset_name. The ControlInterface
        // handler splits on the first '{' to separate intent from JSON.
        if (!args.contains("intent") || !args["intent"].is_string())
            return {};
        const std::string intent = args["intent"].get<std::string>();
        json kwargs = json::object();
        if (args.contains("target_slot") && args["target_slot"].is_string())
            kwargs["target_slot"] = args["target_slot"];
        if (args.contains("asset_name") && args["asset_name"].is_string())
            kwargs["asset_name"] = args["asset_name"];
        if (kwargs.is_object() && kwargs.empty())
            return "workflow_plan " + intent;
        return "workflow_plan " + intent + " " + kwargs.dump();
    }
    if (toolName == "workflow_repair")
    {
        if (!args.contains("feedback") || !args["feedback"].is_string())
            return {};
        return "workflow_repair " + args.dump();
    }
    if (toolName == "workflow_replan")
    {
        if (!args.contains("intent") || !args["intent"].is_string())
            return {};
        return "workflow_replan " + args.dump();
    }
    return {};
}

// ---------------------------------------------------------------------------
// MCP response helpers
// ---------------------------------------------------------------------------

static void sendJson(const json& obj)
{
    std::cout << obj.dump() << "\n";
}

static json makeErrorResponse(const json& id, int code, const std::string& message)
{
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"error", json{{"code", code}, {"message", message}}}};
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

int main()
{
    // Unbuffered stdout so the agent receives responses immediately.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Ignore SIGPIPE -- handle write errors explicitly.
    std::signal(SIGPIPE, SIG_IGN);

    // Determine socket path from environment.
    const char* socketPath = std::getenv("HATHOR_SOCKET_PATH");
    if (socketPath == nullptr || socketPath[0] == '\0')
    {
        std::fprintf(stderr,
                     "hathor-mcp: HATHOR_SOCKET_PATH environment variable is not set.\n");
        return 1;
    }

    // Connect to Hathor Unix socket.
    int sockFd = connectUnixSocket(socketPath);
    if (sockFd < 0)
        return 1;

    // MCP JSON-RPC stdio loop.
    std::string line;
    while (std::getline(std::cin, line))
    {
        if (line.empty())
            continue;

        // Parse incoming JSON-RPC message.
        json req;
        try
        {
            req = json::parse(line);
        }
        catch (const json::parse_error& e)
        {
            sendJson(json{{"jsonrpc", "2.0"},
                          {"id", nullptr},
                          {"error",
                           json{{"code", -32700},
                                {"message", std::string("Parse error: ") + e.what()}}}});
            continue;
        }

        // Extract method -- skip if absent (malformed message).
        if (!req.contains("method") || !req["method"].is_string())
            continue;

        const std::string method = req["method"].get<std::string>();

        // Notifications have no "id" field -- no response needed.
        bool isNotification = !req.contains("id");

        // ----------------------------------------------------------------
        // initialize
        // ----------------------------------------------------------------
        if (method == "initialize")
        {
            if (isNotification)
                continue;
            sendJson(json{
                {"jsonrpc", "2.0"},
                {"id", req["id"]},
                {"result",
                 json{{"protocolVersion", "2024-11-05"},
                      {"capabilities", json{{"tools", json::object()}}},
                      {"serverInfo", json{{"name", "hathor-mcp"}, {"version", "1.0.0"}}}}}});
        }
        // ----------------------------------------------------------------
        // notifications/initialized  (no response)
        // ----------------------------------------------------------------
        else if (method == "notifications/initialized")
        {
            // Notification -- no response required.
        }
        // ----------------------------------------------------------------
        // tools/list
        // ----------------------------------------------------------------
        else if (method == "tools/list")
        {
            if (isNotification)
                continue;
            sendJson(json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", makeToolsList()}});
        }
        // ----------------------------------------------------------------
        // tools/call
        // ----------------------------------------------------------------
        else if (method == "tools/call")
        {
            if (isNotification)
                continue;

            const json& id = req["id"];

            // Validate params.
            if (!req.contains("params") || !req["params"].is_object()
                || !req["params"].contains("name"))
            {
                sendJson(makeErrorResponse(id, -32602, "Invalid params: missing tool name"));
                continue;
            }

            const std::string toolName = req["params"]["name"].get<std::string>();
            const json        args     = req["params"].contains("arguments")
                                             ? req["params"]["arguments"]
                                             : json::object();

            // Build the Hathor command string.
            std::string cmd = buildHathorCommand(toolName, args);
            if (cmd.empty())
            {
                sendJson(json{
                    {"jsonrpc", "2.0"},
                    {"id", id},
                    {"result",
                     json{{"content",
                           json::array({json{{"type", "text"},
                                            {"text",
                                             "unknown tool or invalid arguments: " + toolName}}})},
                          {"isError", true}}}});
                continue;
            }

            // Forward command to Hathor over the Unix socket.
            if (sockFd < 0 || !sendCommand(sockFd, cmd))
            {
                sendJson(json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result",
                               json{{"content",
                                     json::array({json{{"type", "text"},
                                                       {"text", "hathor socket write error"}}})},
                                    {"isError", true}}}});
                if (sockFd >= 0)
                {
                    ::close(sockFd);
                    sockFd = -1;
                }
                continue;
            }

            // Wait up to 5 seconds for the response.
            bool        timedOut     = false;
            std::string responseLine = readLine(sockFd, 5000, timedOut);

            if (timedOut)
            {
                sendJson(json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result",
                               json{{"content",
                                     json::array({json{{"type", "text"},
                                                       {"text", "hathor tool call timed out"}}})},
                                    {"isError", true}}}});
                continue;
            }

            if (responseLine.empty())
            {
                sendJson(json{{"jsonrpc", "2.0"},
                              {"id", id},
                              {"result",
                               json{{"content",
                                     json::array({json{{"type", "text"},
                                                       {"text", "hathor socket read error"}}})},
                                    {"isError", true}}}});
                continue;
            }

            // Parse the Hathor response to determine success or failure.
            bool isError = false;
            try
            {
                json hathorResp = json::parse(responseLine);
                if (hathorResp.contains("ok") && hathorResp["ok"].is_boolean()
                    && !hathorResp["ok"].get<bool>())
                {
                    isError = true;
                }
            }
            catch (...)
            {
                // Non-JSON response -- treat as error.
                isError = true;
            }

            sendJson(json{
                {"jsonrpc", "2.0"},
                {"id", id},
                {"result",
                 json{{"content",
                       json::array({json{{"type", "text"}, {"text", responseLine}}})},
                      {"isError", isError}}}});
        }
        // ----------------------------------------------------------------
        // Unknown method
        // ----------------------------------------------------------------
        else
        {
            if (!isNotification)
                sendJson(makeErrorResponse(req["id"], -32601, "Method not found: " + method));
        }
    }

    // stdin EOF -- agent disconnected.
    if (sockFd >= 0)
        ::close(sockFd);

    return 0;
}
