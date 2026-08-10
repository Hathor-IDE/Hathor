// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * LspContextBridge.hpp — JUCE-dependent implementation of
 * hathor::control::LspContextProvider.
 *
 * Bridges the JUCE UI layer's LSP client (HathorLspClient) and the
 * diagnostics display model (LspDiagnosticsDisplay) to the
 * LspContextProvider interface consumed by the control layer.
 *
 * Diagnostics are stored in a thread-safe internal map, updated from the
 * JUCE message thread when the LSP server publishes new diagnostics.
 *
 * Requirement references: AI-8 §9, AI-4
 */

#include "LspContextProvider.hpp"
#include "LspProtocol.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace hathor::ui {

// Forward declaration — full definition in HathorLspClient.hpp
class HathorLspClient;

/**
 * LspContextBridge
 *
 * Not a JUCE Component — a pure logic class.  Holds a non-owning pointer
 * to the HathorLspClient (for connection status) and maintains a
 * thread-safe snapshot of the most recently published diagnostics per URI.
 *
 * The UI layer calls setDiagnostics() from the JUCE message thread (via the
 * HathorLspClient diagnostics callback).  The control layer calls
 * diagnosticsForDocument() from the MCP accept-loop worker thread.
 */
class LspContextBridge : public hathor::control::LspContextProvider
{
public:
    /**
     * Construct with a non-owning reference to the HathorLspClient.
     * @param lspClient  May be nullptr if the LSP client is not available.
     */
    explicit LspContextBridge(HathorLspClient* lspClient = nullptr);

    ~LspContextBridge() override = default;

    // Non-copyable
    LspContextBridge(const LspContextBridge&)            = delete;
    LspContextBridge& operator=(const LspContextBridge&) = delete;

    /**
     * Update the stored LSP client reference (e.g. after LSP client
     * re-creation).  Called from the JUCE message thread.
     */
    void setLspClient(HathorLspClient* client) noexcept { lspClient_ = client; }

    /**
     * Store diagnostics for a document URI.
     * Called from the JUCE message thread when the LSP server publishes
     * diagnostics (via HathorLspClient::setDiagnosticsCallback).
     *
     * @param uri      The document URI.
     * @param diags    The diagnostics as JSON objects.
     */
     void setDiagnostics(const std::string& uri,
                        const std::vector<nlohmann::json>& diags);

    /**
     * Store LSP diagnostics (from the lsp::Diagnostic struct) for a document.
     * Called from the JUCE message thread via the HathorLspClient diagnostics
     * callback.  Converts diagnostics to JSON internally.
     */
    void setLspDiagnostics(const std::string& uri,
                          const std::vector<lsp::Diagnostic>& diags);

    /**
     * Clear all stored diagnostics for a URI (document closed).
     * Called from the JUCE message thread.
     */
    void clearDiagnostics(const std::string& uri);

    /**
     * Clear all stored diagnostics (e.g. LSP server restarted).
     * Called from the JUCE message thread.
     */
    void clearAll();

    // --- LspContextProvider interface ---

    nlohmann::json lspStatus() const override;

    nlohmann::json diagnosticsForDocument(std::string_view uri) const override;

    nlohmann::json completionsAt(
        std::string_view uri,
        int line,
        int character,
        std::string_view documentText) const override;

    nlohmann::json hoverAt(
        std::string_view uri,
        int line,
        int character) const override;

private:
    HathorLspClient* lspClient_;

    // Diagnostics stored per URI, updated from the message thread.
    // Protected by diagsMtx_.
    mutable std::mutex                                    diagsMtx_;
    std::map<std::string, std::vector<nlohmann::json>>    diags_;
};

} // namespace hathor::ui
