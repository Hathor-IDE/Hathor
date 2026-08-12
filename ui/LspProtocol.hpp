// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * LspProtocol.hpp — JUCE-free LSP type definitions.
 *
 * These structs mirror the subset of the Language Server Protocol (LSP) that
 * Hathor's editor integration needs: completion, hover, signature help, and
 * diagnostics. They are used as an intermediate representation between the
 * JSON-RPC layer (LspJsonRpc) and the completion logic (LspCompletionLogic).
 *
 * The types are plain C++ structs with no JUCE or external dependencies,
 * so the completion logic that consumes them can be unit-tested in the
 * JUCE-free hathor-ui-tests target.
 *
 * Requirement references: AI-4
 */

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hathor::lsp {

// ---------------------------------------------------------------------------
// LSP CompletionItemKind (subset)
// ---------------------------------------------------------------------------
enum class CompletionItemKind : int {
    Text       = 1,
    Method     = 2,
    Function   = 3,
    Constructor = 4,
    Field      = 5,
    Variable   = 6,
    Class      = 7,
    Interface  = 8,
    Module     = 9,
    Property   = 10,
    Unit       = 11,
    Value      = 12,
    Enum       = 13,
    Snippet    = 15,
    Keyword    = 14,
};

// ---------------------------------------------------------------------------
// LSP Position
// ---------------------------------------------------------------------------
struct Position {
    int line      = 0;        ///< 0-based line number
    int character = 0;        ///< 0-based UTF-16 code unit offset

    bool operator==(const Position& other) const noexcept {
        return line == other.line && character == other.character;
    }
};

// ---------------------------------------------------------------------------
// LSP Range
// ---------------------------------------------------------------------------
struct Range {
    Position start;
    Position end;

    Range() = default;
    Range(Position s, Position e) : start(s), end(e) {}
};

// ---------------------------------------------------------------------------
// LSP Location
// ---------------------------------------------------------------------------
struct Location {
    std::string uri;
    Range       range;
};

// ---------------------------------------------------------------------------
// LSP MarkupContent (for hover, signatures)
// ---------------------------------------------------------------------------
struct MarkupContent {
    std::string kind;      ///< "plaintext" | "markdown"
    std::string value;
};

// ---------------------------------------------------------------------------
// LSP CompletionItem
// ---------------------------------------------------------------------------
struct CompletionItem {
    std::string                 label;
    std::optional<CompletionItemKind> kind;
    std::optional<std::string>  detail;       ///< short description
    std::optional<MarkupContent> documentation;
    std::optional<std::string>  insertText;
    std::optional<std::string>  sortText;

    CompletionItem() = default;
};

// ---------------------------------------------------------------------------
// LSP CompletionList
// ---------------------------------------------------------------------------
struct CompletionList {
    bool                 isIncomplete = false;
    std::vector<CompletionItem> items;
};

// ---------------------------------------------------------------------------
// LSP SymbolKind (subset for navigation)
// ---------------------------------------------------------------------------
enum class SymbolKind : int {
    File        = 1,
    Module      = 2,
    Namespace   = 3,
    Package     = 4,
    Class       = 5,
    Method      = 6,
    Property    = 7,
    Field       = 8,
    Constructor = 9,
    Enum        = 10,
    Interface   = 11,
    Function    = 12,
    Variable    = 13,
    Constant    = 14,
    String      = 15,
    Number      = 16,
    Boolean     = 17,
    Array       = 18,
    Object      = 19,
    Key         = 20,
    Null        = 21,
    Struct      = 22,
    Event       = 23,
    Operator    = 24,
    TypeParameter = 25,
};

// ---------------------------------------------------------------------------
// LSP SymbolInformation (for documentSymbol / workspaceSymbol)
// ---------------------------------------------------------------------------
struct SymbolInformation {
    std::string                   name;
    SymbolKind                    kind = SymbolKind::Function;
    std::optional<bool>           deprecated;
    std::optional<std::string>    detail;
    Location                      location;
    std::string                   containerName;
    std::optional<int>            flags;
};

// ---------------------------------------------------------------------------
// LSP DocumentSymbol (hierarchical, for documentSymbol with hierarchical capability)
// ---------------------------------------------------------------------------
struct DocumentSymbol {
    std::string                   name;
    std::string                   detail;
    SymbolKind                    kind = SymbolKind::Function;
    Range                         range;
    Range                         selectionRange;
    std::optional<bool>           deprecated;
    std::vector<DocumentSymbol>   children;
};

// ---------------------------------------------------------------------------
// LSP ParameterInformation (for SignatureInformation)
// ---------------------------------------------------------------------------
struct ParameterInformation {
    std::string label;
    std::optional<MarkupContent> documentation;
};

// ---------------------------------------------------------------------------
// LSP SignatureInformation
// ---------------------------------------------------------------------------
struct SignatureInformation {
    std::string                           label;
    std::optional<MarkupContent>          documentation;
    std::vector<ParameterInformation>     parameters;
};

// ---------------------------------------------------------------------------
// LSP SignatureHelp
// ---------------------------------------------------------------------------
struct SignatureHelp {
    std::vector<SignatureInformation> signatures;
    int activeSignature = 0;
    int activeParameter = 0;
};

// ---------------------------------------------------------------------------
// LSP Hover
// ---------------------------------------------------------------------------
struct Hover {
    std::optional<Range>       range;
    std::vector<MarkupContent> contents;
};

// ---------------------------------------------------------------------------
// LSP DiagnosticSeverity
// ---------------------------------------------------------------------------
enum class DiagnosticSeverity : int {
    Error   = 1,
    Warning = 2,
    Info    = 3,
    Hint    = 4,
};

// ---------------------------------------------------------------------------
// LSP Diagnostic
// ---------------------------------------------------------------------------
struct Diagnostic {
    Range                       range;
    std::optional<DiagnosticSeverity> severity;
    std::optional<std::string>  code;
    std::optional<std::string>  source;
    std::string                 message;
};

// ---------------------------------------------------------------------------
// LSP TextDocumentItem (for didOpen / didChange)
// ---------------------------------------------------------------------------
struct TextDocumentItem {
    std::string uri;
    std::string languageId;
    int         version = 0;
    std::string text;
};

// ---------------------------------------------------------------------------
// LSP VersionedTextDocumentIdentifier
// ---------------------------------------------------------------------------
struct VersionedTextDocumentIdentifier {
    std::string uri;
    int         version;
};

// ---------------------------------------------------------------------------
// LSP TextDocumentContentChangeEvent
// ---------------------------------------------------------------------------
struct TextDocumentContentChangeEvent {
    std::optional<Range> range;
    std::optional<int>   rangeLength;
    std::string          text;
};

// ---------------------------------------------------------------------------
// Merged completion item — output of LspCompletionLogic
// ---------------------------------------------------------------------------
/**
 * A completion item merged from LSP result and optional metadata fallback.
 * The `source` field tracks which provider produced this item — useful for
 * debugging and UI presentation (e.g. showing a different icon).
 */
struct CompletionCandidate {
    std::string label;
    CompletionItemKind kind = CompletionItemKind::Text;
    std::string detail;
    std::string documentation;
    std::string insertText;
    std::string source;     ///< "lsp" | "metadata" | "builtin"
};

/**
 * Result of a completion query.
 */
struct CompletionResult {
    std::vector<CompletionCandidate> items;
    bool                             isIncomplete = false;
};

// ---------------------------------------------------------------------------
// Completion context: describes what kind of completion is requested
// ---------------------------------------------------------------------------
enum class CompletionContextKind {
    Code,          ///< general code position (e.g. start of expression)
    StringSample,  ///< inside s("...") string — completing sample names
    StringScale,   ///< inside scale("|...") string — completing scale names
    StringNote,    ///< inside note("...") string — completing note names
    FunctionArgs,  ///< inside function arguments
    Operator,      ///< after an operator context
};

struct CompletionContext {
    CompletionContextKind kind;
    std::string           prefix;       ///< text before cursor that narrows results
    std::string           fullText;     ///< entire document text at the time of request
    Position              position;
    std::string           uri;
};

// ---------------------------------------------------------------------------
// Navigation result types
// ---------------------------------------------------------------------------

/**
 * Result of a textDocument/definition or textDocument/references request.
 * May contain 0, 1, or many locations.
 */
struct NavigationResult {
    std::vector<Location> locations;
};

/**
 * Result of a textDocument/rename request.
 * Contains the locations to rename (for display/preview).
 */
struct RenameResult {
    std::vector<Location> changes;
};

/**
 * Result of a textDocument/documentSymbol request (flat list).
 */
struct DocumentSymbolResult {
    std::vector<SymbolInformation> symbols;
};

/**
 * Result of a workspace/symbol request (flat list).
 */
struct WorkspaceSymbolResult {
    std::vector<SymbolInformation> symbols;
};

/**
 * Kind of navigation requested by the editor.
 */
enum class NavigationKind {
    Definition,
    References,
    TypeDefinition,
    Declaration,
};

} // namespace hathor::lsp

