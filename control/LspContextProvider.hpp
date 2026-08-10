// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * LspContextProvider.hpp — JUCE-free abstract interface that exposes
 * LSP-derived language intelligence (diagnostics, completions, hover,
 * signature help) to the AI-8 authoring context layer.
 *
 * Architecture boundary (AI-8):
 *
 *   Strudel LSP server
 *         ↓
 *   HathorLspClient (ui/)         ───┐
 *         ↓                            │ implements LspContextProvider
 *   LspCompletionLogic (ui/)           │ returns nlohmann::json
 *         ↓                            │
 *   ┌────┴────┐                        │
 *   │ LspContextProvider  ← this interface (JUCE-free, in control/) │
 *   └─────────┘                        │
 *         ↓                            │
 *   AuthoringContext (control/) ───────┘
 *         ↓
 *   get-context MCP tool → AI
 *
 * The provider returns JSON directly (not lsp:: types) so that the
 * control/ layer does not need to depend on ui/LspProtocol.hpp.
 * The UI-layer implementation translates internal lsp:: types to JSON.
 *
 * Requirement references: AI-8 §9, AI-4
 */

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace hathor::control {

/**
 * Interface for querying LSP-derived language context.
 *
 * Implementations must be thread-safe — the methods may be called from
 * the MCP accept-loop worker thread.  Implementations should cache or
 * snapshot LSP state rather than making synchronous LSP requests on the
 * acceptor thread (which could block).
 *
 * If the LSP is unavailable or incompatible, methods should return
 * the JSON equivalent of an empty result with an explicit status field
 * rather than throwing.
 */
class LspContextProvider {
public:
    virtual ~LspContextProvider() = default;

    /**
     * Check whether the LSP server is currently connected and healthy.
     * Returns a JSON object: {"ok": bool, "reason": string (if not ok)}.
     */
    virtual nlohmann::json lspStatus() const = 0;

    /**
     * Return diagnostics for the document identified by @p uri.
     *
     * The result is a JSON object:
     *   {"ok": true,
     *    "source": "strudel_lsp",
     *    "diagnostics": [
     *      {"severity":"error","code":"...","message":"...","line":N,"column":N},
     *      ...
     *    ]}
     *
     * If the LSP has no diagnostics for this URI, "diagnostics" is an empty
     * array.  If the LSP is unavailable, returns {"ok": false, "reason": "..."}.
     */
    virtual nlohmann::json diagnosticsForDocument(std::string_view uri) const = 0;

    /**
     * Return LSP completions at the given cursor position.
     *
     * The result is a JSON object:
     *   {"ok": true,
     *    "items": [
     *      {"label":"...","kind":"function","detail":"...","insert_text":"..."},
     *      ...
     *    ]}
     *
     * If the LSP is unavailable, returns {"ok": false, "reason": "..."}.
     */
    virtual nlohmann::json completionsAt(
        std::string_view uri,
        int line,
        int character,
        std::string_view documentText) const = 0;

    /**
     * Return LSP hover information at the given cursor position.
     *
     * The result is a JSON object:
     *   {"ok": true,
     *    "contents": [{"kind":"markdown","value":"..."}]}
     *
     * If the LSP is unavailable, returns {"ok": false, "reason": "..."}.
     */
    virtual nlohmann::json hoverAt(
        std::string_view uri,
        int line,
        int character) const = 0;
};

} // namespace hathor::control
