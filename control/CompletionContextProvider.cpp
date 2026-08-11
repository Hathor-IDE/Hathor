// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * CompletionContextProvider.cpp — AI-G3 Hathor-specific authoring-context provider.
 *
 * Assembles a compact, location-aware, bounded context for a single edit
 * location, reusing the shared AI-8 / AI-2 / AI-3 / AI-4 infrastructure:
 *   - EditorContextProvider  (editor snapshot: file, cursor, selection, slot)
 *   - LspContextProvider     (LSP diagnostics, completions, hover — AI-4)
 *   - ProjectReadFacade      (samples, instruments, runtime, compiler diagnostics)
 *   - LanguageMetadata       (versioned supported-surface definitions — AI-3)
 *
 * The assembled JSON is injected into the llm-ls FIM request as `fim.prefix`
 * (and stored on GhostContext.authoringContext by HathorTab).
 *
 * Requirement references: AI-G3, AI-2, AI-3, AI-4, AI-8, AI-G1, AI-G2
 */

#include "CompletionContextProvider.hpp"
#include "ProjectReadFacade.hpp"

#include "hathor/LanguageMetadata.hpp"
#include "hathor/FewShotCorpus.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

namespace hathor::control {

using language::LanguageMetadata;
using language::MetadataCompatibility;
using language::FewShotCorpus;

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Current ISO-8601 UTC timestamp (mirrors AI-8's helper).
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

/// Detect language from a file path extension (mirrors AI-8).
std::string languageFromPath(std::string_view path)
{
    const auto pos = path.rfind('.');
    if (pos == std::string_view::npos)
        return {};
    const std::string ext = std::string(path.substr(pos + 1));
    if (ext == "hathor") return "mininotation";
    if (ext == "ck")     return "chuck";
    return {};
}

/// Trim leading/trailing whitespace.
std::string_view trimSV(std::string_view sv) noexcept
{
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
                           sv.front() == '\n' || sv.front() == '\r'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' ||
                           sv.back() == '\n' || sv.back() == '\r'))
        sv.remove_suffix(1);
    return sv;
}

/// Truncate a string to maxLen, appending a truncation marker.
std::string truncateStr(std::string_view content, int maxLen)
{
    if (maxLen <= 0 || static_cast<int>(content.size()) <= maxLen)
        return std::string(content);
    return std::string(content.substr(0, static_cast<std::size_t>(maxLen))) + "...[trunc]";
}

/// Read the line at 0-based `lineIdx` from `text` (empty if out of range).
std::string lineAt(std::string_view text, int lineIdx)
{
    if (lineIdx < 0) return {};
    std::size_t i = 0;
    int current = 0;
    while (current < lineIdx && i <= text.size())
    {
        std::size_t nl = text.find('\n', i);
        if (nl == std::string_view::npos) return {};
        i = nl + 1;
        ++current;
    }
    if (current != lineIdx) return {};
    std::size_t nl = text.find('\n', i);
    if (nl == std::string_view::npos)
        return std::string(text.substr(i));
    return std::string(text.substr(i, nl - i));
}

// ---------------------------------------------------------------------------
// Bounded example catalogue — AI-G4 owned, external, version-attributed.
//
// Examples are NOT hardcoded inline (see below for the rationale). They are
// loaded from reference/language-metadata/HathorFewShotExamples.json via the
// FewShotCorpus model (engine/include/hathor/FewShotCorpus.hpp) and injected
// through setFewShotCorpus(). Each example is tagged with the supported-surface
// version it is valid for; the corpus-as-a-whole is version-gated at load time
// against the AI-3 constants, so stale examples can never reach the model.
// AI-G3 selects only a small, relevant subset per request (assembleExamples).
//
// Surface identifiers — these are fallbacks used only when the AI-3 metadata or
// the AI-G4 corpus is not loaded. They mirror the k* constants in
// LanguageMetadata.hpp so the version gate never silently drifts.
// ---------------------------------------------------------------------------

/// Map an example context string (from the corpus JSON) back to a
/// CursorContextKind. Returns CursorContextKind::General for unknown strings.
CursorContextKind parseContextKind(std::string_view ctx)
{
    using K = CursorContextKind;
    if (ctx == "sample_expr")     return K::SampleExpr;
    if (ctx == "transform")       return K::Transform;
    if (ctx == "scale_expr")      return K::ScaleExpr;
    if (ctx == "rhythm")          return K::Rhythm;
    if (ctx == "ugen_decl")       return K::UgenDecl;
    if (ctx == "routing")         return K::Routing;
    if (ctx == "timing")          return K::Timing;
    if (ctx == "synth_section")   return K::SynthSection;
    return K::General;
}

// Strudel mini-notation surface version (matches kStrudelMiniNotationCompat).
const char* const kStrudelSurface = "1.2.6";
// ChucK integration surface (matches kChuckIntegrationSurface).
const char* const kChuckSurface   = "B4-K3";

// ---------------------------------------------------------------------------
// Bounded scale reference — version-attributed to the running Strudel surface.
// This is a *reference enumeration* (not a grammar/parser) so the model has a
// bounded hint of which scale names are valid. Stale when the surface version
// changes (see assembleMetadata scale-version gating).
// ---------------------------------------------------------------------------

static const char* const kStrudelScales[] = {
    "major", "minor", "dorian", "phrygian", "lydian", "mixolydian",
    "aeolian", "ionian", "locrian", "major2", "minor2", "major6", "minor6",
    "major7", "minor7", "major9", "minor9", "major11", "minor11",
    "major13", "minor13", "pentatonic", "majorPenta", "minorPenta",
    "blues", "chromatic", "whole", "wholeTone", "bebop", "harmonicMinor",
    "harmonicMajor", "phrygianDominant", "dorianFlat2", "lydianSharp5",
    "mixolydianFlat6", "locrianNatural2", "locrianSharp2", "locrianMajor",
    "superLocrian", "majorFlat5", "minorFlat5", "minorMajor", "minorMajor7",
    "majorPenta", "minorPenta", "egyptian", "indian", "japanese",
};

/// Names of transformation / effect functions used to classify a cursor that
/// sits right after one of these tokens.
static const std::vector<std::string> kTransformFunctions = {
    "fast", "slow", "stut", "every", "sometimes", "hurry", "density",
    "loopAt", "zoom", "when", "off", "striate", "slice", "splice", "fit",
    "iter", "rev", "palindrome", "shuffle", "scramble", "step", "whenmod",
    "linger", "echo", "clamp", "wrap", "coarse", "shift", "vowel", "shape",
    "jux", "juxby", "dist", "squeeze", "contrast", "map", "fastRel",
    "slowRel", "gain", "pan", "speed", "cutoff", "room", "delay", "orbit",
    "begin", "end", "legato", "cut", "segment", "part", "whenmod",
};

/// Read the identifier token immediately preceding `pos` in `line` (skipping
/// trailing whitespace). Returns "" if no alphabetic/underscore-started token.
std::string identifierBefore(std::string_view line, std::size_t pos)
{
    std::size_t i = pos;
    while (i > 0 && (line[i - 1] == ' ' || line[i - 1] == '\t'))
        --i;
    std::size_t end = i;
    std::size_t start = end;
    while (start > 0 &&
           ((line[start - 1] >= 'a' && line[start - 1] <= 'z') ||
            (line[start - 1] >= 'A' && line[start - 1] <= 'Z') ||
            line[start - 1] == '_' ||
            (line[start - 1] >= '0' && line[start - 1] <= '9')))
        --start;
    if (start == end) return {};
    return std::string(line.substr(start, end - start));
}

/// True if `name` appears as a whole identifier word in `text`.
bool containsWord(std::string_view text, std::string_view name)
{
    const auto* p = text.data();
    const auto n = static_cast<int>(name.size());
    for (std::size_t i = 0; i + n <= text.size(); ++i)
    {
        if (text.compare(i, n, name) != 0) continue;
        const bool leftOk  = (i == 0 || (!std::isalnum(static_cast<unsigned char>(p[i - 1])) && p[i-1] != '_'));
        const bool rightOk = (i + n == text.size() || (!std::isalnum(static_cast<unsigned char>(p[i + n])) && p[i+n] != '_'));
        if (leftOk && rightOk) return true;
    }
    return false;
}

/// Whether the identifier at the cursor position (scanning backward) matches.
std::string identifierAtCursor(std::string_view line, int character)
{
    if (character < 0) return {};
    std::size_t i = static_cast<std::size_t>(character);
    if (i > line.size()) i = line.size();
    while (i > 0 &&
           ((line[i - 1] >= 'a' && line[i - 1] <= 'z') ||
            (line[i - 1] >= 'A' && line[i - 1] <= 'Z') ||
            line[i - 1] == '_' ||
            (line[i - 1] >= '0' && line[i - 1] <= '9')))
        --i;
    if (i == static_cast<std::size_t>(character))
    {
        // cursor is in the middle of / start of a token — read forward too
        std::size_t end = character;
        while (end < line.size() &&
               ((line[end] >= 'a' && line[end] <= 'z') ||
                (line[end] >= 'A' && line[end] <= 'Z') ||
                line[end] == '_' ||
                (line[end] >= '0' && line[end] <= '9')))
            ++end;
        return std::string(line.substr(i, end - i));
    }
    std::size_t end = character;
    while (end < line.size() &&
           ((line[end] >= 'a' && line[end] <= 'z') ||
            (line[end] >= 'A' && line[end] <= 'Z') ||
            line[end] == '_' ||
            (line[end] >= '0' && line[end] <= '9')))
        ++end;
    return std::string(line.substr(i, end - i));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

CompletionContextProvider::CompletionContextProvider(
    ProjectReadFacade&                              readFacade,
    EditorContextProvider*                          editorCtx,
    LspContextProvider*                             lspCtx,
    const LanguageMetadata*                         metadata,
    const MetadataCompatibility*                    compat,
    const FewShotCorpus*                            corpus)
    : readFacade_(readFacade)
    , editorCtx_(editorCtx)
    , lspCtx_(lspCtx)
    , metadata_(metadata)
    , compat_(compat)
    , corpus_(corpus)
    , projectSymbolIndex_(nullptr)
    , projectRetrieval_(nullptr)
{
}

void CompletionContextProvider::setMetadata(
    const LanguageMetadata* metadata,
    const MetadataCompatibility* compat) noexcept
{
    metadata_ = metadata;
    compat_ = compat;
}

// ---------------------------------------------------------------------------
// J-5: Project symbol index wiring
// ---------------------------------------------------------------------------

void CompletionContextProvider::setProjectSymbolIndex(
    hathor::language::ProjectSymbolIndex* index) noexcept
{
    projectSymbolIndex_ = index;
    projectRetrieval_.setIndex(index);
}

// ---------------------------------------------------------------------------
// Metadata version / compatibility helpers
// ---------------------------------------------------------------------------

bool CompletionContextProvider::metadataCompatible() const noexcept
{
    if (metadata_ == nullptr) return false;
    if (compat_ == nullptr) return true;
    return compat_->compatible;
}

nlohmann::json CompletionContextProvider::metadataVersionBlock() const
{
    nlohmann::json mv;
    if (metadata_ == nullptr)
    {
        mv["available"] = false;
        mv["reason"] = "LanguageMetadata not loaded";
        return mv;
    }

    // Detect empty/default metadata (file was not found or failed to load).
    // A valid metadata has a non-zero schemaVersion.
    if (metadata_->schemaVersion == 0)
    {
        mv["available"] = false;
        mv["reason"] = "LanguageMetadata file not found or failed to load";
        mv["schema"] = 0;
        mv["compatible"] = false;
        return mv;
    }

    mv["schema"]      = metadata_->schemaVersion;
    mv["engine"]      = metadata_->hathorEngineCompat;
    mv["strudel"]     = metadata_->strudelMiniNotationCompat;
    mv["chuck"]       = metadata_->chuckLibVersion;
    mv["surface"]     = metadata_->chuckIntegrationSurface;
    mv["compatible"]  = metadataCompatible();
    if (!metadataCompatible() && compat_ != nullptr)
    {
        mv["errors"] = nlohmann::json::array();
        for (const auto& e : compat_->errors)
            mv["errors"].push_back(e);
    }
    return mv;
}

// ---------------------------------------------------------------------------
// Snapshot / language resolution
// ---------------------------------------------------------------------------

EditorContextSnapshot CompletionContextProvider::resolveSnapshot(
    const CompletionRequest& req) const
{
    if (editorCtx_ != nullptr)
    {
        auto snap = editorCtx_->snapshot();
        if (req.line >= 0)        snap.cursorLine = req.line;
        if (req.character >= 0)   snap.cursorChar = req.character;
        if (!req.file.empty())    snap.file = req.file;
        if (!req.uri.empty())     snap.uri = req.uri;
        if (!req.language.empty()) snap.language = req.language;
        if (!req.documentText.empty()) snap.content = req.documentText;
        return snap;
    }
    EditorContextSnapshot empty;
    empty.capturedAt = isoTimestamp();
    empty.cursorLine = req.line;
    empty.cursorChar = req.character;
    if (!req.file.empty())         empty.file = req.file;
    if (!req.uri.empty())          empty.uri = req.uri;
    if (!req.language.empty())     empty.language = req.language;
    if (!req.documentText.empty()) empty.content = req.documentText;
    else empty.content = req.documentText;
    if (!req.selectedText.empty())
    {
        empty.hasSelection = true;
        empty.selectedText = req.selectedText;
    }
    empty.hasContent = !req.file.empty() || !req.language.empty() || !req.documentText.empty();
    return empty;
}

std::string CompletionContextProvider::inferLanguage(
    std::string_view file,
    const std::string& reqLanguage,
    const EditorContextSnapshot& snap) const noexcept
{
    if (!reqLanguage.empty())
    {
        if (reqLanguage == "mininotation") return "mininotation";
        if (reqLanguage == "hathor")      return "mininotation";
        if (reqLanguage == "chuck")       return "chuck";
        return reqLanguage;
    }
    if (!file.empty())
    {
        const auto lang = languageFromPath(file);
        if (!lang.empty()) return lang;
    }
    if (!snap.language.empty()) return snap.language;
    if (!snap.file.empty())
    {
        const auto lang = languageFromPath(snap.file);
        if (!lang.empty()) return lang;
    }
    return "unknown";
}

std::string_view CompletionContextProvider::languageLabel(std::string_view language) noexcept
{
    if (language == "mininotation" || language == "hathor") return "hathor";
    if (language == "chuck") return "chuck";
    return "unknown";
}

// ---------------------------------------------------------------------------
// Cursor-context classification (deterministic)
// ---------------------------------------------------------------------------

CursorContext CompletionContextProvider::classifyCursorContext(
    std::string_view documentText,
    int line,
    int character,
    std::string_view language) const
{
    CursorContext result{CursorContextKind::General, "general", ""};

    const std::string currentLine = lineAt(documentText, line);
    const std::size_t offset = cursorToOffset(documentText, line, character);

    // Build a small bounded probe around the cursor for inspection/debugging.
    const std::size_t probeStart = offset > 120 ? offset - 120 : 0;
    const std::size_t probeEnd   = std::min(offset + 120, documentText.size());
    result.probe = std::string(documentText.substr(probeStart, probeEnd - probeStart));

    if (language == "chuck")
    {
        // --- ChucK classification ---
        // Routing: cursor adjacent to `=>`
        {
            std::size_t arrow = currentLine.find("=>");
            while (arrow != std::string::npos)
            {
                std::size_t arrowEnd = arrow + 2;
                if (static_cast<int>(character) >= static_cast<int>(arrow) - 4 &&
                    static_cast<int>(character) <= static_cast<int>(arrowEnd) + 4)
                {
                    result.kind  = CursorContextKind::Routing;
                    result.label = "audio-graph routing (=>)";
                    return result;
                }
                arrow = currentLine.find("=>", arrowEnd);
            }
        }
        // Timing: cursor near `now` or a duration literal (`::`).
        {
            const std::string id = identifierAtCursor(currentLine, character);
            if (id == "now")
            {
                result.kind = CursorContextKind::Timing;
                result.label = "time advancement / `now`";
                return result;
            }
            // `::` duration token nearby (e.g. 10::ms)
            if (currentLine.find("::", 0) != std::string::npos &&
                currentLine.substr(0, static_cast<std::size_t>(character)).find("::") != std::string::npos)
            {
                result.kind = CursorContextKind::Timing;
                result.label = "duration / time advancement";
                return result;
            }
        }
        // Synth section: envelope / filter / oscillator UGen names nearby.
        if (metadata_ != nullptr)
        {
            bool hasSynthUgen = false;
            for (const auto& ca : metadata_->chuckApi)
            {
                if (!ca.supported) continue;
                if (ca.kind != "ugen") continue;
                const std::string lower = ca.name;
                if (lower == "LPF" || lower == "HPF" || lower == "BPF" ||
                    lower == "BRF" || lower == "Envelope" || lower == "ADSR" ||
                    lower == "Gain" || lower == "Pan2" ||
                    (lower.find("Osc") != std::string::npos) ||
                    lower.find("Filter") != std::string::npos ||
                    lower.find("Env") != std::string::npos)
                {
                    if (containsWord(result.probe, lower))
                    {
                        hasSynthUgen = true;
                        break;
                    }
                }
            }
            if (hasSynthUgen)
            {
                result.kind = CursorContextKind::SynthSection;
                result.label = "envelope/filter/oscillator section";
                return result;
            }
        }
        // UGen declaration: identifier at cursor matches a known UGen/class.
        if (metadata_ != nullptr)
        {
            const std::string id = identifierAtCursor(currentLine, character);
            for (const auto& ca : metadata_->chuckApi)
            {
                if (ca.supported && (ca.kind == "ugen" || ca.kind == "class") && ca.name == id)
                {
                    result.kind = CursorContextKind::UgenDecl;
                    result.label = "unit generator declaration";
                    return result;
                }
            }
        }
        // Fallback: identifier at cursor is a known UGen/class name.
        if (metadata_ != nullptr)
        {
            const std::string id = identifierAtCursor(currentLine, character);
            for (const auto& ca : metadata_->chuckApi)
            {
                if (ca.supported && ca.name == id)
                {
                    result.kind = CursorContextKind::UgenDecl;
                    result.label = "unit generator reference";
                    return result;
                }
            }
        }
        result.kind = CursorContextKind::General;
        result.label = "chuck source";
        return result;
    }

    // --- mini-notation (.hathor) classification ---
    // 1. Inside a double-quoted string? Determine the preceding keyword.
    {
        const std::size_t lineOffsetStart =
            [documentText, &line]() {
                std::size_t o = 0;
                int cur = 0;
                while (cur < line)
                {
                    std::size_t nl = documentText.find('\n', o);
                    if (nl == std::string_view::npos) return o;
                    o = nl + 1;
                    ++cur;
                }
                return o;
            }();
        // Find the opening quote to the left of the cursor on this line.
        std::size_t openQuote = std::string::npos;
        for (std::size_t i = character; i > 0; --i)
        {
            if (currentLine[i - 1] == '"') { openQuote = i - 1; break; }
            if (currentLine[i - 1] == '\n' || currentLine[i - 1] == '\r') break;
        }
        // Find the closing quote to the right.
        bool hasClosing = false;
        for (std::size_t i = character; i < currentLine.size(); ++i)
        {
            if (currentLine[i] == '"') { hasClosing = true; break; }
        }
        if (openQuote != std::string::npos)
        {
            std::string before(lineAt(documentText, line).substr(0, character));
            // Trim back to the opening quote.
            if (openQuote < before.size()) before = before.substr(0, openQuote);
            const std::string kw = identifierBefore(before, before.size());
            if (kw == "scale")
            {
                result.kind = CursorContextKind::ScaleExpr;
                result.label = "inside scale expression";
            }
            else
            {
                result.kind = CursorContextKind::SampleExpr;
                result.label = "inside sample string";
            }
            (void)hasClosing;
            (void)lineOffsetStart;
            return result;
        }
        // Scale `|` syntax: cursor inside `|...|` after a `scale`/`degree` token.
        if (currentLine.find('|') != std::string::npos)
        {
            const std::string kw = identifierBefore(currentLine, character);
            if (kw == "scale" || kw == "degree" || kw == "note" || kw == "n")
            {
                result.kind = CursorContextKind::ScaleExpr;
                result.label = "inside scale expression";
                return result;
            }
        }
        // 2. Transformation function call (identifier at cursor or just after).
        // If the preceding word is a known transform function → Transform.
        {
            const std::string kw = identifierBefore(currentLine, character);
            if (!kw.empty())
            {
                if (std::find(kTransformFunctions.begin(), kTransformFunctions.end(), kw)
                    != kTransformFunctions.end())
                {
                    result.kind = CursorContextKind::Transform;
                    result.label = "transformation function";
                    return result;
                }
            }
        }
        // 3. Rhythm / pattern structure: the line contains pattern operators or
        // space-separated sample names.
        {
            const bool hasPatternOp =
                currentLine.find('*') != std::string::npos ||
                currentLine.find('/') != std::string::npos ||
                currentLine.find('!') != std::string::npos ||
                currentLine.find('[') != std::string::npos ||
                currentLine.find(']') != std::string::npos ||
                currentLine.find('(') != std::string::npos ||
                currentLine.find(',') != std::string::npos ||
                currentLine.find('~') != std::string::npos ||
                currentLine.find('<') != std::string::npos;
            if (hasPatternOp)
            {
                result.kind = CursorContextKind::Rhythm;
                result.label = "pattern / rhythmic structure";
                return result;
            }
        }
    }

    result.kind = CursorContextKind::General;
    if (language == "mininotation") result.label = "mini-notation pattern";
    return result;
}

// ---------------------------------------------------------------------------
// assembleRegion — bounded surrounding source region
// ---------------------------------------------------------------------------

nlohmann::json CompletionContextProvider::assembleRegion(
    const CompletionRequest& req,
    const EditorContextSnapshot& snap) const
{
    const auto& bounds = resolveBounds(req);
    const std::string& text = req.documentText.empty() ? snap.content : req.documentText;

    nlohmann::json region;
    if (text.empty())
    {
        region["available"] = false;
        region["reason"] = "no document text available";
        return region;
    }

    const int maxLines = bounds.maxRegionLines;
    const int startLine = std::max(0, req.line - maxLines);
    const int endLine   = req.line + maxLines;

    std::string_view window;
    std::string      windowStr;
    // Extract the bounded window of lines.
    {
        std::size_t o = 0;
        int cur = 0;
        while (cur < startLine)
        {
            std::size_t nl = text.find('\n', o);
            if (nl == std::string_view::npos) { windowStr = ""; break; }
            o = nl + 1; ++cur;
        }
        std::size_t start = o;
        int cur2 = startLine;
        while (cur2 < endLine)
        {
            std::size_t nl = text.find('\n', o);
            if (nl == std::string_view::npos) { o = text.size(); break; }
            o = nl + 1; ++cur2;
        }
        windowStr = std::string(text.substr(start, o - start));
        window = windowStr;
    }

    region["available"] = true;
    region["line"] = req.line;
    region["character"] = req.character;
    region["window_lines"] = (endLine - startLine + 1);

    // The current line text (for context probes).
    region["current_line"] = truncateStr(lineAt(window, req.line - startLine),
                                         bounds.maxSurroundingChars);

    // Bounded multi-line window, char-capped.
    region["surrounding"] = truncateStr(window, bounds.maxSurroundingChars);
    region["surrounding_truncated"] =
        static_cast<int>(window.size()) > bounds.maxSurroundingChars;

    return region;
}

// ---------------------------------------------------------------------------
// assembleDiagnostics — proximity-ordered, bounded (compiler + LSP / AI-4)
// ---------------------------------------------------------------------------

namespace {

/// A diagnostic with a proximity hint (line distance from the cursor).
struct RankedDiag {
    int lineDelta;          // abs(line - cursorLine)
    int charDelta;          // abs(char - cursorChar) if same line, else large
    nlohmann::json diag;
};

} // namespace

nlohmann::json CompletionContextProvider::assembleDiagnostics(
    const CompletionRequest& req,
    const EditorContextSnapshot& snap,
    std::string_view language) const
{
    const auto& bounds = resolveBounds(req);
    const bool isChuck = (language == "chuck");
    nlohmann::json result;
    result["sources"] = nlohmann::json::array();
    result["diagnostics"] = nlohmann::json::array();

    std::vector<RankedDiag> ranked;

    // --- 1. Compiler diagnostics (real parser/compiler — ProjectReadFacade/AI-2) ---
    if (!snap.content.empty())
    {
        const std::string content = req.documentText.empty() ? snap.content : req.documentText;
        const std::string sourceId = snap.uri.empty()
            ? (snap.slotName.empty() ? std::string("untitled") : ("slot:" + snap.slotName))
            : snap.uri;
        auto compilerDiags = readFacade_.getDiagnostics(content, sourceId, isChuck);
        if (compilerDiags.value("ok", false))
        {
            for (const auto& d : compilerDiags.value("diagnostics", nlohmann::json::array()))
            {
                RankedDiag rd;
                rd.diag = d;
                const int dline = d.value("location", nlohmann::json::object())
                                     .value("line", req.line);
                const int dchar = d.value("location", nlohmann::json::object())
                                     .value("column", req.character);
                rd.lineDelta = std::abs(dline - req.line);
                rd.charDelta = (dline == req.line) ? std::abs(dchar - req.character) : 100000;
                ranked.push_back(std::move(rd));
            }
            result["sources"].push_back("compiler");
        }
        else
        {
            result["compiler_unavailable"] = true;
        }
    }

    // --- 2. LSP diagnostics (AI-4) — only if the LSP is healthy ---
    if (lspCtx_ != nullptr)
    {
        auto status = lspCtx_->lspStatus();
        result["lsp_available"] = status.value("ok", false);
        if (status.value("ok", false) && !snap.uri.empty())
        {
            auto lspDiags = lspCtx_->diagnosticsForDocument(snap.uri);
            for (const auto& d : lspDiags.value("diagnostics", nlohmann::json::array()))
            {
                RankedDiag rd;
                rd.diag = d;
                const int dline = d.value("line", req.line);
                const int dchar = d.value("column", req.character);
                rd.lineDelta = std::abs(dline - req.line);
                rd.charDelta = (dline == req.line) ? std::abs(dchar - req.character) : 100000;
                ranked.push_back(std::move(rd));
            }
            result["sources"].push_back("lsp");
        }
        else if (!status.value("ok", false))
        {
            result["lsp_reason"] = status.value("reason", "LSP unavailable");
        }
    }
    else
    {
        result["lsp_available"] = false;
    }

    // --- Proximity ordering: nearest diagnostics first. ---
    std::sort(ranked.begin(), ranked.end(),
              [](const RankedDiag& a, const RankedDiag& b)
              {
                  if (a.lineDelta != b.lineDelta) return a.lineDelta < b.lineDelta;
                  return a.charDelta < b.charDelta;
              });

    // --- Bound the diagnostics (nearest maxDiagnostics win). ---
    int emitted = 0;
    for (const auto& rd : ranked)
    {
        if (emitted >= bounds.maxDiagnostics) break;
        // Lightweight copy of the diagnostic (avoid surfacing huge messages).
        nlohmann::json d = rd.diag;
        result["diagnostics"].push_back(d);
        ++emitted;
    }
    result["count"] = emitted;
    result["max"] = bounds.maxDiagnostics;

    return result;
}

// ---------------------------------------------------------------------------
// assembleMetadata — relevance-filtered, version-gated (AI-3)
// ---------------------------------------------------------------------------

nlohmann::json CompletionContextProvider::assembleMetadata(
    const CompletionRequest& req,
    std::string_view language) const
{
    const auto& bounds = resolveBounds(req);
    nlohmann::json result = metadataVersionBlock();

    if (metadata_ == nullptr)
    {
        result["available"] = false;
        result["reason"] = "LanguageMetadata not loaded";
        return result;
    }

    // Detect empty/default metadata (file not found or failed to load).
    if (metadata_->schemaVersion == 0)
    {
        result["available"] = false;
        result["reason"] = "LanguageMetadata file not found or failed to load";
        return result;
    }

    if (!metadataCompatible())
    {
        result["available"] = false;
        result["reason"] = "metadata incompatible with running surface";
        return result;
    }

    result["available"] = true;

    if (language == "mininotation" || language == "hathor")
    {
        // Functions — filtered by relevance to the cursor context.
        nlohmann::json funcs = nlohmann::json::array();
        for (const auto& fn : metadata_->functions)
        {
            if (!fn.supported) continue;
            nlohmann::json f;
            f["name"] = fn.name;
            f["category"] = fn.category;
            if (fn.signature.size() <= 64)       f["signature"] = fn.signature;
            if (fn.description.size() <= 160)    f["description"] = fn.description;
            if (fn.example) f["example"] = truncateStr(*fn.example, 120);
            if (static_cast<int>(funcs.size()) >= bounds.maxMetadataEntries) break;
            funcs.push_back(std::move(f));
        }
        result["functions"] = std::move(funcs);

        // Operators — bounded.
        nlohmann::json ops = nlohmann::json::array();
        for (const auto& op : metadata_->operators)
        {
            nlohmann::json o;
            o["name"] = op.name;
            o["description"] = truncateStr(op.description, 120);
            if (static_cast<int>(ops.size()) >= bounds.maxOperators) break;
            ops.push_back(std::move(o));
        }
        result["operators"] = std::move(ops);

        // Grammar elements — bounded.
        nlohmann::json grammar = nlohmann::json::array();
        for (const auto& ge : metadata_->grammar)
        {
            if (!ge.supported) continue;
            nlohmann::json g;
            g["name"] = ge.name;
            g["syntax"] = truncateStr(ge.syntax, 80);
            if (static_cast<int>(grammar.size()) >= bounds.maxGrammarEntries) break;
            grammar.push_back(std::move(g));
        }
        result["grammar"] = std::move(grammar);

        // Params — bounded.
        nlohmann::json params = nlohmann::json::array();
        for (const auto& pd : metadata_->params)
        {
            if (!pd.supported) continue;
            nlohmann::json p;
            p["key"] = pd.key;
            p["value_type"] = pd.valueType;
            if (static_cast<int>(params.size()) >= bounds.maxMetadataEntries) break;
            params.push_back(std::move(p));
        }
        result["params"] = std::move(params);

        // Supported scales — version-attributed reference enumeration.
        // Only surfaced when the Strudel mini-notation surface version matches
        // the running surface (stale-metadata rejection).  The canonical scale
        // list lives in the Strudel LSP (AI-4); this bounded reference is gated
        // on kStrudelMiniNotationCompat so it can never silently drift.
        nlohmann::json scales = nlohmann::json::array();
        if (metadata_->strudelMiniNotationCompat == kStrudelSurface)
        {
            for (const auto* s : kStrudelScales)
                scales.push_back(s);
        }
        result["scales"] = std::move(scales);
        result["scales_version"] = metadata_->strudelMiniNotationCompat;
        result["scales_source"] = "AI-3 versioned reference + Strudel LSP (AI-4) completions";

        // Sample definitions from metadata — bounded.
        nlohmann::json sampleDefs = nlohmann::json::array();
        for (const auto& sd : metadata_->samples)
        {
            nlohmann::json s;
            s["name"] = sd.name;
            s["category"] = sd.category;
            if (static_cast<int>(sampleDefs.size()) >= bounds.maxMetadataEntries) break;
            sampleDefs.push_back(std::move(s));
        }
        result["sample_definitions"] = std::move(sampleDefs);
    }
    else if (language == "chuck")
    {
        // ChucK API — filtered to supported surface, bounded, relevance-ordered.
        nlohmann::json api = nlohmann::json::array();
        for (const auto& ca : metadata_->chuckApi)
        {
            if (!ca.supported) continue;
            nlohmann::json c;
            c["name"] = ca.name;
            c["kind"] = ca.kind;
            c["signature"] = truncateStr(ca.signature, 96);
            if (ca.example) c["example"] = truncateStr(*ca.example, 120);
            if (static_cast<int>(api.size()) >= bounds.maxMetadataEntries) break;
            api.push_back(std::move(c));
        }
        result["chuck_api"] = std::move(api);
    }

    return result;
}

// ---------------------------------------------------------------------------
// assembleSamples — bounded, relevance-filtered SampleBank (AI-2)
// ---------------------------------------------------------------------------

nlohmann::json CompletionContextProvider::assembleSamples(
    const CompletionRequest& req,
    const CursorContext& ctx) const
{
    const auto& bounds = resolveBounds(req);
    nlohmann::json result;
    auto raw = readFacade_.listSamples();
    result["ok"] = raw.value("ok", false);

    if (!raw.value("ok", false))
    {
        result["reason"] = raw.value("error", "samples unavailable");
        result["samples"] = nlohmann::json::array();
        return result;
    }

    // Relevance: if the cursor is inside a sample string, the user is typing a
    // sample name — order/favor samples whose name starts with the typed prefix.
    std::string prefix;
    if (ctx.kind == CursorContextKind::SampleExpr ||
        ctx.kind == CursorContextKind::Rhythm)
    {
        prefix = trimSV(req.selectedText);
        if (prefix.empty())
        {
            // Derive prefix from the token being typed at the cursor.
            const std::string line = lineAt(req.documentText.empty() ? "" : req.documentText, req.line);
            // Find the last whitespace/quote-delimited token before the cursor.
            std::string_view tail = line;
            if (static_cast<int>(line.size()) > req.character)
                tail = std::string_view(line).substr(0, static_cast<std::size_t>(req.character));
            // Take the last token of the tail.
            std::size_t sp = tail.find_last_of(" \t\"");
            prefix = std::string(tail.substr(sp == std::string_view::npos ? 0 : sp + 1));
        }
    }

    auto samples = raw.value("samples", nlohmann::json::array());
    std::vector<nlohmann::json> ordered;
    ordered.reserve(samples.size());
    for (const auto& s : samples)
        ordered.push_back(s);

    // Move prefix-matching samples to the front (semantic relevance).
    if (!prefix.empty())
    {
        std::stable_sort(ordered.begin(), ordered.end(),
            [&prefix](const nlohmann::json& a, const nlohmann::json& b)
            {
                const std::string na = a.value("name", std::string{});
                const std::string nb = b.value("name", std::string{});
                const bool aMatch = na.rfind(prefix, 0) == 0;
                const bool bMatch = nb.rfind(prefix, 0) == 0;
                if (aMatch != bMatch) return aMatch;
                bool aEq = (na == prefix);
                bool bEq = (nb == prefix);
                if (aEq != bEq) return aEq;
                return na < nb;
            });
    }

    nlohmann::json bounded = nlohmann::json::array();
    int emitted = 0;
    for (const auto& s : ordered)
    {
        if (emitted >= bounds.maxSamples) break;
        nlohmann::json slim;
        slim["name"] = s.value("name", std::string{});
        slim["index"] = s.value("index", 0);
        if (s.contains("duration_seconds")) slim["duration_seconds"] = s["duration_seconds"];
        if (s.contains("channels"))          slim["channels"] = s["channels"];
        bounded.push_back(std::move(slim));
        ++emitted;
    }
    result["samples"] = std::move(bounded);
    result["count"] = emitted;
    result["max"] = bounds.maxSamples;
    if (!prefix.empty()) result["filter_prefix"] = prefix;
    return result;
}

// ---------------------------------------------------------------------------
// assembleInstruments — bounded, relevance-filtered baked ChucK instruments
// ---------------------------------------------------------------------------

nlohmann::json CompletionContextProvider::assembleInstruments(
    const CompletionRequest& req,
    const CursorContext& ctx) const
{
    (void)ctx;
    const auto& bounds = resolveBounds(req);
    nlohmann::json result;

    std::string projectDir;
    {
        auto proj = readFacade_.inspectProject();
        projectDir = proj.value("project_dir", std::string{});
    }

    auto raw = readFacade_.listChuckInstruments(projectDir);
    result["ok"] = raw.value("ok", false);
    if (!raw.value("ok", false))
    {
        result["instruments"] = nlohmann::json::array();
        result["reason"] = raw.value("error", "instruments unavailable");
        return result;
    }

    auto instruments = raw.value("instruments", nlohmann::json::array());

    // Relevance: for ChucK synth-section / routing contexts, surface instrument
    // names (they are usable as sample names via `s "instrument"`).
    nlohmann::json bounded = nlohmann::json::array();
    int emitted = 0;
    for (const auto& in : instruments)
    {
        if (emitted >= bounds.maxInstruments) break;
        nlohmann::json slim;
        slim["name"] = in.value("name", std::string{});
        slim["resource_id"] = in.value("resource_id", std::string{});
        slim["lifecycle_state"] = in.value("lifecycle_state", std::string{"unknown"});
        if (in.contains("rendered_wav_exists"))
            slim["rendered"] = in["rendered_wav_exists"].get<bool>();
        bounded.push_back(std::move(slim));
        ++emitted;
    }
    result["instruments"] = std::move(bounded);
    result["count"] = emitted;
    result["max"] = bounds.maxInstruments;
    return result;
}

// ---------------------------------------------------------------------------
// assembleExamples — bounded, version-compatible, context-keyed (AI-G4)
//
// Reads from the AI-G4 few-shot corpus (loaded externally and injected via
// setFewShotCorpus). Selection is AI-G3's retrieval job: only a small,
// relevant subset reaches the model. Version-gating at corpus-load time
// (FewShotCorpus::compatible) already rejected stale examples; the per-example
// surface_version check is a secondary safety net.
// ---------------------------------------------------------------------------

nlohmann::json CompletionContextProvider::assembleExamples(
    const CompletionRequest& req,
    const CursorContext& ctx) const
{
    const auto& bounds = resolveBounds(req);
    nlohmann::json result;
    result["examples"] = nlohmann::json::array();
    result["count"]    = 0;
    result["max"]      = bounds.maxExamples;
    result["version_attributed"] = false;

    // No corpus loaded → no few-shot examples (never fall back to hardcoded
    // inline examples; the corpus is the single source of truth for AI-G4).
    if (corpus_ == nullptr) {
        result["available"] = false;
        result["reason"] = "FewShotCorpus not loaded";
        result["examples"] = nlohmann::json::array();
        return result;
    }

    // Corpus-level version gate: if the AI-G4 corpus was rejected at load time
    // (surface version mismatch with AI-3), emit zero examples.
    if (!corpus_->compatible) {
        result["available"] = false;
        result["reason"] = "FewShotCorpus incompatible with running surface";
        if (!corpus_->errors.empty())
            result["errors"] = corpus_->errors;
        return result;
    }

    result["available"] = true;
    result["version_attributed"] = true;

    // Resolve effective language label used for example tagging.
    const std::string language = req.language;
    std::string effLang;
    if (language == "mininotation" || language == "hathor") effLang = "mininotation";
    else if (language == "chuck")                            effLang = "chuck";
    else                                                    effLang = "mininotation";

    // Resolve the running surface version (from AI-3 metadata, with fallback).
    std::string surface;
    if (effLang == "chuck")
        surface = metadata_ ? metadata_->chuckIntegrationSurface : kChuckSurface;
    else
        surface = metadata_ ? metadata_->strudelMiniNotationCompat : kStrudelSurface;

    const CursorContextKind kind = ctx.kind;

    // Pass 1: exact-context matches.
    // Pass 2: General fallback (only if no exact matches found).
    std::vector<const language::FewShotExample*> matches;
    std::vector<const language::FewShotExample*> general;

    for (const auto& ex : corpus_->examples) {
        if (ex.language != effLang) continue;
        if (ex.surfaceVersion != surface) continue;   // stale — reject
        const CursorContextKind exKind = parseContextKind(ex.context);
        if (exKind == kind)          matches.push_back(&ex);
        if (exKind == CursorContextKind::General) general.push_back(&ex);
    }

    nlohmann::json examples = nlohmann::json::array();
    int emitted = 0;
    for (const auto* ex : matches) {
        if (emitted >= bounds.maxExamples) break;
        nlohmann::json e;
        e["title"]  = ex->title;
        e["language"] = ex->language;
        e["surface_version"] = ex->surfaceVersion;
        e["context"] = ex->context;
        e["code"] = truncateStr(ex->code, 320);
        if (!ex->validatesAgainst.empty())
            e["validates_against"] = ex->validatesAgainst;
        examples.push_back(std::move(e));
        ++emitted;
    }
    // Fill remaining budget with general examples.
    for (const auto* ex : general) {
        if (emitted >= bounds.maxExamples) break;
        nlohmann::json e;
        e["title"]  = ex->title;
        e["language"] = ex->language;
        e["surface_version"] = ex->surfaceVersion;
        e["context"] = ex->context;
        e["code"] = truncateStr(ex->code, 320);
        if (!ex->validatesAgainst.empty())
            e["validates_against"] = ex->validatesAgainst;
        examples.push_back(std::move(e));
        ++emitted;
    }

    result["examples"] = std::move(examples);
    result["count"] = emitted;
    result["selected_context"] = ctx.label;
    return result;
}

// ---------------------------------------------------------------------------
// assembleRuntime — bounded tempo/transport/slot (AI-2)
// ---------------------------------------------------------------------------

nlohmann::json CompletionContextProvider::assembleRuntime(
    const CompletionRequest& req,
    const EditorContextSnapshot& snap,
    std::string_view language) const
{
    (void)req;
    nlohmann::json result;
    auto status = readFacade_.getAudioStatus();
    result["ok"] = status.value("ok", false);

    nlohmann::json transport = nlohmann::json::object();
    if (status.contains("transport"))
        transport = status["transport"];
    result["bpm"] = transport.value("bpm", 0.0);
    result["running"] = transport.value("running", false);
    result["sample_rate"] = transport.value("sample_rate", 0);

    // Prefer the file's declared front-matter BPM when present.
    if (snap.frontMatterBpm > 0.0)
        result["declared_bpm"] = snap.frontMatterBpm;
    result["slot_name"] = snap.slotName.empty()
        ? (snap.frontMatterSlot.empty() ? std::string{} : snap.frontMatterSlot)
        : snap.slotName;
    result["slot_index"] = snap.slotIndex;

    // Current pattern notation for this slot (bounded), if available.
    {
        std::string notation;
        for (const auto& s : status.value("slots", nlohmann::json::array()))
        {
            if (s.value("slot_name", std::string{}) == result["slot_name"].get<std::string>())
            {
                notation = s.value("notation", std::string{});
                break;
            }
        }
        if (!notation.empty())
            result["slot_pattern"] = truncateStr(notation, 256);
    }

    if (language == "chuck")
    {
        const auto vmStatus = status.value("worker", nlohmann::json::object());
        result["vm_alive"] = vmStatus.value("alive", false);
    }
    return result;
}

// ---------------------------------------------------------------------------
// assembleProject — compact project overview (not the whole repo)
// ---------------------------------------------------------------------------

nlohmann::json CompletionContextProvider::assembleProject(const CompletionRequest& req) const
{
    const auto& bounds = resolveBounds(req);
    auto proj = readFacade_.inspectProject();

    nlohmann::json result;
    result["ok"] = proj.value("ok", false);
    result["project_name"] = proj.value("project_name", std::string{});
    result["bpm"] = proj.value("bpm", 0.0);
    result["current_song"] = proj.value("current_song", nullptr);

    // Active slot names + names of referenced assets only (bounded).
    nlohmann::json slots = nlohmann::json::array();
    for (const auto& s : proj.value("active_slots", nlohmann::json::array()))
        slots.push_back(s);
    result["active_slots"] = std::move(slots);

    nlohmann::json sampleNames = truncateArray(
        proj.value("sample_names", nlohmann::json::array()), bounds.maxSamples);
    result["sample_names"] = std::move(sampleNames);

    result["samples_count"] = proj.value("samples_count", 0);
    result["chuck_instruments_count"] =
        proj.value("chuck_instruments", nlohmann::json::array()).size();

    return result;
}

// ---------------------------------------------------------------------------
// assembleProjectRetrieval — J-5 bounded, ranked project snippets
// ---------------------------------------------------------------------------

nlohmann::json CompletionContextProvider::assembleProjectRetrieval(
    const CompletionRequest& req,
    const CursorContext& ctx,
    std::string_view language) const
{
    const auto& bounds = resolveBounds(req);

    nlohmann::json result;
    result["ok"] = (projectSymbolIndex_ != nullptr);
    result["available"] = (projectSymbolIndex_ != nullptr);

    if (projectSymbolIndex_ == nullptr)
    {
        result["reason"] = "ProjectSymbolIndex not bound";
        return result;
    }

    // Build the retrieval context from the current edit location.
    RetrievalContext rctx;
    rctx.language = std::string(language);
    rctx.cursorContextKind = [k = ctx.kind] {
        switch (k) {
            case CursorContextKind::SampleExpr:   return "sample_expr";
            case CursorContextKind::Transform:    return "transform";
            case CursorContextKind::ScaleExpr:    return "scale_expr";
            case CursorContextKind::Rhythm:       return "rhythm";
            case CursorContextKind::UgenDecl:     return "ugen_decl";
            case CursorContextKind::Routing:      return "routing";
            case CursorContextKind::Timing:       return "timing";
            case CursorContextKind::SynthSection: return "synth_section";
            case CursorContextKind::General:      return "general";
        }
        return "general";
    }();
    rctx.cursorContextLabel = ctx.label;
    rctx.currentFile = req.file;

    // Derive the typed text (partial token at the cursor).
    {
        const std::string currentLine = lineAt(
            req.documentText.empty() ? std::string{} : req.documentText, req.line);
        rctx.typedText = identifierAtCursor(currentLine, req.character);
    }

    // Build bounded retrieval limits from the ContextBounds.
    RetrievalBounds rbounds;
    rbounds.maxSnippets = bounds.maxProjectSnippets;
    rbounds.maxSnippetChars = bounds.maxProjectSnippetChars;
    rbounds.maxTotalChars = bounds.maxProjectRetrievalChars;
    rbounds.maxFiles = bounds.maxInstruments;  // reuse the instrument count for file listing
    rbounds.maxSearchedSymbols = 30;

    auto retrieved = projectRetrieval_.retrieve(rctx, rbounds);

    // Enforce the overall context budget as a hard cap on total chars.
    // The retrieve() method already bounds by maxTotalChars, but we
    // double-check the serialized form fits within maxContextChars.
    const auto serialized = retrieved.dump();
    if (static_cast<int>(serialized.size()) > bounds.maxProjectRetrievalChars)
    {
        // Trim the snippets array until we fit.
        auto& snippets = retrieved["snippets"];
        while (!snippets.empty() &&
               static_cast<int>(retrieved.dump().size()) > bounds.maxProjectRetrievalChars)
        {
            snippets.erase(std::prev(snippets.end()));
            retrieved["count"] = snippets.size();
            retrieved["truncated"] = true;
        }
    }

    result["version_token"] = retrieved.value("version_token", std::string{});
    result["query"] = retrieved.value("query", std::string{});
    result["snippets"] = retrieved["snippets"];
    result["files"] = retrieved["files"];
    result["count"] = retrieved.value("count", 0);
    result["file_count"] = retrieved.value("file_count", 0);
    result["truncated"] = retrieved.value("truncated", false);

    return result;
}

// ---------------------------------------------------------------------------
// Intent classification (J-4) — single intent-aware path, no second classifier
// ---------------------------------------------------------------------------

std::string_view CompletionContextProvider::intentLabel(IntentKind intent) noexcept
{
    switch (intent)
    {
        case IntentKind::Continue:  return "continue";
        case IntentKind::Transform: return "transform";
        case IntentKind::Densify:   return "densify";
        case IntentKind::Repair:    return "repair";
        case IntentKind::General:   return "general";
    }
    return "general";
}

IntentKind CompletionContextProvider::classifyIntent(
    const CursorContext& cursorCtx,
    const nlohmann::json& diagnostics) const
{
    // Repair takes top priority: if there are any error or warning
    // diagnostics near the cursor, the author's intent is most likely
    // to fix/repair.  Info-level diagnostics (e.g. "parsed successfully")
    // do NOT indicate a repair intent.
    int problemCount = 0;
    for (const auto& d : diagnostics.value("diagnostics", nlohmann::json::array()))
    {
        const auto severity = d.value("severity", std::string{});
        if (severity == "error" || severity == "warning")
            ++problemCount;
    }
    if (problemCount > 0)
        return IntentKind::Repair;

    // Cursor-context-driven intent mapping.
    switch (cursorCtx.kind)
    {
        case CursorContextKind::Transform:
            // Cursor after/inside a transformation function → transform intent.
            return IntentKind::Transform;

        case CursorContextKind::Rhythm:
        case CursorContextKind::SampleExpr:
            // In a pattern or sample expression, the author is likely building
            // up density of events — densify intent.
            return IntentKind::Densify;

        case CursorContextKind::ScaleExpr:
        case CursorContextKind::UgenDecl:
        case CursorContextKind::Routing:
        case CursorContextKind::Timing:
        case CursorContextKind::SynthSection:
            // These are all continuation contexts: the author is writing the
            // next piece of code at a syntactically meaningful boundary.
            return IntentKind::Continue;

        case CursorContextKind::General:
            // No specific classification — default to continue (the model
            // should generate the next likely token/line).
            return IntentKind::General;
    }
    return IntentKind::General;
}

// ---------------------------------------------------------------------------

CompletionContext CompletionContextProvider::assemble(const CompletionRequest& req) const
{
    CompletionContext out;
    out.ok = true;

    const auto snap = resolveSnapshot(req);
    const std::string language = inferLanguage(req.file, req.language, snap);
    out.language = std::string(languageLabel(language));

    const CursorContext ctx = classifyCursorContext(
        req.documentText.empty() ? snap.content : req.documentText,
        req.line, req.character, language);
    out.cursorContextLabel = ctx.label;

    const auto& bounds = resolveBounds(req);

    nlohmann::json ctxJson;
    ctxJson["ok"] = true;
    ctxJson["language"] = out.language;
    ctxJson["cursor_context"] = ctx.label;
    ctxJson["cursor_context_kind"] = [k = ctx.kind] {
        switch (k) {
            case CursorContextKind::SampleExpr:   return "sample_expr";
            case CursorContextKind::Transform:    return "transform";
            case CursorContextKind::ScaleExpr:    return "scale_expr";
            case CursorContextKind::Rhythm:       return "rhythm";
            case CursorContextKind::UgenDecl:     return "ugen_decl";
            case CursorContextKind::Routing:      return "routing";
            case CursorContextKind::Timing:       return "timing";
            case CursorContextKind::SynthSection: return "synth_section";
            case CursorContextKind::General:      return "general";
        }
        return "general";
    }();
    ctxJson["metadata_version"] = metadataVersionBlock();
    ctxJson["bounds"] = nlohmann::json{
        {"max_examples",        bounds.maxExamples},
        {"max_samples",         bounds.maxSamples},
        {"max_instruments",     bounds.maxInstruments},
        {"max_diagnostics",     bounds.maxDiagnostics},
        {"max_metadata_entries",bounds.maxMetadataEntries},
        {"max_region_lines",    bounds.maxRegionLines},
        {"max_context_chars",   bounds.maxContextChars},
    };

    // --- editor (bounded) ---
    {
        nlohmann::json editor;
        editor["file"] = snap.file.empty() ? nullptr : nlohmann::json(snap.file);
        editor["uri"] = snap.uri.empty() ? nullptr : nlohmann::json(snap.uri);
        editor["language"] = out.language;
        editor["cursor"] = nlohmann::json{{"line", req.line}, {"character", req.character}};
        if (snap.hasSelection || !req.selectedText.empty())
        {
            editor["has_selection"] = true;
            editor["selected_text"] = truncateStr(
                snap.selectedText.empty() ? req.selectedText : snap.selectedText, 256);
        }
        else
        {
            editor["has_selection"] = false;
        }
        if (!snap.slotName.empty())
        {
            editor["slot_name"] = snap.slotName;
            editor["slot_index"] = snap.slotIndex;
        }
        ctxJson["editor"] = std::move(editor);
    }

    // --- region (bounded surrounding source) ---
    ctxJson["region"] = assembleRegion(req, snap);

    // --- diagnostics (proximity-ordered, bounded) ---
    ctxJson["diagnostics"] = assembleDiagnostics(req, snap, language);

    // --- J-4: classify authoring intent from cursor context + diagnostics ---
    // Intent is derived from the existing AI-G3 cursor classification and
    // the diagnostics just assembled — no second intent classifier.
    {
        const IntentKind intent = classifyIntent(ctx, ctxJson["diagnostics"]);
        ctxJson["intent"] = intentLabel(intent);
        ctxJson["intent_kind"] = [intent] {
            switch (intent) {
                case IntentKind::Continue:  return "continue";
                case IntentKind::Transform: return "transform";
                case IntentKind::Densify:   return "densify";
                case IntentKind::Repair:    return "repair";
                case IntentKind::General:   return "general";
            }
            return "general";
        }();
        out.intent = std::string(intentLabel(intent));
        out.intentKind = ctxJson["intent_kind"].get<std::string>();
    }

    // --- metadata (relevance-filtered, version-gated) ---
    ctxJson["metadata"] = assembleMetadata(req, language);

    // --- samples (bounded, relevance-filtered) ---
    ctxJson["samples"] = assembleSamples(req, ctx);

    // --- instruments (bounded) — always included (relevant for .ck and .hathor) ---
    ctxJson["instruments"] = assembleInstruments(req, ctx);

    // --- examples (version-compatible, context-keyed, bounded) ---
    ctxJson["examples"] = assembleExamples(req, ctx);

    // --- runtime (bpm, transport, slot) ---
    ctxJson["runtime"] = assembleRuntime(req, snap, language);

    // --- project (compact overview) ---
    ctxJson["project"] = assembleProject(req);

    // --- J-5: project retrieval (bounded, ranked snippets) ---
    ctxJson["project_retrieval"] = assembleProjectRetrieval(req, ctx, language);

    // J-4: Intent-aware instructions — the intent (continue / transform /
    // densify / repair) is derived from the AI-G3 cursor classification +
    // diagnostics above. There is exactly one intent-aware authoring path.
    ctxJson["instructions"] = [intent = out.intent] {
        const std::string base =
            "You are a Hathor inline-completion (FIM) assistant. Complete only the "
            "missing middle code at the cursor. Prefer Hathor-supported language "
            "constructs and valid sample/UGen names. Respect the surrounding "
            "prefix/suffix so your completion fits between them. Do not invent "
            "syntax the supported surface does not include.";
        if (intent == "continue")
            return base + " The author's intent is to continue: generate the next "
                          "natural token(s) at this boundary.";
        if (intent == "transform")
            return base + " The author's intent is to transform: extend or apply "
                          "the surrounding transformation construct.";
        if (intent == "densify")
            return base + " The author's intent is to densify: add more events or "
                          "density to the current pattern/sample expression.";
        if (intent == "repair")
            return base + " The author's intent is to repair: the diagnostics "
                          "near the cursor indicate an error — fix it or complete "
                          "around it.";
        return base + " The author's intent is general continuation.";
    }();

      // --- final size budget enforcement ---
      out.fimPrefix = ctxJson.dump();
      // Progressive trim: strip categories one at a time (examples, samples,
      // instruments, region surrounding) until the budget is met or no more
      // categories remain. Each category is trimmed at most once; if the
      // metadata block alone exceeds the budget the hard-truncate below
      // handles it.
      if (static_cast<int>(out.fimPrefix.size()) > bounds.maxContextChars)
      {
          if (ctxJson.contains("examples") &&
              ctxJson["examples"].contains("examples") &&
              !ctxJson["examples"]["examples"].empty())
          {
              ctxJson["examples"]["examples"] = nlohmann::json::array();
              ctxJson["examples"]["count"] = 0;
              out.fimPrefix = ctxJson.dump();
          }
          if (static_cast<int>(out.fimPrefix.size()) > bounds.maxContextChars &&
              ctxJson.contains("samples") &&
              ctxJson["samples"].contains("samples") &&
              !ctxJson["samples"]["samples"].empty())
          {
              ctxJson["samples"]["samples"] = nlohmann::json::array();
              out.fimPrefix = ctxJson.dump();
          }
          if (static_cast<int>(out.fimPrefix.size()) > bounds.maxContextChars &&
              ctxJson.contains("instruments") &&
              ctxJson["instruments"].contains("instruments") &&
              !ctxJson["instruments"]["instruments"].empty())
          {
              ctxJson["instruments"]["instruments"] = nlohmann::json::array();
              out.fimPrefix = ctxJson.dump();
          }
          if (static_cast<int>(out.fimPrefix.size()) > bounds.maxContextChars &&
              ctxJson.contains("region") &&
              ctxJson["region"].contains("surrounding") &&
              !ctxJson["region"]["surrounding"].empty())
          {
              ctxJson["region"]["surrounding"] = "";
              out.fimPrefix = ctxJson.dump();
          }
      }
      // If still over budget, hard-truncate the serialized blob so fim.prefix is
      // always bounded (never the whole repo, never unbounded).
      if (static_cast<int>(out.fimPrefix.size()) > bounds.maxContextChars)
      {
          out.fimPrefix = out.fimPrefix.substr(0, static_cast<std::size_t>(bounds.maxContextChars));
          // Re-parse so `context` and `fimPrefix` stay consistent.
          try { ctxJson = nlohmann::json::parse(out.fimPrefix); }
          catch (...) { ctxJson = nlohmann::json::object(); ctxJson["ok"] = false; ctxJson["error"] = "context truncated to fit budget"; }
      }

    out.context = std::move(ctxJson);
    return out;
}

// ---------------------------------------------------------------------------
// Bounded JSON helpers
// ---------------------------------------------------------------------------

nlohmann::json CompletionContextProvider::boundedNames(const std::vector<std::string>& names,
                                                     int maxEntries)
{
    nlohmann::json arr = nlohmann::json::array();
    int emitted = 0;
    for (const auto& n : names)
    {
        if (emitted >= maxEntries) break;
        arr.push_back(n);
        ++emitted;
    }
    return arr;
}

nlohmann::json CompletionContextProvider::truncateArray(const nlohmann::json& arr, int maxEntries)
{
    if (!arr.is_array()) return arr;
    nlohmann::json out = nlohmann::json::array();
    int emitted = 0;
    for (const auto& e : arr)
    {
        if (emitted >= maxEntries) break;
        out.push_back(e);
        ++emitted;
    }
    return out;
}

} // namespace hathor::control
