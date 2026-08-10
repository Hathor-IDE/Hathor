// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * LspContextBridge.cpp — implementation.
 *
 * Requirement references: AI-8 §9, AI-4
 */

#include "LspContextBridge.hpp"
#include "HathorLspClient.hpp"

#include <atomic>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LspContextBridge::LspContextBridge(HathorLspClient* lspClient)
    : lspClient_(lspClient)
{
}

// ---------------------------------------------------------------------------
// Diagnostics management (called from JUCE message thread)
// ---------------------------------------------------------------------------

void LspContextBridge::setDiagnostics(
    const std::string& uri,
    const std::vector<nlohmann::json>& diags)
{
    std::lock_guard<std::mutex> lock(diagsMtx_);
    diags_[uri] = diags;
}

void LspContextBridge::clearDiagnostics(const std::string& uri)
{
    std::lock_guard<std::mutex> lock(diagsMtx_);
    diags_.erase(uri);
}

void LspContextBridge::clearAll()
{
    std::lock_guard<std::mutex> lock(diagsMtx_);
    diags_.clear();
}

// ---------------------------------------------------------------------------
// LspContextProvider interface
// ---------------------------------------------------------------------------

nlohmann::json LspContextBridge::lspStatus() const
{
    if (lspClient_ == nullptr)
    {
        return nlohmann::json{
            {"ok", false},
            {"reason", "LSP client not available"}
        };
    }

    if (!lspClient_->isRunning())
    {
        return nlohmann::json{
            {"ok", false},
            {"reason", "LSP server not running"}
        };
    }

    return nlohmann::json{
        {"ok", true},
        {"source", "strudel_lsp"},
        {"server_script", lspClient_->serverScriptPath()},
        {"language_id", "mininotation"}
    };
}

nlohmann::json LspContextBridge::diagnosticsForDocument(std::string_view uri) const
{
    nlohmann::json result;
    result["ok"] = true;
    result["source"] = "strudel_lsp";
    result["uri"] = std::string(uri);

    std::vector<nlohmann::json> diags;
    {
        std::lock_guard<std::mutex> lock(diagsMtx_);
        auto it = diags_.find(std::string(uri));
        if (it != diags_.end())
            diags = it->second;
    }

    result["diagnostics"] = nlohmann::json::array();
    for (const auto& d : diags)
        result["diagnostics"].push_back(d);

    result["count"] = diags.size();
    return result;
}

nlohmann::json LspContextBridge::completionsAt(
    std::string_view uri,
    int line,
    int character,
    std::string_view documentText) const
{
    // Completions require an async LSP request — not synchronously available
    // through the context provider.  The AI-G3 (FIM/ghost-writing) layer
    // handles edit-location-specific completions via the LSP→MCP bridge.
    //
    // We return what we have: the URI, position, and a note that live
    // completions require a direct LSP round-trip.
    (void)uri; (void)line; (void)character; (void)documentText;

    return nlohmann::json{
        {"ok", false},
        {"reason", "Live completions require an async LSP request — "
                     "use the LSP→MCP bridge directly for completion context"}
    };
}

nlohmann::json LspContextBridge::hoverAt(
    std::string_view uri,
    int line,
    int character) const
{
    // Hover also requires an async LSP request.
    (void)uri; (void)line; (void)character;

    return nlohmann::json{
        {"ok", false},
        {"reason", "Live hover requires an async LSP request — "
                     "use the LSP→MCP bridge directly for hover context"}
    };
}

} // namespace hathor::ui
