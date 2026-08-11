// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * LspCompletionLogic.cpp — JUCE-free implementation of completion logic.
 *
 * Architecture:
 *   LSP client → LspJsonRpc (parse response) → LspCompletionLogic (merge)
 *      → CompletionResult (consumed by JUCE UI layer)
 *
 * Requirement references: AI-4, AI-3 decision #18
 */

#include "LspCompletionLogic.hpp"
#include "hathor/LanguageMetadata.hpp"
#include "ChuckKeywords.hpp"

#include <cstdint>

namespace hathor {
namespace language {

// Forward declarations of the LanguageMetadata struct fields we need.
// (Full definition is in engine/include/hathor/LanguageMetadata.hpp)

} // namespace language

namespace lsp {

// ---------------------------------------------------------------------------
// Context analysis
// ---------------------------------------------------------------------------

static std::string_view getLine(std::string_view text, int line)
{
    int currentLine = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (currentLine == line)
            break;
        if (text[i] == '\n')
        {
            ++currentLine;
            start = i + 1;
        }
    }
    if (currentLine != line)
        return "";

    std::size_t end = text.find('\n', start);
    if (end == std::string::npos)
        end = text.size();
    return text.substr(start, end - start);
}

static std::string_view::const_iterator findWordEnd(std::string_view line, std::size_t pos)
{
    auto it = line.begin() + std::min(pos, line.size());
    static const char* delimiters = " \t\n\r[]<>(){},.*/+!~|:;'\"";
    while (it != line.end() && !strchr(delimiters, *it))
        ++it;
    return it;
}

static std::string_view::const_iterator findWordStart(std::string_view line, std::size_t pos)
{
    auto it = line.begin() + std::min(pos, line.size());
    static const char* delimiters = " \t\n\r[]<>(){},.*/+!~|:;'\"";
    while (it != line.begin() && !strchr(delimiters, *(it - 1)))
        --it;
    return it;
}

// Forward declare helper used in context analysis
static std::string extractLastIdentifier(const std::string& text);

ContextAnalysis analyzeContext(std::string_view documentText, int line, int character)
{
    ContextAnalysis result;
    result.kind = CompletionContextKind::Code;

    std::string_view currentLine = getLine(documentText, line);

    if (character < 0 || static_cast<std::size_t>(character) > currentLine.size())
    {
        result.prefix = "";
        return result;
    }

    // Determine prefix (word being completed) by walking backwards
    std::size_t pos = static_cast<std::size_t>(character);
    auto startIt = findWordStart(currentLine, pos);
    auto endIt = findWordEnd(currentLine, pos);

    std::string_view prefix(currentLine.data() + (startIt - currentLine.begin()),
                            endIt - startIt);
    result.prefix = std::string(prefix);
    result.fullPrefix = result.prefix;

    // Convert to lowercase for case-insensitive matching
    std::string lowerPrefix;
    lowerPrefix.reserve(prefix.size());
    for (char c : prefix)
        lowerPrefix += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    result.prefix = lowerPrefix;

    // Check if inside a string literal
    bool inString = false;
    char stringDelimiter = '\0';
    for (std::size_t i = 0; i < pos; ++i)
    {
        char c = currentLine[i];
        if ((c == '"' || c == '\'') && c == stringDelimiter)
        {
            inString = false;
            stringDelimiter = '\0';
        }
        else if (c == '"' || c == '\'')
        {
            inString = true;
            stringDelimiter = c;
        }
    }

    // Check if inside parentheses
    int parenDepth = 0;
    for (std::size_t i = 0; i < pos; ++i)
    {
        if (currentLine[i] == '(') ++parenDepth;
        if (currentLine[i] == ')') --parenDepth;
    }
    result.insideParens = parenDepth > 0;

    // Determine context kind
    if (inString)
    {
        // Check if preceded by a function that expects a string argument
        std::string beforeString = std::string(currentLine.substr(0, static_cast<std::size_t>(startIt - currentLine.begin())));
        std::string lastWord = extractLastIdentifier(beforeString);

        if (lastWord == "s" || lastWord == "sound")
        {
            result.kind = CompletionContextKind::StringSample;
            result.insideString = true;
        }
        else if (lastWord == "scale")
        {
            result.kind = CompletionContextKind::StringScale;
            result.insideString = true;
        }
        else if (lastWord == "note" || lastWord == "n")
        {
            result.kind = CompletionContextKind::StringNote;
            result.insideString = true;
        }
        else
        {
            result.kind = CompletionContextKind::Code;
            result.insideString = true;
        }
    }
    else if (result.insideParens)
    {
        result.kind = CompletionContextKind::FunctionArgs;
        // Find the enclosing function name
        std::string before = std::string(currentLine.substr(0, pos));
        std::size_t parenPos = before.rfind('(');
        if (parenPos != std::string::npos)
        {
            std::string beforeParen = before.substr(0, parenPos);
            std::size_t funcEnd = beforeParen.find_last_not_of(" \t");
            if (funcEnd != std::string::npos)
            {
                std::size_t funcStart = beforeParen.find_last_of(" \t()=,", funcEnd);
                std::size_t idStart = (funcStart == std::string::npos) ? 0 : funcStart + 1;
                result.functionName = beforeParen.substr(idStart, funcEnd - idStart + 1);
            }
        }
    }
    else
    {
        result.kind = CompletionContextKind::Code;

        // Check for operator context
        if (prefix.empty())
        {
            // Could be at start of atom, function, or operator position
        }

        // Check if previous char is an operator
        if (pos > 0)
        {
            char prevChar = currentLine[pos - 1];
            if (prevChar == '*' || prevChar == '/' || prevChar == '!' ||
                prevChar == '~' || prevChar == '|' || prevChar == ',' ||
                prevChar == '<' || prevChar == '>' || prevChar == ']')
            {
                result.kind = CompletionContextKind::Operator;
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

static std::string extractLastIdentifier(const std::string& text)
{
    if (text.empty())
        return "";

    // Walk backwards past non-identifier characters (skip delimiters, quotes, etc.)
    std::size_t i = text.size();
    while (i > 0)
    {
        char c = text[i - 1];
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            break;
        --i;
    }

    // Collect identifier characters backwards
    std::size_t start = i;
    while (start > 0)
    {
        char c = text[start - 1];
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            break;
        --start;
    }

    if (start == i)
        return "";

    return text.substr(start, i - start);
}

// ---------------------------------------------------------------------------
// Prefix matching
// ---------------------------------------------------------------------------

bool matchesPrefix(std::string_view label, std::string_view prefix) noexcept
{
    if (prefix.empty())
        return true;

    if (label.size() < prefix.size())
        return false;

    for (std::size_t i = 0; i < prefix.size(); ++i)
    {
        char l = static_cast<char>(std::tolower(static_cast<unsigned char>(label[i])));
        char p = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
        if (l != p)
            return false;
    }
    return true;
}

std::vector<CompletionItem> filterByPrefix(const std::vector<CompletionItem>& items,
                                           std::string_view prefix)
{
    std::vector<CompletionItem> result;
    for (const auto& item : items)
    {
        if (matchesPrefix(item.label, prefix))
            result.push_back(item);
    }
    return result;
}

static int kindPriority(CompletionItemKind kind)
{
    switch (kind)
    {
        case CompletionItemKind::Function:      return 0;
        case CompletionItemKind::Class:         return 1;
        case CompletionItemKind::Enum:          return 2;
        case CompletionItemKind::Value:         return 3;  // samples
        case CompletionItemKind::Keyword:       return 4;
        case CompletionItemKind::Field:         return 5;
        case CompletionItemKind::Variable:      return 6;
        case CompletionItemKind::Property:      return 7;
        case CompletionItemKind::Method:        return 8;
        case CompletionItemKind::Constructor:   return 9;
        case CompletionItemKind::Unit:          return 10;
        case CompletionItemKind::Module:        return 11;
        case CompletionItemKind::Interface:     return 12;
        case CompletionItemKind::Snippet:       return 13;
        case CompletionItemKind::Text:          return 14;
    }
    return 15;
}

void sortCompletionItems(std::vector<CompletionItem>& items) noexcept
{
    std::sort(items.begin(), items.end(), [](const CompletionItem& a, const CompletionItem& b) {
        int pa = a.kind ? kindPriority(*a.kind) : 20;
        int pb = b.kind ? kindPriority(*b.kind) : 20;
        if (pa != pb)
            return pa < pb;
        // Within same kind, sort alphabetically (case-insensitive)
        std::string la, lb;
        for (char c : a.label) la += std::tolower(static_cast<unsigned char>(c));
        for (char c : b.label) lb += std::tolower(static_cast<unsigned char>(c));
        return la < lb;
    });
}

// ---------------------------------------------------------------------------
// Metadata fallback (L1)
// ---------------------------------------------------------------------------

std::vector<CompletionCandidate> metadataFallback(
    const language::LanguageMetadata& metadata,
    const language::MetadataCompatibility& compatibility,
    const ContextAnalysis& context)
{
    // AI-3 decision #18: verify version compatibility before using metadata
    if (!compatibility.compatible)
        return {};

    std::vector<CompletionCandidate> candidates;

    if (context.kind == CompletionContextKind::StringSample)
    {
        // L1: Add supported sample names from metadata
        for (const auto& sample : metadata.samples)
        {
            if (matchesPrefix(sample.name, context.prefix))
            {
                CompletionCandidate c;
                c.label = sample.name;
                c.kind = CompletionItemKind::Value;
                c.detail = "sample";
                c.documentation = sample.description;
                c.insertText = sample.name;
                c.source = "metadata";
                candidates.push_back(std::move(c));
            }
        }
    }
    else if (context.kind == CompletionContextKind::StringScale)
    {
        // Scale completions — not in metadata, use built-in list
        // (metadata has no scale list; scales are part of Strudel core)
        static const std::vector<const char*> scales = {
            "major", "minor", "dorian", "phrygian", "lydian",
            "mixolydian", "aeolian", "locrian", "chromatic",
        };
        for (const auto* s : scales)
        {
            if (matchesPrefix(s, context.prefix))
            {
                CompletionCandidate c;
                c.label = s;
                c.kind = CompletionItemKind::Enum;
                c.detail = "scale";
                c.documentation = std::string(s) + " scale";
                c.insertText = s;
                c.source = "builtin";
                candidates.push_back(std::move(c));
            }
        }
    }
    else
    {
        // L1: Function completion at code position
        for (const auto& fn : metadata.functions)
        {
            if (!fn.supported)
                continue; // Skip unsupported functions
            if (matchesPrefix(fn.name, context.prefix))
            {
                CompletionCandidate c;
                c.label = fn.name;
                c.kind = CompletionItemKind::Function;
                c.detail = fn.signature;
                c.documentation = fn.description;
                c.insertText = fn.name;
                c.source = "metadata";
                candidates.push_back(std::move(c));
            }
        }
    }

    return candidates;
}

// ---------------------------------------------------------------------------
// ChucK metadata fallback (AI-G7) — deterministic completion from versioned
// supported-surface metadata (chuckApi) + built-in ChucK keyword sets.
// No reusable ChucK LSP server exists — this is the sole deterministic
// completion source for .ck files.
// ---------------------------------------------------------------------------

namespace {

/// Map a ChuckAPIDefinition::kind string to an LSP CompletionItemKind.
CompletionItemKind chuckKindToLspKind(std::string_view kind) noexcept
{
    if (kind == "ugen")     return CompletionItemKind::Class;
    if (kind == "constant") return CompletionItemKind::Value;
    if (kind == "library")  return CompletionItemKind::Module;
    if (kind == "class")    return CompletionItemKind::Class;
    return CompletionItemKind::Text;
}

/// Match a ChucK identifier candidate against the prefix (case-sensitive,
/// since ChucK is case-sensitive like C++).
bool matchesChuckPrefix(std::string_view label, std::string_view prefix) noexcept
{
    if (prefix.empty()) return true;
    if (label.size() < prefix.size()) return false;
    return label.substr(0, prefix.size()) == prefix;
}

} // anonymous namespace

std::vector<CompletionCandidate> chuckMetadataFallback(
    const language::LanguageMetadata& metadata,
    const language::MetadataCompatibility& compatibility,
    const ContextAnalysis& context)
{
    // AI-3 decision #18: verify version compatibility before using metadata.
    if (!compatibility.compatible)
        return {};

    std::vector<CompletionCandidate> candidates;

    // --- L1a: ChucK API definitions from versioned metadata ---
    // These are the canonical supported-surface ChucK APIs (UGens, constants,
    // library classes). Only `supported` entries are offered.
    for (const auto& ca : metadata.chuckApi)
    {
        if (!ca.supported) continue;
        if (!matchesChuckPrefix(ca.name, context.fullPrefix)) continue;

        CompletionCandidate c;
        c.label = ca.name;
        c.kind = chuckKindToLspKind(ca.kind);
        c.detail = ca.signature;
        c.documentation = ca.description;
        if (ca.example)
            c.documentation += "\n\nExample:\n" + truncateStr(*ca.example, 120);
        c.insertText = ca.name;
        c.source = "metadata";
        candidates.push_back(std::move(c));
    }

    // --- L1b: ChucK built-in keywords and types ---
    // From the vscode-chuck grammar (ChuckKeywords). These are always available
    // regardless of metadata, but we gate on compatibility for consistency.
    const auto& keywords = chuckKeywordSet();
    for (const auto& kw : keywords)
    {
        if (!matchesChuckPrefix(kw, context.fullPrefix)) continue;
        CompletionCandidate c;
        c.label = std::string(kw);
        c.kind = CompletionItemKind::Keyword;
        c.detail = "ChucK keyword";
        c.insertText = std::string(kw);
        c.source = "builtin";
        candidates.push_back(std::move(c));
    }

    const auto& modifiers = chuckModifierSet();
    for (const auto& m : modifiers)
    {
        if (!matchesChuckPrefix(m, context.fullPrefix)) continue;
        CompletionCandidate c;
        c.label = std::string(m);
        c.kind = CompletionItemKind::Keyword;
        c.detail = "ChucK modifier";
        c.insertText = std::string(m);
        c.source = "builtin";
        candidates.push_back(std::move(c));
    }

    const auto& types = chuckTypeSet();
    for (const auto& t : types)
    {
        if (!matchesChuckPrefix(t, context.fullPrefix)) continue;
        CompletionCandidate c;
        c.label = std::string(t);
        c.kind = CompletionItemKind::Class;
        c.detail = "ChucK type";
        c.insertText = std::string(t);
        c.source = "builtin";
        candidates.push_back(std::move(c));
    }

    const auto& typeKeywords = chuckTypeKeywordSet();
    for (const auto& tk : typeKeywords)
    {
        if (!matchesChuckPrefix(tk, context.fullPrefix)) continue;
        CompletionCandidate c;
        c.label = std::string(tk);
        c.kind = CompletionItemKind::Keyword;
        c.detail = "ChucK type keyword";
        c.insertText = std::string(tk);
        c.source = "builtin";
        candidates.push_back(std::move(c));
    }

    const auto& varLang = chuckVariableLanguageSet();
    for (const auto& vl : varLang)
    {
        if (!matchesChuckPrefix(vl, context.fullPrefix)) continue;
        CompletionCandidate c;
        c.label = std::string(vl);
        c.kind = CompletionItemKind::Variable;
        c.detail = "ChucK variable";
        c.insertText = std::string(vl);
        c.source = "builtin";
        candidates.push_back(std::move(c));
    }

    const auto& constants = chuckConstantSet();
    for (const auto& constant : constants)
    {
        if (!matchesChuckPrefix(constant, context.fullPrefix)) continue;
        CompletionCandidate c;
        c.label = std::string(constant);
        c.kind = CompletionItemKind::Value;
        c.detail = "ChucK constant";
        c.insertText = std::string(constant);
        c.source = "builtin";
        candidates.push_back(std::move(c));
    }

    const auto& ugens = chuckUgenSet();
    for (const auto& ugen : ugens)
    {
        if (!matchesChuckPrefix(ugen, context.fullPrefix)) continue;
        // Don't duplicate if already in metadata
        bool alreadyListed = false;
        for (const auto& existing : candidates)
        {
            if (existing.label == ugen && existing.source == "metadata")
            {
                alreadyListed = true;
                break;
            }
        }
        if (alreadyListed) continue;
        CompletionCandidate c;
        c.label = std::string(ugen);
        c.kind = CompletionItemKind::Class;
        c.detail = "UGen";
        c.insertText = std::string(ugen);
        c.source = "builtin";
        candidates.push_back(std::move(c));
    }

    const auto& libs = chuckLibrarySet();
    for (const auto& lib : libs)
    {
        if (!matchesChuckPrefix(lib, context.fullPrefix)) continue;
        CompletionCandidate c;
        c.label = std::string(lib);
        c.kind = CompletionItemKind::Module;
        c.detail = "ChucK library";
        c.insertText = std::string(lib);
        c.source = "builtin";
        candidates.push_back(std::move(c));
    }

    return candidates;
}
{
    CompletionCandidate c;
    c.label = fn.name;
    c.kind = CompletionItemKind::Function;
    c.detail = fn.signature;
    c.documentation = fn.description;
    c.insertText = fn.name;
    c.source = "metadata";
    return c;
}

std::vector<CompletionCandidate> makeSampleCandidates(
    const language::LanguageMetadata& /*metadata*/,
    const language::SampleDefinition* sampleDef,
    std::string_view prefix)
{
    std::vector<CompletionCandidate> candidates;
    if (!sampleDef)
        return candidates;

    if (matchesPrefix(sampleDef->name, prefix))
    {
        CompletionCandidate c;
        c.label = sampleDef->name;
        c.kind = CompletionItemKind::Value;
        c.detail = "sample";
        c.documentation = sampleDef->description;
        c.insertText = sampleDef->name;
        c.source = "metadata";
        candidates.push_back(std::move(c));
    }
    return candidates;
}

// ---------------------------------------------------------------------------
// L1: Merged completion
// ---------------------------------------------------------------------------

CompletionResult mergeCompletion(
    const std::vector<CompletionItem>& lspItems,
    const language::LanguageMetadata* metadata,
    const language::MetadataCompatibility* compatibility,
    const ContextAnalysis& context)
{
    CompletionResult result;
    result.isIncomplete = false;

    // Track which labels we already have from LSP (for dedup)
    std::unordered_set<std::string> seenLabels;

    // 1. Start with LSP-provided items (filtered by prefix)
    auto filtered = filterByPrefix(lspItems, context.prefix);
    sortCompletionItems(filtered);

    for (const auto& item : filtered)
    {
        CompletionCandidate c;
        c.label = item.label;
        c.kind = item.kind.value_or(CompletionItemKind::Text);
        c.detail = item.detail.value_or("");
        c.documentation = item.documentation
            ? item.documentation->value
            : "";
        c.insertText = item.insertText.value_or(item.label);
        c.source = "lsp";
        result.items.push_back(std::move(c));
        seenLabels.insert(item.label);
    }

    // 2. Metadata fallback (only if compatible)
    if (metadata && compatibility && compatibility->compatible)
    {
        auto fallback = metadataFallback(*metadata, *compatibility, context);
        for (const auto& fc : fallback)
        {
            if (seenLabels.find(fc.label) == seenLabels.end())
            {
                result.items.push_back(fc);
                seenLabels.insert(fc.label);
            }
        }
    }

    // 3. If LSP is down or empty, and we have metadata, ensure we have
    //    at least some completions for the context
    if (result.items.empty() && metadata && compatibility && compatibility->compatible)
    {
        // Full metadata fallback (no LSP results at all)
        auto all = metadataFallback(*metadata, *compatibility, context);
        for (const auto& fc : all)
        {
            if (seenLabels.find(fc.label) == seenLabels.end())
            {
                result.items.push_back(fc);
                seenLabels.insert(fc.label);
            }
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// L2: Signature-aware enrichment
// ---------------------------------------------------------------------------

void enrichWithSignatureInfo(CompletionResult& result,
                             const language::LanguageMetadata* metadata,
                             const ContextAnalysis& context) noexcept
{
    // If the cursor is inside a function call and we have metadata,
    // enrich items that match the function name with signature info.
    if (!context.insideParens || context.functionName.empty() || !metadata)
        return;

    const auto* fn = findFunction(*metadata, context.functionName);
    if (!fn || !fn->supported)
        return;

    // Check if any completion items match the function name
    for (auto& item : result.items)
    {
        if (item.label == context.functionName)
        {
            if (item.detail.empty())
                item.detail = fn->signature;
            if (item.documentation.empty())
                item.documentation = fn->description;
        }
    }
}

// ---------------------------------------------------------------------------
// L3: Context-aware filtering
// ---------------------------------------------------------------------------

void filterByProjectContext(CompletionResult& result,
                            const std::unordered_set<std::string>& projectSamples,
                            const std::optional<std::string>& /*frontMatterSlot*/) noexcept
{
    if (projectSamples.empty())
        return; // No project context — don't filter

    // For sample completions, filter by actual project samples
    auto it = result.items.begin();
    while (it != result.items.end())
    {
        if (it->kind == CompletionItemKind::Value && it->detail == "sample")
        {
            // Check if this sample is actually in the project
            std::string lowerLabel;
            for (char c : it->label)
                lowerLabel += std::tolower(static_cast<unsigned char>(c));
            if (projectSamples.find(lowerLabel) == projectSamples.end())
            {
                it = result.items.erase(it);
            }
            else
            {
                ++it;
            }
        }
        else
        {
            ++it;
        }
    }
}

// ---------------------------------------------------------------------------
// Hover merging
// ---------------------------------------------------------------------------

std::optional<Hover> mergeHover(
    const std::optional<Hover>& lspHover,
    const language::LanguageMetadata* metadata,
    const language::MetadataCompatibility* compatibility,
    std::string_view word)
{
    // If LSP provided hover, use it
    if (lspHover.has_value() && !lspHover->contents.empty())
        return lspHover;

    // Fall back to metadata
    if (metadata && compatibility && compatibility->compatible)
    {
        const auto* fn = findFunction(*metadata, word);
        if (fn)
        {
            Hover h;
            h.contents.push_back({.kind = "markdown", .value = fn->description});
            if (fn->example)
                h.contents.push_back({.kind = "markdown", .value = "```\n" + *fn->example + "\n```"});
            return h;
        }

        const auto* sample = findSample(*metadata, word);
        if (sample)
        {
            Hover h;
            h.contents.push_back({.kind = "markdown", .value = "**" + sample->name + "** — " + sample->description});
            return h;
        }
    }

    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Diagnostics merging
// ---------------------------------------------------------------------------

static std::string extractWords(std::string_view text)
{
    std::string result;
    for (char c : text)
    {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')
            result += c;
        else
            result += ' ';
    }
    return result;
}

std::vector<Diagnostic> mergeDiagnostics(
    const std::vector<Diagnostic>& lspDiagnostics,
    const language::LanguageMetadata* metadata,
    const language::MetadataCompatibility* compatibility,
    std::string_view documentText)
{
    std::vector<Diagnostic> result = lspDiagnostics;

    // Only do metadata-based checks if metadata is compatible
    if (!metadata || !compatibility || !compatibility->compatible)
        return result;

    // Tokenize the document by lines
    std::string docText(documentText);
    std::size_t pos = 0;
    int lineNum = 0;

    while (pos <= docText.size())
    {
        std::size_t nextPos = docText.find('\n', pos);
        std::string line = (nextPos == std::string::npos)
                               ? docText.substr(pos)
                               : docText.substr(pos, nextPos - pos);

        // Remove trailing \r if present
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // Advance pos for next iteration
        pos = (nextPos == std::string::npos) ? docText.size() + 1 : nextPos + 1;

        // Skip front-matter
        if (line == "[hathor]" && lineNum == 0)
        {
            ++lineNum;
            while (pos <= docText.size())
            {
                std::size_t nextPos2 = docText.find('\n', pos);
                std::string fmLine = (nextPos2 == std::string::npos)
                                         ? docText.substr(pos)
                                         : docText.substr(pos, nextPos2 - pos);
                if (!fmLine.empty() && fmLine.back() == '\r')
                    fmLine.pop_back();
                pos = (nextPos2 == std::string::npos) ? docText.size() + 1 : nextPos2 + 1;
                ++lineNum;
                if (fmLine.empty())
                    break;
            }
            continue;
        }

        // Extract words from the line for metadata checks
        std::string words = extractWords(line);
        // Manual word splitting (avoids <sstream> on older SDKs)
        auto wordBegin = std::string::npos;
        for (std::size_t i = 0; i <= words.size(); ++i)
        {
            bool isSpace = (i == words.size()) || std::isspace(static_cast<unsigned char>(words[i]));
            if (isSpace)
            {
                if (wordBegin != std::string::npos)
                {
                    std::string word = words.substr(wordBegin, i - wordBegin);
                    // Check for unsupported functions (warnings)
                    // Only check words that look like identifiers (not numbers)
                    if (!word.empty() && !std::isdigit(static_cast<unsigned char>(word[0])))
                    {
                        // Check if it's a function name followed by '('
                        // (This is a heuristic — the LSP already provides parse errors)

                        // Check for unsupported mini-notation functions
                        // We check if the word matches a function name that is marked
                        // as unsupported in metadata
                        const auto* fn = findFunction(*metadata, word);
                        if (fn && !fn->supported)
                        {
                            // Find position of word in line
                            std::size_t pos = line.find(word);
                            if (pos != std::string::npos)
                            {
                                Diagnostic d;
                                d.range = {
                                    {lineNum, static_cast<int>(pos)},
                                    {lineNum, static_cast<int>(pos + word.size())}
                                };
                                d.severity = DiagnosticSeverity::Warning;
                                d.source = "hathor-metadata";
                                d.code = "UNSUPPORTED_FUNCTION";
                                d.message = "Function '" + word + "' is listed in metadata but not yet supported by Hathor's engine: " + fn->description;
                                result.push_back(d);
                            }
                        }
                    }
                    wordBegin = std::string::npos;
                }
            }
            else
            {
                if (wordBegin == std::string::npos)
                    wordBegin = i;
            }
        }
        ++lineNum;
    }

    return result;
}

// ---------------------------------------------------------------------------
// ChucK diagnostics (AI-G7) — real compiler + metadata-aware checks
// ---------------------------------------------------------------------------

std::vector<Diagnostic> chuckDiagnostics(
    const ChuckCompileDiagnostic& compileDiag,
    const language::LanguageMetadata* metadata,
    const language::MetadataCompatibility* compatibility,
    std::string_view documentText)
{
    std::vector<Diagnostic> result;

    // --- 1. Real compiler diagnostic (authoritative) ---
    if (!compileDiag.ok)
    {
        Diagnostic d;
        d.severity = DiagnosticSeverity::Error;
        d.code = "CK_COMPILE_ERROR";
        d.source = "chuck_compiler";
        d.message = compileDiag.message;
        // libchuck uses 1-based line/column; convert to 0-based LSP convention.
        d.range.start.line = compileDiag.errorLine > 0 ? compileDiag.errorLine - 1 : 0;
        d.range.start.character = compileDiag.errorColumn > 0 ? compileDiag.errorColumn - 1 : 0;
        d.range.end = d.range.start;
        d.range.end.character = d.range.start.character + 1;
        result.push_back(std::move(d));
        // If the compiler found an error, still run metadata checks for
        // additional context (e.g. unsupported APIs that co-occur).
    }

    // --- 2. Metadata-aware checks (warnings for unsupported ChucK APIs) ---
    if (!metadata || !compatibility || !compatibility->compatible)
        return result;

    // Tokenize the document by lines and scan for identifiers that
    // correspond to unsupported ChuckAPI entries.
    std::string docText(documentText);
    std::size_t pos = 0;
    int lineNum = 0;

    while (pos <= docText.size())
    {
        std::size_t nextPos = docText.find('\n', pos);
        std::string lineText = (nextPos == std::string::npos)
                                   ? docText.substr(pos)
                                   : docText.substr(pos, nextPos - pos);

        if (!lineText.empty() && lineText.back() == '\r')
            lineText.pop_back();

        pos = (nextPos == std::string::npos) ? docText.size() + 1 : nextPos + 1;

        // Extract identifiers from this line.
        // An identifier starts with [a-zA-Z_] and continues with [a-zA-Z0-9_].
        std::size_t i = 0;
        while (i < lineText.size())
        {
            if (std::isalpha(static_cast<unsigned char>(lineText[i])) || lineText[i] == '_')
            {
                std::size_t start = i;
                while (i < lineText.size() &&
                       (std::isalnum(static_cast<unsigned char>(lineText[i])) || lineText[i] == '_'))
                    ++i;
                std::string word = lineText.substr(start, i - start);

                // Check if this word is a ChucK API that is in metadata but unsupported.
                const auto* ca = [&metadata, &word]() -> const language::ChuckAPIDefinition* {
                    for (const auto& api : metadata->chuckApi)
                        if (api.name == word) return &api;
                    return nullptr;
                }();

                if (ca && !ca->supported)
                {
                    Diagnostic d;
                    d.severity = DiagnosticSeverity::Warning;
                    d.code = "UNSUPPORTED_CHUCK_API";
                    d.source = "hathor-metadata";
                    d.message = "ChucK API '" + word + "' is in the metadata but not yet supported by Hathor: " + ca->description;
                    d.range.start = {lineNum, static_cast<int>(start)};
                    d.range.end   = {lineNum, static_cast<int>(i)};
                    result.push_back(std::move(d));
                }
            }
            else
            {
                ++i;
            }
        }
        ++lineNum;
    }

    return result;
}

} // namespace lsp
} // namespace hathor
