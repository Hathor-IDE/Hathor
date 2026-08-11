// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ProjectSymbolIndex.cpp — implementation of the lightweight, versioned,
 * text-based project symbol/file indexer (J-5).
 *
 * JUCE-free: uses only the standard library. Safe to call from any non-audio
 * thread. Indexing performs filesystem I/O + text parsing — it is never
 * invoked on the JUCE real-time audio callback thread (Requirement #10).
 *
 * Requirement references: J-5, AI-G2 (JUCE-free), threading requirement #10
 */

#include "hathor/ProjectSymbolIndex.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hathor::language {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// File extension → language label.
std::string languageFromExtension(const std::filesystem::path& p)
{
    const auto ext = p.extension().string();
    if (ext == ".hathor") return "mininotation";
    if (ext == ".ck")     return "chuck";
    return {};
}

/// File modification time in milliseconds since epoch (0 on error).
std::uint64_t fileMtimeMs(const std::filesystem::path& p) noexcept
{
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    const auto s = std::chrono::time_point_cast<std::chrono::milliseconds>(ft);
    return static_cast<std::uint64_t>(s.time_since_epoch().count());
}

/// Read entire file into a string. Returns empty on error.
std::string readFileToString(const std::filesystem::path& p, std::size_t maxLen = 0)
{
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs.is_open()) return {};
    std::stringstream ss;
    ss << ifs.rdbuf();
    if (maxLen > 0 && ss.str().size() > maxLen)
        return ss.str().substr(0, maxLen);
    return ss.str();
}

/// Trim leading/trailing whitespace from a string_view.
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

/// Extract a bounded snippet around the given 0-based line index.
std::string extractSnippet(std::string_view text, int lineIdx1Based, int maxChars)
{
    // Find the start of the target line (1-based).
    std::size_t i = 0;
    int current = 1;
    while (current < lineIdx1Based && i < text.size())
    {
        std::size_t nl = text.find('\n', i);
        if (nl == std::string_view::npos) return {};
        i = nl + 1;
        ++current;
    }
    if (current != lineIdx1Based) return {};

    // Find end of the target line.
    std::size_t nl = text.find('\n', i);
    std::size_t lineEnd = (nl == std::string_view::npos) ? text.size() : nl;
    std::string snippet = std::string(text.substr(i, lineEnd - i));
    if (static_cast<int>(snippet.size()) > maxChars)
        snippet = snippet.substr(0, static_cast<std::size_t>(maxChars - 3)) + "...";
    return snippet;
}

/// Read an identifier starting at pos, scanning backward.
std::string identifierBefore(std::string_view line, std::size_t pos)
{
    // First skip backward over any whitespace to find the end of the identifier.
    std::size_t end = pos;
    while (end > 0 && (line[end - 1] == ' ' || line[end - 1] == '\t'))
        --end;
    // Now scan backward for identifier characters.
    std::size_t i = end;
    while (i > 0 &&
           ((line[i - 1] >= 'a' && line[i - 1] <= 'z') ||
            (line[i - 1] >= 'A' && line[i - 1] <= 'Z') ||
            line[i - 1] == '_' ||
            (line[i - 1] >= '0' && line[i - 1] <= '9')))
        --i;
    if (i == end) return {};  // no identifier found (was only whitespace)
    return std::string(line.substr(i, end - i));
}

/// Read an identifier starting at pos, scanning forward.
std::string identifierAt(std::string_view line, std::size_t pos)
{
    std::size_t start = pos;
    // Skip backward to start of identifier.
    while (start > 0 &&
           ((line[start - 1] >= 'a' && line[start - 1] <= 'z') ||
            (line[start - 1] >= 'A' && line[start - 1] <= 'Z') ||
            line[start - 1] == '_' ||
            (line[start - 1] >= '0' && line[start - 1] <= '9')))
        --start;
    std::size_t end = start;
    while (end < line.size() &&
           ((line[end] >= 'a' && line[end] <= 'z') ||
            (line[end] >= 'A' && line[end] <= 'Z') ||
            line[end] == '_' ||
            (line[end] >= '0' && line[end] <= '9')))
        ++end;
    return (end > start) ? std::string(line.substr(start, end - start)) : std::string{};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ProjectSymbolIndex::ProjectSymbolIndex() = default;
ProjectSymbolIndex::~ProjectSymbolIndex() = default;

void ProjectSymbolIndex::setConfig(IndexConfig cfg) noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    config_ = std::move(cfg);
}

// ---------------------------------------------------------------------------
// Full reindex
// ---------------------------------------------------------------------------

void ProjectSymbolIndex::reindex(const std::filesystem::path& projectDir)
{
    IndexData newData;
    newData.projectDir = projectDir;

    std::error_code ec;
    if (!std::filesystem::exists(projectDir, ec) || !std::filesystem::is_directory(projectDir, ec))
    {
        // Clear state for an invalid/empty project dir.
        data_ = std::move(newData);
        data_.versionToken = "empty";
        publishSnapshot();
        return;
    }

    int filesScanned = 0;

    // Walk the project directory recursively, collecting .hathor and .ck files.
    std::vector<std::filesystem::path> candidates;
    std::unordered_map<std::string, std::uint64_t> newMtimes;

    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             projectDir, std::filesystem::directory_options::skip_permission_denied, ec))
    {
        if (ec) continue;
        if (!entry.is_regular_file()) continue;
        if (filesScanned >= config_.maxFiles) break;

        const auto& p = entry.path();
        const auto lang = languageFromExtension(p);
        if (lang.empty()) continue;

        const auto str = p.string();
        const auto mtime = fileMtimeMs(p);
        newMtimes[str] = mtime;
        candidates.push_back(p);
        ++filesScanned;
    }

    // Sort candidates by path for deterministic ordering.
    std::sort(candidates.begin(), candidates.end());

    for (const auto& p : candidates)
    {
        const auto lang = languageFromExtension(p);
        const auto mtime = newMtimes[p.string()];

        IndexedFile file;
        file.path = p.string();
        file.uri = "file://" + p.string();
        file.language = lang;
        file.mtimeMs = mtime;

        std::vector<IndexedSymbol> fileSymbols;
        indexFile(p, lang, mtime, fileSymbols, file.symbolCount);

        newData.symbols.insert(newData.symbols.end(),
                               std::make_move_iterator(fileSymbols.begin()),
                               std::make_move_iterator(fileSymbols.end()));

        // File preview — first N lines, bounded by chars.
        const auto content = readFileToString(p, static_cast<std::size_t>(config_.maxPreviewChars));
        int lineCount = 0;
        std::string preview;
        {
            std::size_t i = 0;
            while (i <= content.size() && lineCount < config_.maxPreviewLines)
            {
                std::size_t nl = content.find('\n', i);
                if (nl == std::string::npos)
                {
                    preview += content.substr(i);
                    break;
                }
                preview += content.substr(i, nl - i) + "\n";
                i = nl + 1;
                ++lineCount;
            }
        }
        if (static_cast<int>(preview.size()) > config_.maxPreviewChars)
            preview = preview.substr(0, static_cast<std::size_t>(config_.maxPreviewChars - 3)) + "...";
        file.preview = std::move(preview);

        newData.files.push_back(std::move(file));
    }

    // Build version token from sorted (path, mtime) pairs.
    std::string token;
    for (const auto& [path, mtime] : newMtimes)
    {
        token += path;
        token += ":";
        token += std::to_string(mtime);
        token += ";";
    }
    if (token.empty())
        token = "empty";
    else
        token = "v1:" + std::to_string(filesScanned) + ":" +
                std::to_string(newData.symbols.size()) + ":" +
                std::to_string(std::hash<std::string>{}(token));

    newData.mtimes = std::move(newMtimes);
    newData.versionToken = std::move(token);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        data_ = std::move(newData);
    }
    publishSnapshot();
}

// ---------------------------------------------------------------------------
// Lazy re-index
// ---------------------------------------------------------------------------

bool ProjectSymbolIndex::maybeReindex(const std::filesystem::path& projectDir)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto oldToken = data_.versionToken;

    if (oldToken == "empty" && data_.projectDir.empty())
    {
        // First-time indexing.
        data_.projectDir = projectDir;
        // We need to release the lock to call reindex, which acquires it again.
        lock.~lock_guard();
        reindex(projectDir);
        return true;
    }

    if (data_.projectDir != projectDir)
    {
        // Project dir changed — full reindex.
        lock.~lock_guard();
        reindex(projectDir);
        return true;
    }

    // Check each tracked file's mtime. If any changed, do a full reindex
    // (file sets may have been added/removed). This is the conservative
    // approach — cheaper than tracking added/deleted files per-request.
    bool changed = false;
    for (const auto& [path, mtime] : data_.mtimes)
    {
        const auto current = fileMtimeMs(path);
        if (current != mtime)
        {
            changed = true;
            break;
        }
    }

    if (!changed)
    {
        // Check for new/deleted files by counting again.
        std::error_code ec;
        int currentCount = 0;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 projectDir, std::filesystem::directory_options::skip_permission_denied, ec))
        {
            if (ec) continue;
            if (!entry.is_regular_file()) continue;
            const auto lang = languageFromExtension(entry.path());
            if (!lang.empty()) ++currentCount;
        }
        if (currentCount != static_cast<int>(data_.files.size()))
            changed = true;
    }

    if (changed)
    {
        lock.~lock_guard();
        reindex(projectDir);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------
// Clear
// ---------------------------------------------------------------------------

void ProjectSymbolIndex::clear() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    data_ = IndexData{};
    data_.versionToken = "empty";
    snapshot_ = data_;
}

// ---------------------------------------------------------------------------
// File indexing (extract symbols from one source file)
// ---------------------------------------------------------------------------

void ProjectSymbolIndex::indexFile(
    const std::filesystem::path& path,
    const std::string& language,
    std::uint64_t mtimeMs,
    std::vector<IndexedSymbol>& outSymbols,
    int& outSymbolCount)
{
    const auto content = readFileToString(path);
    if (content.empty()) return;

    outSymbolCount = 0;

    // Split content into lines (1-based for reporting).
    std::vector<std::string> lines;
    {
        std::size_t start = 0;
        while (start <= content.size())
        {
            std::size_t nl = content.find('\n', start);
            if (nl == std::string::npos)
            {
                lines.push_back(content.substr(start));
                break;
            }
            lines.push_back(content.substr(start, nl - start));
            start = nl + 1;
        }
    }

    if (language == "mininotation")
    {
        // Parse front matter for slot name.
        bool inFrontMatter = false;
        for (std::size_t li = 0; li < lines.size() && outSymbolCount < config_.maxSymbolsPerFile; ++li)
        {
            const auto& line = lines[li];
            const auto trimmed = trimSV(line);

            if (li == 0 && trimmed == "[hathor]")
            {
                inFrontMatter = true;
                continue;
            }

            if (inFrontMatter)
            {
                if (trimmed.empty())
                {
                    inFrontMatter = false;  // blank line ends front matter
                    continue;
                }
                // Look for "slot = d1"
                if (line.find("slot") != std::string::npos)
                {
                    const auto eq = line.find('=');
                    if (eq != std::string::npos)
                    {
                        const auto val = trimSV(line.substr(eq + 1));
                        if (!val.empty())
                        {
                            IndexedSymbol sym;
                            sym.name = std::string(val);
                            sym.filePath = path.string();
                            sym.uri = "file://" + path.string();
                            sym.line = static_cast<int>(li + 1);
                            sym.column = static_cast<int>(eq + 2);
                            sym.kind = SymbolKind::PatternSlot;
                            sym.language = "mininotation";
                            sym.fileMtimeMs = mtimeMs;
                            sym.snippet = extractSnippet(content, sym.line, config_.maxSnippetChars);
                            outSymbols.push_back(std::move(sym));
                            ++outSymbolCount;
                        }
                    }
                }
                continue;
            }

            // Not in front matter — scan the body.
            // Extract sample references: `s "name"` or `sound "name"`.
            {
                std::size_t pos = 0;
                while (pos < line.size())
                {
                    // Find "s " or "sound " followed by a quote.
                    std::size_t sPos = line.find_first_of("\"'", pos);
                    if (sPos == std::string::npos) break;

                    // Check what precedes the quote — must be s / sound / note / n.
                    std::string before = identifierBefore(line, sPos);
                    if (before == "s" || before == "sound" || before == "note" || before == "n")
                    {
                        std::size_t quoteEnd = line.find_first_of("\"'", sPos + 1);
                        if (quoteEnd != std::string::npos)
                        {
                            std::string sampleName = line.substr(sPos + 1, quoteEnd - sPos - 1);
                            sampleName = std::string(trimSV(sampleName));

                            // Handle space-separated sample lists inside the string.
                            std::stringstream ss(sampleName);
                            std::string token;
                            while (ss >> token)
                            {
                                if (outSymbolCount >= config_.maxSymbolsPerFile) break;
                                IndexedSymbol sym;
                                sym.name = token;
                                sym.filePath = path.string();
                                sym.uri = "file://" + path.string();
                                sym.line = static_cast<int>(li + 1);
                                sym.column = static_cast<int>(sPos + 1);
                                sym.kind = SymbolKind::SampleRef;
                                sym.language = "mininotation";
                                sym.fileMtimeMs = mtimeMs;
                                sym.snippet = extractSnippet(content, sym.line, config_.maxSnippetChars);
                                outSymbols.push_back(std::move(sym));
                                ++outSymbolCount;
                            }
                            pos = quoteEnd + 1;
                        }
                        else
                        {
                            pos = sPos + 1;
                        }
                    }
                    else
                    {
                        pos = sPos + 1;
                    }
                }
            }

            // Extract function calls: known transform functions.
            {
                std::size_t pos = 0;
                while (pos < line.size())
                {
                    std::string id = identifierAt(line, pos);
                    if (!id.empty())
                    {
                        if (outSymbolCount < config_.maxSymbolsPerFile)
                        {
                            // Tag known mini-notation functions.
                            static const std::unordered_set<std::string> kMiniFuncs = {
                                "s", "sound", "fast", "slow", "stut", "every", "sometimes",
                                "hurry", "density", "loopAt", "zoom", "when", "off",
                                "striate", "slice", "splice", "fit", "iter", "rev",
                                "palindrome", "shuffle", "scramble", "step", "whenmod",
                                "linger", "echo", "clamp", "wrap", "coarse", "shift",
                                "vowel", "shape", "jux", "juxby", "dist", "squeeze",
                                "contrast", "map", "fastRel", "slowRel", "gain", "pan",
                                "speed", "cutoff", "room", "delay", "orbit", "begin",
                                "end", "legato", "cut", "segment", "part",
                                "scale", "degree", "note", "n", "stack"
                            };
                            if (kMiniFuncs.count(id))
                            {
                                IndexedSymbol sym;
                                sym.name = id;
                                sym.filePath = path.string();
                                sym.uri = "file://" + path.string();
                                sym.line = static_cast<int>(li + 1);
                                sym.column = static_cast<int>(pos + 1);
                                sym.kind = SymbolKind::FunctionCall;
                                sym.language = "mininotation";
                                sym.fileMtimeMs = mtimeMs;
                                sym.snippet = extractSnippet(content, sym.line, config_.maxSnippetChars);
                                outSymbols.push_back(std::move(sym));
                                ++outSymbolCount;
                            }
                        }
                        pos += id.size();
                    }
                    else
                    {
                        ++pos;
                    }
                }
            }

            // Extract bare sample atoms (tokens that aren't functions, inside
            // pattern position — heuristics: words not preceded by `$` or `:`
            // and not inside quotes already captured above).
            {
                std::size_t pos = 0;
                while (pos < line.size())
                {
                    std::string id = identifierAt(line, pos);
                    if (!id.empty())
                    {
                        // Skip if it's a known function (already captured).
                        static const std::unordered_set<std::string> kSkip = {
                            "s", "sound", "fast", "slow", "stut", "every", "sometimes",
                            "hurry", "density", "loopAt", "zoom", "when", "off",
                            "striate", "slice", "splice", "fit", "iter", "rev",
                            "palindrome", "shuffle", "scramble", "step", "whenmod",
                            "linger", "echo", "clamp", "wrap", "coarse", "shift",
                            "vowel", "shape", "jux", "juxby", "dist", "squeeze",
                            "contrast", "map", "fastRel", "slowRel", "gain", "pan",
                            "speed", "cutoff", "room", "delay", "orbit", "begin",
                            "end", "legato", "cut", "segment", "part",
                            "scale", "degree", "note", "n", "stack"
                        };
                        if (!kSkip.count(id) && outSymbolCount < config_.maxSymbolsPerFile)
                        {
                            // Heuristic: bare atoms like "bd", "sn", "hh", "cp"
                            // — short lowercase identifiers (2-4 chars) that
                            // are likely sample names.
                            if (id.size() >= 2 && id.size() <= 8 &&
                                id.find_first_not_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos)
                            {
                                IndexedSymbol sym;
                                sym.name = id;
                                sym.filePath = path.string();
                                sym.uri = "file://" + path.string();
                                sym.line = static_cast<int>(li + 1);
                                sym.column = static_cast<int>(pos + 1);
                                sym.kind = SymbolKind::GenericSymbol;
                                sym.language = "mininotation";
                                sym.fileMtimeMs = mtimeMs;
                                sym.snippet = extractSnippet(content, sym.line, config_.maxSnippetChars);
                                outSymbols.push_back(std::move(sym));
                                ++outSymbolCount;
                            }
                        }
                        pos += id.size();
                    }
                    else
                    {
                        ++pos;
                    }
                }
            }
        }
    }
    else if (language == "chuck")
    {
        // Parse ChucK source for UGen instantiations, function defs,
        // class defs, `=>` routing, and `now` timing usage.
        for (std::size_t li = 0; li < lines.size() && outSymbolCount < config_.maxSymbolsPerFile; ++li)
        {
            const auto& line = lines[li];

            // Extract UGen / class instantiations: `ClassName name;` or `ClassName name =>`
            {
                std::size_t pos = 0;
                while (pos < line.size() && outSymbolCount < config_.maxSymbolsPerFile)
                {
                    std::string id = identifierAt(line, pos);
                    if (!id.empty())
                    {
                        // Check if this is followed by an identifier (the instance name).
                        std::size_t afterId = pos + id.size();
                        // Skip whitespace.
                        while (afterId < line.size() &&
                               (line[afterId] == ' ' || line[afterId] == '\t'))
                            ++afterId;

                        if (afterId < line.size())
                        {
                            std::string instanceName = identifierAt(line, afterId);
                            if (!instanceName.empty())
                            {
                                // Check for UGen-like names (capitalized or known UGens).
                                static const std::unordered_set<std::string> kUgenNames = {
                                    "SinOsc", "CosOsc", "SqrOsc", "SawOsc", "TriOsc",
                                    "PulseOsc", "Blit", "BlitSquare", "BlitSaw",
                                    "Noise", "Impulse", "Step", "Sequencer",
                                    "Gain", "Pan2", "Panned", "Delay", "DelayL", "DelayA",
                                    "Echo", "JCRev", "NRev", "FreeVerb", "PoleR",
                                    "LPF", "HPF", "BPF", "BRF", "BPF", "LPF12",
                                    "Envelope", "ADSR", "Attack", "Dur",
                                    "dac", "adc", " Noise", "Noise", "WhiteNoise", "PinkNoise",
                                    "SndBuf", "LiSa", "Granulator", "Wavetable",
                                    "LPF4", "HPF4", "BPF4", "BRF4",
                                    "OnePole", "BiQuad", "Butter", "ResonZ",
                                    "Chown", "SubNoise", "SubSqr", "SubSaw",
                                    "Filter", "LPF", "HPF", "BPF", "BRF"
                                };
                                bool isUgen = kUgenNames.count(id) ||
                                    (!id.empty() && id[0] >= 'A' && id[0] <= 'Z');

                                if (isUgen)
                                {
                                    IndexedSymbol sym;
                                    sym.name = id;
                                    sym.filePath = path.string();
                                    sym.uri = "file://" + path.string();
                                    sym.line = static_cast<int>(li + 1);
                                    sym.column = static_cast<int>(pos + 1);
                                    sym.kind = SymbolKind::UgenInstantiation;
                                    sym.language = "chuck";
                                    sym.fileMtimeMs = mtimeMs;
                                    sym.snippet = extractSnippet(content, sym.line, config_.maxSnippetChars);
                                    outSymbols.push_back(std::move(sym));
                                    ++outSymbolCount;
                                }
                            }
                        }
                        // Else: single-word token (e.g. "SinOsc;" or just a statement). Skip.
                        pos = afterId;
                    }
                    else
                    {
                        ++pos;
                    }
                }
            }

            // Extract `=>` routing: look for the arrow.
            {
                std::size_t arrow = line.find("=>");
                while (arrow != std::string::npos)
                {
                    if (outSymbolCount < config_.maxSymbolsPerFile)
                    {
                        // Get the identifier before => (the source UGen/variable).
                        std::string before = identifierBefore(line, arrow);
                        if (before.empty())
                        {
                            // Get the identifier after => (the destination).
                            before = identifierAt(line, arrow + 2);
                        }
                        if (!before.empty())
                        {
                            IndexedSymbol sym;
                            sym.name = before;
                            sym.filePath = path.string();
                            sym.uri = "file://" + path.string();
                            sym.line = static_cast<int>(li + 1);
                            sym.column = static_cast<int>(arrow + 1);
                            sym.kind = SymbolKind::UgenRouting;
                            sym.language = "chuck";
                            sym.fileMtimeMs = mtimeMs;
                            sym.snippet = extractSnippet(content, sym.line, config_.maxSnippetChars);
                            outSymbols.push_back(std::move(sym));
                            ++outSymbolCount;
                        }
                    }
                    arrow = line.find("=>", arrow + 2);
                }
            }

            // Extract `fun` function definitions.
            {
                std::size_t funPos = line.find("fun");
                while (funPos != std::string::npos)
                {
                    if (outSymbolCount < config_.maxSymbolsPerFile)
                    {
                        // Get the function name (token after the return type).
                        std::string name = identifierAt(line, funPos + 4);
                        if (!name.empty() && name != "fun")
                        {
                            // Check it's actually after "fun " (not part of a longer word)
                            if (funPos == 0 || !isalnum(static_cast<unsigned char>(line[funPos - 1])))
                            {
                                IndexedSymbol sym;
                                sym.name = name;
                                sym.filePath = path.string();
                                sym.uri = "file://" + path.string();
                                sym.line = static_cast<int>(li + 1);
                                sym.column = static_cast<int>(funPos + 1);
                                sym.kind = SymbolKind::ChuckFunction;
                                sym.language = "chuck";
                                sym.fileMtimeMs = mtimeMs;
                                sym.snippet = extractSnippet(content, sym.line, config_.maxSnippetChars);
                                outSymbols.push_back(std::move(sym));
                                ++outSymbolCount;
                            }
                        }
                    }
                    funPos = line.find("fun", funPos + 3);
                }
            }

            // Extract `class` definitions.
            {
                std::size_t classPos = line.find("class");
                while (classPos != std::string::npos)
                {
                    if (outSymbolCount < config_.maxSymbolsPerFile)
                    {
                        std::string name = identifierAt(line, classPos + 5);
                        if (!name.empty() && name != "class")
                        {
                            if (classPos == 0 || !isalnum(static_cast<unsigned char>(line[classPos - 1])))
                            {
                                IndexedSymbol sym;
                                sym.name = name;
                                sym.filePath = path.string();
                                sym.uri = "file://" + path.string();
                                sym.line = static_cast<int>(li + 1);
                                sym.column = static_cast<int>(classPos + 1);
                                sym.kind = SymbolKind::ChuckClass;
                                sym.language = "chuck";
                                sym.fileMtimeMs = mtimeMs;
                                sym.snippet = extractSnippet(content, sym.line, config_.maxSnippetChars);
                                outSymbols.push_back(std::move(sym));
                                ++outSymbolCount;
                            }
                        }
                    }
                    classPos = line.find("class", classPos + 5);
                }
            }

            // Extract `now` timing usage.
            {
                std::size_t nowPos = line.find("now");
                while (nowPos != std::string::npos)
                {
                    if (outSymbolCount < config_.maxSymbolsPerFile)
                    {
                        // Check it's a word boundary.
                        bool leftOk = (nowPos == 0 ||
                                       (!isalnum(static_cast<unsigned char>(line[nowPos - 1])) &&
                                        line[nowPos - 1] != '_'));
                        bool rightOk = (nowPos + 3 >= line.size() ||
                                        (!isalnum(static_cast<unsigned char>(line[nowPos + 3])) &&
                                         line[nowPos + 3] != '_'));
                        if (leftOk && rightOk)
                        {
                            IndexedSymbol sym;
                            sym.name = "now";
                            sym.filePath = path.string();
                            sym.uri = "file://" + path.string();
                            sym.line = static_cast<int>(li + 1);
                            sym.column = static_cast<int>(nowPos + 1);
                            sym.kind = SymbolKind::ChuckTiming;
                            sym.language = "chuck";
                            sym.fileMtimeMs = mtimeMs;
                            sym.snippet = extractSnippet(content, sym.line, config_.maxSnippetChars);
                            outSymbols.push_back(std::move(sym));
                            ++outSymbolCount;
                        }
                    }
                    nowPos = line.find("now", nowPos + 3);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Snapshot publishing
// ---------------------------------------------------------------------------

void ProjectSymbolIndex::publishSnapshot() noexcept
{
    snapshot_ = data_;
}

// ---------------------------------------------------------------------------
// Lookups (lock-free reads on snapshot_)
// ---------------------------------------------------------------------------

std::vector<IndexedSymbol> ProjectSymbolIndex::lookupSymbol(
    std::string_view name,
    std::string_view language) const
{
    // Take a quick snapshot under the lock — the snapshot is a copy-on-write
    // of the stable data.
    IndexData local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local = data_;
    }

    std::vector<IndexedSymbol> exact;
    std::vector<IndexedSymbol> prefix;

    for (const auto& sym : local.symbols)
    {
        if (!language.empty() && sym.language != language)
            continue;

        if (sym.name == name)
            exact.push_back(sym);
        else if (sym.name.size() >= name.size() &&
                 sym.name.compare(0, name.size(), name) == 0)
            prefix.push_back(sym);
    }

    // Sort each bucket by file modification recency (most recent first).
    auto cmp = [](const IndexedSymbol& a, const IndexedSymbol& b)
    {
        return a.fileMtimeMs > b.fileMtimeMs;
    };
    std::sort(exact.begin(), exact.end(), cmp);
    std::sort(prefix.begin(), prefix.end(), cmp);

    // Combine: exact first, then prefix — bounded.
    std::vector<IndexedSymbol> result;
    const int cap = config_.maxSymbolsPerFile;

    auto push = [&](const std::vector<IndexedSymbol>& bucket) {
        for (const auto& s : bucket)
        {
            if (static_cast<int>(result.size()) >= cap) return;
            result.push_back(s);
        }
    };
    push(exact);
    push(prefix);

    return result;
}

std::vector<IndexedSymbol> ProjectSymbolIndex::searchByPrefix(
    std::string_view prefix,
    std::string_view language) const
{
    IndexData local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local = data_;
    }

    std::vector<IndexedSymbol> result;
    for (const auto& sym : local.symbols)
    {
        if (!language.empty() && sym.language != language)
            continue;
        if (sym.name.rfind(prefix, 0) == 0)  // starts_with
        {
            result.push_back(sym);
            if (static_cast<int>(result.size()) >= config_.maxSymbolsPerFile) break;
        }
    }

    // Sort by prefix length (shortest first = most likely a match for the
    // exact name being typed), then by recency.
    std::sort(result.begin(), result.end(),
              [](const IndexedSymbol& a, const IndexedSymbol& b)
    {
        if (a.name.size() != b.name.size())
            return a.name.size() < b.name.size();
        return a.fileMtimeMs > b.fileMtimeMs;
    });

    return result;
}

std::vector<IndexedSymbol> ProjectSymbolIndex::searchByContent(
    std::string_view queryText,
    std::string_view language,
    int maxResults) const
{
    IndexData local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local = data_;
    }

    std::vector<IndexedSymbol> result;
    const std::string q(queryText);
    for (const auto& sym : local.symbols)
    {
        if (!language.empty() && sym.language != language)
            continue;
        if (sym.snippet.find(q) != std::string::npos)
        {
            result.push_back(sym);
            if (static_cast<int>(result.size()) >= maxResults) break;
        }
    }

    std::sort(result.begin(), result.end(),
              [](const IndexedSymbol& a, const IndexedSymbol& b)
    {
        return a.fileMtimeMs > b.fileMtimeMs;
    });

    return result;
}

std::vector<IndexedFile> ProjectSymbolIndex::listFiles(
    std::string_view language) const
{
    IndexData local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local = data_;
    }

    std::vector<IndexedFile> result;
    for (const auto& f : local.files)
    {
        if (!language.empty() && f.language != language)
            continue;
        result.push_back(f);
    }
    return result;
}

std::string ProjectSymbolIndex::versionToken() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.versionToken;
}

std::size_t ProjectSymbolIndex::symbolCount() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.symbols.size();
}

std::size_t ProjectSymbolIndex::fileCount() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.files.size();
}

bool ProjectSymbolIndex::empty() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return data_.symbols.empty() && data_.files.empty();
}

} // namespace hathor::language
