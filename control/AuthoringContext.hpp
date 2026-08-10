// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * AuthoringContext.hpp — AI-8 dynamic authoring-context assembler.
 *
 * Assembles a compact, targeted JSON context payload from the shared
 * application models, verified LSP state, and versioned supported-surface
 * metadata.  This is the single place where AI-8 gathers the editor,
 * language, project, and runtime context that an AI authoring request
 * needs.
 *
 * Architecture (AI-8):
 *
 *   MCP client (AI)
 *         ↓
 *   get_context tool
 *         ↓
 *   "get-context" socket command
 *         ↓
 *   ControlInterface::handleGetContext()
 *         ↓
 *   AuthoringContext::assemble()  ← this class (JUCE-free)
 *         ↓
 *   ┌─────┬──────┬──────────┬──────────────┬──────────┐
 *   │ EditorCtx │ LspCtx   │ Metadata     │ ProjectRF  │ Runtime
 *   │ Provider  │ Provider  │ (AI-3)       │ (AI-2)     │ (Facade)
 *   └─────┴──────┴──────────┴──────────────┴──────────┘
 *
 * No second parser, completion engine, or language-definition database
 * is created here.  Language intelligence comes from the LSP; Hathor-specific
 * facts come from AI-3 metadata; runtime state comes from the canonical
 * AI-2 application models.
 *
 * Requirement references: AI-8 §1–§10, AI-2, AI-3, AI-4
 */

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "EditorContextProvider.hpp"
#include "LspContextProvider.hpp"

namespace hathor {
namespace language {
struct LanguageMetadata;
struct MetadataCompatibility;
}
}

namespace hathor::control {

class ProjectReadFacade;

// ---------------------------------------------------------------------------
// Context request
// ---------------------------------------------------------------------------

/**
 * Parsed arguments for a get-context request.
 *
 * All fields are optional — when absent, the assembler falls back to the
 * current editor state and auto-detects relevance from the file type.
 */
struct ContextRequest {
    /// File path or URI to focus on.  If empty, uses the current active editor.
    std::string file;
    /// Cursor line (0-based).  If omitted, uses the editor's current cursor.
    std::optional<int> line;
    /// Cursor character (0-based).  If omitted, uses the editor's current cursor.
    std::optional<int> character;
    /// Language hint: "mininotation" or "chuck".
    /// If empty, inferred from the file extension.
    std::string language;
    /// Selected text (if a non-empty selection is active).
    std::string selectedText;
    /// Selection range, if any.
    struct Range { int startLine, startChar, endLine, endChar; };
    std::optional<Range> selection;

    // --- Scope control — if empty, auto-determine from file type ---
    // Valid values: "editor", "diagnostics", "metadata", "runtime",
    //               "samples", "instruments", "lsp", "project"
    std::vector<std::string> scope;

    /// Include the full file content in the response (default: false).
    bool includeContent = false;
    /// If includeContent is true, truncate content to this many bytes (0 = no limit).
    int maxContentLength = 8192;
};

// ---------------------------------------------------------------------------
// AuthoringContext — the assembler
// ---------------------------------------------------------------------------

/**
 * Assembles a targeted authoring context payload.
 *
 * Constructed once per ControlInterface lifetime.  All provider pointers
 * may be null — when a provider is absent, the corresponding section in
 * the response reports "unavailable" rather than crashing.
 *
 * Thread-safety: assemble() is called from the MCP accept-loop worker
 * thread.  It reads thread-safe snapshots from the providers and the
 * ProjectReadFacade (which uses lock-free / atomic reads).
 */
class AuthoringContext {
public:
    AuthoringContext(ProjectReadFacade&      readFacade,
                     EditorContextProvider*  editorCtx,
                     LspContextProvider*     lspCtx,
                     const language::LanguageMetadata*    metadata,
                     const language::MetadataCompatibility* compat);

    ~AuthoringContext() = default;

    AuthoringContext(const AuthoringContext&) = delete;
    AuthoringContext& operator=(const AuthoringContext&) = delete;

    /**
     * Set the editor context provider (may be null).
     * Called by the UI layer after construction.
     */
    void setEditorContextProvider(EditorContextProvider* provider) noexcept { editorCtx_ = provider; }

    /**
     * Set the LSP context provider (may be null).
     * Called by the UI layer after construction.
     */
    void setLspContextProvider(LspContextProvider* provider) noexcept { lspCtx_ = provider; }

    /**
     * Assemble a context payload for the given request.
     *
     * The response is a JSON object:
     *   {
     *     "ok": true,
     *     "version": "2026-08-10T...",
     *     "metadata_version": {"schema": 1, "engine": "0.1.0", "strudel": "1.2.6", "chuck": "3.8.3", "surface": "B4-K3"},
     *     "sections": { "editor": {...}, "diagnostics": {...}, ... }
     *   }
     *
     * Sections not requested (and not auto-determined) are omitted.
     * Sections whose data source is unavailable include an "unavailable"
     * field explaining why.
     */
    nlohmann::json assemble(const ContextRequest& req) const;

    /**
     * Update the LanguageMetadata pointer (e.g. after a hot-reload).
     * Thread-safe: uses a shared_ptr internally.
     */
    void setMetadata(const language::LanguageMetadata* metadata,
                     const language::MetadataCompatibility* compat);

private:
    // --- Section assemblers ---

    nlohmann::json assembleEditor(const ContextRequest& req,
                                  const EditorContextSnapshot& snap,
                                  std::string_view language) const;

    nlohmann::json assembleDiagnostics(const ContextRequest& req,
                                       const EditorContextSnapshot& snap,
                                       std::string_view language) const;

    nlohmann::json assembleMetadata(const ContextRequest& req,
                                    std::string_view language) const;

    nlohmann::json assembleRuntime(const ContextRequest& req) const;

    nlohmann::json assembleSamples(const ContextRequest& req) const;

    nlohmann::json assembleInstruments(const ContextRequest& req) const;

    nlohmann::json assembleLsp(const ContextRequest& req,
                               const EditorContextSnapshot& snap,
                               std::string_view language) const;

    nlohmann::json assembleProject(const ContextRequest& req) const;

    // --- Helpers ---

    /// Determine which sections to assemble (auto-detect if scope is empty).
    std::vector<std::string> resolveScope(const ContextRequest& req,
                                          std::string_view language) const;

    /// Determine language from file extension or request override.
    std::string inferLanguage(std::string_view file,
                              const ContextRequest& req,
                              const EditorContextSnapshot& snap) const;

    /// Resolve the editor snapshot (from provider or request overrides).
    EditorContextSnapshot resolveSnapshot(const ContextRequest& req) const;

    ProjectReadFacade&                         readFacade_;
    EditorContextProvider*                     editorCtx_;
    LspContextProvider*                        lspCtx_;
    const language::LanguageMetadata*          metadata_;
    const language::MetadataCompatibility*     compat_;
};

} // namespace hathor::control
