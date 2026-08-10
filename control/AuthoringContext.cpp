// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * AuthoringContext.cpp — AI-8 context assembler implementation.
 *
 * Dynamically assembles a targeted JSON context payload from:
 *   - EditorContextProvider  → current file, cursor, selection (editor state)
 *   - ProjectReadFacade      → project, samples, instruments, runtime, diagnostics
 *   - LanguageMetadata       → versioned supported-surface definitions
 *   - LspContextProvider     → LSP-derived diagnostics, completions, hover
 *
 * Requirement references: AI-8 §1–§10
 */

#include "AuthoringContext.hpp"
#include "ProjectReadFacade.hpp"

#include "hathor/LanguageMetadata.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

namespace hathor::control {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

/// Current ISO-8601 UTC timestamp.
std::string isoTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

/// Detect language from a file path extension.
std::string languageFromPath(std::string_view path)
{
    const std::string ext = [path] {
        const auto pos = path.rfind('.');
        if (pos == std::string_view::npos)
            return std::string{};
        return std::string(path.substr(pos + 1));
    }();
    if (ext == "hathor") return "mininotation";
    if (ext == "ck")     return "chuck";
    return "unknown";
}

} // anonymous namespace
/// "auto" mode (empty scope) includes it.
bool scopeIncludes(const std::vector<std::string>& scope,
                   bool autoMode,
                   const std::string& section)
{
    if (autoMode)
        return true; // auto mode includes all relevant sections
    return std::find(scope.begin(), scope.end(), section) != scope.end();
}

/// Truncate a string to maxLen, appending "..." if truncated.
std::string truncateContent(std::string_view content, int maxLen)
{
    if (maxLen <= 0 || static_cast<int>(content.size()) <= maxLen)
        return std::string(content);
     return std::string(content.substr(0, static_cast<std::size_t>(maxLen))) + "...[truncated]";
 }

/// Check if a scope string is in the requested scope list, or if

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

AuthoringContext::AuthoringContext(
    ProjectReadFacade&                          readFacade,
    EditorContextProvider*                    editorCtx,
    LspContextProvider*                       lspCtx,
    const hathor::language::LanguageMetadata*    metadata,
    const hathor::language::MetadataCompatibility* compat)
    : readFacade_(readFacade)
    , editorCtx_(editorCtx)
    , lspCtx_(lspCtx)
    , metadata_(metadata)
    , compat_(compat)
{
}

// ---------------------------------------------------------------------------
// setMetadata — hot-reload support
// ---------------------------------------------------------------------------

void AuthoringContext::setMetadata(
    const hathor::language::LanguageMetadata* metadata,
    const hathor::language::MetadataCompatibility* compat)
{
    metadata_ = metadata;
    compat_ = compat;
}

// ---------------------------------------------------------------------------
// resolveSnapshot — get editor state
// ---------------------------------------------------------------------------

EditorContextSnapshot AuthoringContext::resolveSnapshot(const ContextRequest& req) const
{
    if (editorCtx_ != nullptr)
    {
        auto snap = editorCtx_->snapshot();
        // Override cursor position if specified in the request.
        if (req.line)       snap.cursorLine = *req.line;
        if (req.character)  snap.cursorChar = *req.character;
        return snap;
    }
    // No editor provider — return an empty snapshot.
    EditorContextSnapshot empty;
    empty.capturedAt = isoTimestamp();
    if (req.line)       empty.cursorLine = *req.line;
    if (req.character)  empty.cursorChar = *req.character;
    if (!req.file.empty())
        empty.file = req.file;
    if (!req.language.empty())
        empty.language = req.language;
    if (!req.selectedText.empty())
    {
        empty.hasSelection = true;
        empty.selectedText = req.selectedText;
    }
    empty.hasContent = !req.file.empty() || !req.language.empty();
    return empty;
}

// ---------------------------------------------------------------------------
// inferLanguage — determine the language for the given file/snapshot
// ---------------------------------------------------------------------------

std::string AuthoringContext::inferLanguage(
    std::string_view file,
    const ContextRequest& req,
    const EditorContextSnapshot& snap) const
{
    // Explicit override from request.
    if (!req.language.empty())
        return req.language;

    // From the request file path.
    if (!file.empty())
        return languageFromPath(file);

    // From the editor snapshot.
    if (!snap.language.empty())
        return snap.language;

    // From the editor snapshot file.
    if (!snap.file.empty())
        return languageFromPath(snap.file);

    return "unknown";
}

// ---------------------------------------------------------------------------
// resolveScope — determine which sections to include
// ---------------------------------------------------------------------------

std::vector<std::string> AuthoringContext::resolveScope(
    const ContextRequest& req,
    std::string_view language) const
{
    const bool autoMode = req.scope.empty();

    if (!autoMode)
        return req.scope;

    // Auto-mode: include sections relevant to the language.
    std::vector<std::string> sections = {"editor", "diagnostics", "runtime", "project"};

    if (language == "mininotation")
    {
        sections.push_back("metadata");
        sections.push_back("samples");
        sections.push_back("lsp");
    }
    else if (language == "chuck")
    {
        sections.push_back("metadata");
        sections.push_back("instruments");
        sections.push_back("lsp");
    }
    else
    {
        // Unknown language — include everything we can.
        sections = {"editor", "diagnostics", "runtime", "project",
                    "metadata", "samples", "instruments", "lsp"};
    }

    return sections;
}

// ---------------------------------------------------------------------------
// assemble — main entry point
// ---------------------------------------------------------------------------

nlohmann::json AuthoringContext::assemble(const ContextRequest& req) const
{
    const auto snap = resolveSnapshot(req);
    const std::string language = inferLanguage(req.file, req, snap);
    const auto sectionsToInclude = resolveScope(req, language);

    nlohmann::json response;
    response["ok"] = true;
    response["version"] = isoTimestamp();

    // --- Metadata version identification ---
    nlohmann::json metaVer;
    if (metadata_ != nullptr)
    {
        metaVer["schema"] = metadata_->schemaVersion;
        metaVer["engine"] = metadata_->hathorEngineCompat;
        metaVer["strudel"] = metadata_->strudelMiniNotationCompat;
        metaVer["chuck"] = metadata_->chuckLibVersion;
        metaVer["surface"] = metadata_->chuckIntegrationSurface;
        if (metadata_->consumer)  metaVer["consumer"] = *metadata_->consumer;
        if (metadata_->loadedAt)  metaVer["loaded_at"] = *metadata_->loadedAt;
    }
    else
    {
        metaVer["available"] = false;
        metaVer["reason"] = "LanguageMetadata not loaded";
    }
    // Compatibility flag
    if (compat_ != nullptr)
    {
        metaVer["compatible"] = compat_->compatible;
        if (!compat_->compatible)
        {
            metaVer["errors"] = nlohmann::json::array();
            for (const auto& e : compat_->errors)
                metaVer["errors"].push_back(e);
        }
    }
    response["metadata_version"] = std::move(metaVer);

    // --- Assemble requested sections ---
    nlohmann::json sectionsJson = nlohmann::json::object();

    for (const auto& name : sectionsToInclude)
    {
        if (name == "editor")
            sectionsJson["editor"] = assembleEditor(req, snap, language);
        else if (name == "diagnostics")
            sectionsJson["diagnostics"] = assembleDiagnostics(req, snap, language);
        else if (name == "metadata")
            sectionsJson["metadata"] = assembleMetadata(req, language);
        else if (name == "runtime")
            sectionsJson["runtime"] = assembleRuntime(req);
        else if (name == "samples")
            sectionsJson["samples"] = assembleSamples(req);
        else if (name == "instruments")
            sectionsJson["instruments"] = assembleInstruments(req);
        else if (name == "lsp")
            sectionsJson["lsp"] = assembleLsp(req, snap, language);
        else if (name == "project")
            sectionsJson["project"] = assembleProject(req);
    }

    response["sections"] = std::move(sectionsJson);
    return response;
}

// ---------------------------------------------------------------------------
// assembleEditor — file, cursor, selection, (optional) content
// ---------------------------------------------------------------------------

nlohmann::json AuthoringContext::assembleEditor(
    const ContextRequest& req,
    const EditorContextSnapshot& snap,
    std::string_view language) const
{
    nlohmann::json editor;

    if (!snap.hasContent)
    {
        // No active editor — return a minimal object.
        editor["active"] = false;
        editor["file"] = nullptr;
        editor["uri"] = nullptr;
        editor["language"] = std::string(language);
        return editor;
    }

    editor["active"] = true;
    editor["file"] = snap.file.empty() ? nullptr : nlohmann::json(snap.file);
    editor["uri"] = snap.uri.empty() ? nullptr : nlohmann::json(snap.uri);
    editor["language"] = snap.language.empty() ? std::string(language) : snap.language;

    // Cursor position
    editor["cursor"] = nlohmann::json{
        {"line", snap.cursorLine},
        {"character", snap.cursorChar}
    };

    // Selection
    editor["hasSelection"] = snap.hasSelection;
    if (snap.hasSelection)
    {
        editor["selection"] = nlohmann::json{
            {"start_line", snap.selStartLine},
            {"start_char", snap.selStartChar},
            {"end_line",   snap.selEndLine},
            {"end_char",   snap.selEndChar},
            {"text",       snap.selectedText}
        };
    }

    // Pattern/slot context
    if (!snap.slotName.empty())
    {
        editor["pattern"] = nlohmann::json{
            {"slot_name", snap.slotName},
            {"slot_index", snap.slotIndex},
        };
        if (snap.frontMatterSlot.empty())
            editor["pattern"]["slot_name"] = snap.slotName;
        else
        {
            editor["pattern"]["slot_name"] = snap.slotName;
            editor["pattern"]["front_matter_slot"] = snap.frontMatterSlot;
        }
        if (snap.frontMatterBpm > 0.0)
            editor["pattern"]["front_matter_bpm"] = snap.frontMatterBpm;
        if (!snap.frontMatterBank.empty())
            editor["pattern"]["front_matter_bank"] = snap.frontMatterBank;
    }

    // Content (only if requested)
    if (req.includeContent)
    {
        std::string_view content = snap.content;
        if (req.maxContentLength > 0 &&
            static_cast<int>(content.size()) > req.maxContentLength)
        {
            editor["content"] = truncateContent(content, req.maxContentLength);
            editor["content_truncated"] = true;
        }
        else
        {
            editor["content"] = std::string(content);
            editor["content_truncated"] = false;
        }
    }
    else
    {
        editor["content"] = nullptr;
    }

    editor["snapshot_timestamp"] = snap.capturedAt;

    return editor;
}

// ---------------------------------------------------------------------------
// assembleDiagnostics — from LSP + compiler
// ---------------------------------------------------------------------------

nlohmann::json AuthoringContext::assembleDiagnostics(
    const ContextRequest& req,
    const EditorContextSnapshot& snap,
    std::string_view language) const
{
    nlohmann::json result;
    result["sources"] = nlohmann::json::array();

    const bool isChuck = (language == "chuck");

    // --- 1. LSP diagnostics (if available) ---
    if (lspCtx_ != nullptr)
    {
        auto status = lspCtx_->lspStatus();
        result["lsp_available"] = status.value("ok", false);

        if (status.value("ok", false) && !snap.uri.empty())
        {
            auto lspDiags = lspCtx_->diagnosticsForDocument(snap.uri);
            result["lsp_diagnostics"] = lspDiags.value("diagnostics", nlohmann::json::array());
            result["sources"].push_back("lsp");
        }
        else
        {
            result["lsp_diagnostics"] = nlohmann::json::array();
            if (!snap.uri.empty() && !status.value("ok", false))
                result["lsp_reason"] = status.value("reason", "LSP unavailable");
        }
    }
    else
    {
        result["lsp_available"] = false;
        result["lsp_diagnostics"] = nlohmann::json::array();
    }

    // --- 2. Compiler diagnostics (from the real parser/compiler infrastructure) ---
    // Use ProjectReadFacade::getDiagnostics() which routes through the real
    // parseMini() / validateChuckSource() — NOT a parallel parser.
    const std::string content = req.includeContent
        ? snap.content
        : (snap.content.empty() ? "" : snap.content.substr(0, 8192));

    if (!content.empty())
    {
        const std::string sourceId = snap.uri.empty()
            ? snap.slotName.empty() ? "untitled" : ("slot:" + snap.slotName)
            : snap.uri;

        auto compilerDiags = readFacade_.getDiagnostics(content, sourceId, isChuck);
        result["compiler_diagnostics"] = compilerDiags.value("diagnostics", nlohmann::json::array());
        result["sources"].push_back(isChuck ? "chuck_compiler" : "miniparser");
    }
    else
    {
        result["compiler_diagnostics"] = nlohmann::json::array();
    }

    return result;
}

// ---------------------------------------------------------------------------
// assembleMetadata — supported surface definitions (AI-3)
// ---------------------------------------------------------------------------

nlohmann::json AuthoringContext::assembleMetadata(
    const ContextRequest& /*req*/,
    std::string_view language) const
{
    nlohmann::json result;

    if (metadata_ == nullptr)
    {
        result["available"] = false;
        result["reason"] = "LanguageMetadata not loaded";
        return result;
    }

    if (compat_ != nullptr && !compat_->compatible)
    {
        result["available"] = false;
        result["reason"] = "Metadata version incompatible with running engine";
        result["errors"] = compat_->errors;
        return result;
    }

    result["available"] = true;
    result["schema_version"] = metadata_->schemaVersion;
    result["hathor_engine_compat"] = metadata_->hathorEngineCompat;
    result["strudel_mini_notation_compat"] = metadata_->strudelMiniNotationCompat;
    result["chuck_lib_version"] = metadata_->chuckLibVersion;
    result["chuck_integration_surface"] = metadata_->chuckIntegrationSurface;

    // Language-specific definitions
    if (language == "mininotation")
    {
        nlohmann::json funcs = nlohmann::json::array();
        for (const auto& fn : metadata_->functions)
        {
            if (!fn.supported)
                continue;
            nlohmann::json f;
            f["name"] = fn.name;
            f["signature"] = fn.signature;
            f["category"] = fn.category;
            if (fn.example) f["example"] = *fn.example;
            funcs.push_back(std::move(f));
        }
        result["functions"] = std::move(funcs);

        nlohmann::json ops = nlohmann::json::array();
        for (const auto& op : metadata_->operators)
        {
            nlohmann::json o;
            o["name"] = op.name;
            o["description"] = op.description;
            o["example"] = op.example;
            ops.push_back(std::move(o));
        }
        result["operators"] = std::move(ops);

        nlohmann::json grammar = nlohmann::json::array();
        for (const auto& ge : metadata_->grammar)
        {
            if (!ge.supported)
                continue;
            nlohmann::json g;
            g["name"] = ge.name;
            g["syntax"] = ge.syntax;
            g["example"] = ge.example;
            grammar.push_back(std::move(g));
        }
        result["grammar"] = std::move(grammar);

        nlohmann::json params = nlohmann::json::array();
        for (const auto& pd : metadata_->params)
        {
            if (!pd.supported)
                continue;
            nlohmann::json p;
            p["key"] = pd.key;
            p["value_type"] = pd.valueType;
            p["description"] = pd.description;
            params.push_back(std::move(p));
        }
        result["params"] = std::move(params);

        // Sample definitions from metadata
        nlohmann::json sampleDefs = nlohmann::json::array();
        for (const auto& sd : metadata_->samples)
        {
            nlohmann::json s;
            s["name"] = sd.name;
            s["category"] = sd.category;
            sampleDefs.push_back(std::move(s));
        }
        result["sample_definitions"] = std::move(sampleDefs);
    }
    else if (language == "chuck")
    {
        nlohmann::json api = nlohmann::json::array();
        for (const auto& ca : metadata_->chuckApi)
        {
            if (!ca.supported)
                continue;
            nlohmann::json c;
            c["name"] = ca.name;
            c["kind"] = ca.kind;
            c["signature"] = ca.signature;
            c["description"] = ca.description;
            if (ca.example) c["example"] = *ca.example;
            api.push_back(std::move(c));
        }
        result["chuck_api"] = std::move(api);
    }

    return result;
}

// ---------------------------------------------------------------------------
// assembleRuntime — BPM, playback state, slots
// ---------------------------------------------------------------------------

nlohmann::json AuthoringContext::assembleRuntime(const ContextRequest& /*req*/) const
{
    return readFacade_.getAudioStatus();
}

// ---------------------------------------------------------------------------
// assembleSamples — from the real SampleBank
// ---------------------------------------------------------------------------

nlohmann::json AuthoringContext::assembleSamples(const ContextRequest& /*req*/) const
{
    return readFacade_.listSamples();
}

// ---------------------------------------------------------------------------
// assembleInstruments — from AudioEngineFacade
// ---------------------------------------------------------------------------

nlohmann::json AuthoringContext::assembleInstruments(const ContextRequest& /*req*/) const
{
    const std::string projectDir = [this] {
        // Use the readFacade's access to AudioEngineFacade::currentProjectDir()
        // via inspect_project — we need the project dir for instrument listing.
        auto proj = readFacade_.inspectProject();
        return proj.value("project_dir", std::string{});
    }();

    return readFacade_.listChuckInstruments(projectDir);
}

// ---------------------------------------------------------------------------
// assembleLsp — LSP availability + context-derived info
// ---------------------------------------------------------------------------

nlohmann::json AuthoringContext::assembleLsp(
    const ContextRequest& req,
    const EditorContextSnapshot& snap,
    std::string_view language) const
{
    (void)req;
    (void)language;
    nlohmann::json result;

    if (lspCtx_ == nullptr)
    {
        result["available"] = false;
        result["reason"] = "LSP context provider not bound";
        return result;
    }

    auto status = lspCtx_->lspStatus();
    result["available"] = status.value("ok", false);

    if (!status.value("ok", false))
    {
        result["reason"] = status.value("reason", "LSP unavailable");
        return result;
    }

    result["source"] = "strudel_lsp";
    result["language_served"] = std::string(language);

    // Diagnostics
    if (!snap.uri.empty())
    {
        result["diagnostics"] = lspCtx_->diagnosticsForDocument(snap.uri);
    }

    // Completions at cursor (if position specified)
    if (snap.hasContent && !snap.uri.empty())
    {
        result["cursor_completions"] = lspCtx_->completionsAt(
            snap.uri, snap.cursorLine, snap.cursorChar, snap.content);

        // Hover
        result["hover"] = lspCtx_->hoverAt(
            snap.uri, snap.cursorLine, snap.cursorChar);
    }

    return result;
}

// ---------------------------------------------------------------------------
// assembleProject — project overview
// ---------------------------------------------------------------------------

nlohmann::json AuthoringContext::assembleProject(const ContextRequest& req) const
{
    (void)req;
    return readFacade_.inspectProject();
}

} // namespace hathor::control
