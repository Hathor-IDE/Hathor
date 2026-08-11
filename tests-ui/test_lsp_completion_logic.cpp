// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_lsp_completion_logic.cpp — unit tests for LSP completion merge logic
 * and metadata fallback.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

#include "LspCompletionLogic.hpp"
#include "LspDiagnosticsDisplay.hpp"
#include "LspProtocol.hpp"
#include "hathor/LanguageMetadata.hpp"

#include <string>

using namespace hathor;
using namespace hathor::ui;

// ===========================================================================
// Context analysis
// ===========================================================================

TEST_CASE("analyzeContext extracts prefix at end of line", "[lsp][completion]")
{
    std::string text = "bd sn fast";
    auto ctx = lsp::analyzeContext(text, 0, 9);

    REQUIRE(ctx.prefix == "fast");
}

TEST_CASE("analyzeContext detects cursor inside parens", "[lsp][completion]")
{
    std::string text = "fast(";
    auto ctx = lsp::analyzeContext(text, 0, 5);

    REQUIRE(ctx.insideParens);
    REQUIRE(ctx.functionName == "fast");
}

TEST_CASE("analyzeContext detects cursor inside string", "[lsp][completion]")
{
    std::string text = "s(\"bd\")";
    auto ctx = lsp::analyzeContext(text, 0, 4);

    REQUIRE(ctx.insideString);
}

TEST_CASE("analyzeContext handles multi-line document", "[lsp][completion]")
{
    std::string text = "bd sn\nfast ";
    auto ctx = lsp::analyzeContext(text, 1, 4);

    REQUIRE(ctx.prefix == "fast");
}

// ===========================================================================
// Prefix matching and filtering
// ===========================================================================

TEST_CASE("matchesPrefix is case-insensitive", "[lsp][completion]")
{
    REQUIRE(lsp::matchesPrefix("Fast", "fast"));
    REQUIRE(lsp::matchesPrefix("FA", "fa"));
    REQUIRE_FALSE(lsp::matchesPrefix("slow", "fast"));
}

TEST_CASE("filterByPrefix returns only matching items", "[lsp][completion]")
{
    std::vector<lsp::CompletionItem> items;

    lsp::CompletionItem fast;
    fast.label = "fast";
    items.push_back(fast);

    lsp::CompletionItem fa;
    fa.label = "fa";
    items.push_back(fa);

    lsp::CompletionItem slow;
    slow.label = "slow";
    items.push_back(slow);

    auto filtered = lsp::filterByPrefix(items, "fa");
    REQUIRE(filtered.size() == 2);
    REQUIRE(filtered[0].label == "fast");
    REQUIRE(filtered[1].label == "fa");
}

TEST_CASE("filterByPrefix returns empty for no match", "[lsp][completion]")
{
    std::vector<lsp::CompletionItem> items;

    lsp::CompletionItem fast;
    fast.label = "fast";
    items.push_back(fast);

    auto filtered = lsp::filterByPrefix(items, "xyz");
    REQUIRE(filtered.empty());
}

// ===========================================================================
// Sorting
// ===========================================================================

TEST_CASE("sortCompletionItems puts functions first", "[lsp][completion]")
{
    std::vector<lsp::CompletionItem> items;

    lsp::CompletionItem bd;
    bd.label = "bd";
    bd.kind = lsp::CompletionItemKind::Value;
    items.push_back(bd);

    lsp::CompletionItem fast;
    fast.label = "fast";
    fast.kind = lsp::CompletionItemKind::Function;
    items.push_back(fast);

    lsp::CompletionItem sn;
    sn.label = "sn";
    sn.kind = lsp::CompletionItemKind::Value;
    items.push_back(sn);

    lsp::CompletionItem slow;
    slow.label = "slow";
    slow.kind = lsp::CompletionItemKind::Function;
    items.push_back(slow);

    lsp::sortCompletionItems(items);

    // Functions should come before Values
    REQUIRE(items[0].kind.has_value());
    REQUIRE(items[0].kind.value() == lsp::CompletionItemKind::Function);
    REQUIRE(items[1].kind.value() == lsp::CompletionItemKind::Function);
    REQUIRE(items[2].kind.value() == lsp::CompletionItemKind::Value);
    REQUIRE(items[3].kind.value() == lsp::CompletionItemKind::Value);
}

// ===========================================================================
// Metadata fallback (L1)
// ===========================================================================

TEST_CASE("metadataFallback adds supported functions from metadata", "[lsp][completion]")
{
    language::LanguageMetadata metadata;
    metadata.schemaVersion = language::kSchemaVersion;
    metadata.hathorEngineCompat = std::string(language::kHathorEngineCompat);
    metadata.strudelMiniNotationCompat = std::string(language::kStrudelMiniNotationCompat);
    metadata.chuckLibVersion = std::string(language::kChuckLibVersion);
    metadata.chuckIntegrationSurface = std::string(language::kChuckIntegrationSurface);

    metadata.functions.push_back({
        "fast", "fast(multiplier: number)", "Speed up a pattern",
        "pattern", true, std::nullopt
    });

    language::MetadataCompatibility compat;
    compat.compatible = true;
    compat.schemaVersion = std::to_string(language::kSchemaVersion);
    compat.engineVersion = std::string(language::kHathorEngineCompat);

    auto ctx = lsp::analyzeContext("fast", 0, 4);

    auto candidates = lsp::metadataFallback(metadata, compat, ctx);
    REQUIRE_FALSE(candidates.empty());
    REQUIRE(candidates[0].label == "fast");
    REQUIRE(candidates[0].kind == lsp::CompletionItemKind::Function);
}

TEST_CASE("metadataFallback returns empty when incompatible", "[lsp][completion]")
{
    language::LanguageMetadata metadata;
    metadata.functions.push_back({
        "fast", "fast(n)", "Speed up", "pattern", true, std::nullopt
    });

    language::MetadataCompatibility compat;
    compat.compatible = false;
    compat.errors.push_back("Version mismatch");

    auto ctx = lsp::analyzeContext("fa", 0, 2);

    auto candidates = lsp::metadataFallback(metadata, compat, ctx);
    REQUIRE(candidates.empty());
}

TEST_CASE("metadataFallback does not include unsupported functions", "[lsp][completion]")
{
    language::LanguageMetadata metadata;
    metadata.functions.push_back({
        "rev", "rev()", "Reverse (not yet supported)", "pattern", false, std::nullopt
    });
    metadata.functions.push_back({
        "fast", "fast(n)", "Speed up", "pattern", true, std::nullopt
    });

    language::MetadataCompatibility compat;
    compat.compatible = true;

    auto ctx = lsp::analyzeContext("f", 0, 1);

    auto candidates = lsp::metadataFallback(metadata, compat, ctx);
    bool hasRev = false;
    bool hasFast = false;
    for (const auto& c : candidates)
    {
        if (c.label == "rev") hasRev = true;
        if (c.label == "fast") hasFast = true;
    }
    REQUIRE_FALSE(hasRev);
    REQUIRE(hasFast);
}

TEST_CASE("metadataFallback adds sample definitions", "[lsp][completion]")
{
    language::LanguageMetadata metadata;
    metadata.schemaVersion = language::kSchemaVersion;
    metadata.hathorEngineCompat = std::string(language::kHathorEngineCompat);
    metadata.strudelMiniNotationCompat = std::string(language::kStrudelMiniNotationCompat);
    metadata.chuckLibVersion = std::string(language::kChuckLibVersion);
    metadata.chuckIntegrationSurface = std::string(language::kChuckIntegrationSurface);

    metadata.samples.push_back({
        "bd", "Bass drum", "drum"
    });
    metadata.samples.push_back({
        "sn", "Snare drum", "drum"
    });

    language::MetadataCompatibility compat;
    compat.compatible = true;

    // Context: completing a sample name inside s("...") matching "bd"
    auto ctx = lsp::analyzeContext("s(\"bd\")", 0, 4);

    auto candidates = lsp::metadataFallback(metadata, compat, ctx);
    bool hasBd = false;
    bool hasSn = false;
    for (const auto& c : candidates)
    {
        if (c.label == "bd") hasBd = true;
        if (c.label == "sn") hasSn = true;
    }
    REQUIRE(hasBd);
    // "sn" should NOT be included because it doesn't match prefix "bd"
    REQUIRE_FALSE(hasSn);
}

// ===========================================================================
// Merge completion (L1)
// ===========================================================================

TEST_CASE("mergeCompletion prefers LSP items over metadata (dedup)", "[lsp][completion]")
{
    language::LanguageMetadata metadata;
    metadata.schemaVersion = language::kSchemaVersion;
    metadata.hathorEngineCompat = std::string(language::kHathorEngineCompat);
    metadata.strudelMiniNotationCompat = std::string(language::kStrudelMiniNotationCompat);
    metadata.chuckLibVersion = std::string(language::kChuckLibVersion);
    metadata.chuckIntegrationSurface = std::string(language::kChuckIntegrationSurface);

    metadata.functions.push_back({
        "fast", "fast(n)", "LSP doesn't know", "pattern", true, std::nullopt
    });

    language::MetadataCompatibility compat;
    compat.compatible = true;

    auto ctx = lsp::analyzeContext("fa", 0, 2);

    // LSP provides "fast"
    std::vector<lsp::CompletionItem> lspItems;
    lsp::CompletionItem item;
    item.label = "fast";
    item.kind = lsp::CompletionItemKind::Function;
    item.insertText = "fast";
    lspItems.push_back(item);

    auto result = lsp::mergeCompletion(lspItems, &metadata, &compat, ctx);

    REQUIRE(result.items.size() == 1);
    REQUIRE(result.items[0].label == "fast");
}

TEST_CASE("mergeCompletion adds metadata items not in LSP", "[lsp][completion]")
{
    language::LanguageMetadata metadata;
    metadata.schemaVersion = language::kSchemaVersion;
    metadata.hathorEngineCompat = std::string(language::kHathorEngineCompat);
    metadata.strudelMiniNotationCompat = std::string(language::kStrudelMiniNotationCompat);
    metadata.chuckLibVersion = std::string(language::kChuckLibVersion);
    metadata.chuckIntegrationSurface = std::string(language::kChuckIntegrationSurface);

    metadata.functions.push_back({
        "fast", "fast(n)", "LSP doesn't know", "pattern", true, std::nullopt
    });
    metadata.functions.push_back({
        "faster", "faster(n)", "Even faster", "pattern", true, std::nullopt
    });

    language::MetadataCompatibility compat;
    compat.compatible = true;

    auto ctx = lsp::analyzeContext("fa", 0, 2);

    // LSP provides "fast" but not "faster"
    std::vector<lsp::CompletionItem> lspItems;
    lsp::CompletionItem item;
    item.label = "fast";
    item.kind = lsp::CompletionItemKind::Function;
    item.insertText = "fast";
    lspItems.push_back(item);

    auto result = lsp::mergeCompletion(lspItems, &metadata, &compat, ctx);

    // Should have both: "fast" (from LSP) and "faster" (from metadata)
    REQUIRE(result.items.size() == 2);
    bool hasFast = false;
    bool hasFaster = false;
    for (const auto& c : result.items)
    {
        if (c.label == "fast") hasFast = true;
        if (c.label == "faster") hasFaster = true;
    }
    REQUIRE(hasFast);
    REQUIRE(hasFaster);
}

TEST_CASE("mergeCompletion works when LSP returns empty items", "[lsp][completion]")
{
    language::LanguageMetadata metadata;
    metadata.schemaVersion = language::kSchemaVersion;
    metadata.hathorEngineCompat = std::string(language::kHathorEngineCompat);
    metadata.strudelMiniNotationCompat = std::string(language::kStrudelMiniNotationCompat);
    metadata.chuckLibVersion = std::string(language::kChuckLibVersion);
    metadata.chuckIntegrationSurface = std::string(language::kChuckIntegrationSurface);

    metadata.functions.push_back({
        "fast", "fast(n)", "Speed up", "pattern", true, std::nullopt
    });

    language::MetadataCompatibility compat;
    compat.compatible = true;

    auto ctx = lsp::analyzeContext("fast", 0, 4);

    auto result = lsp::mergeCompletion({}, &metadata, &compat, ctx);

    REQUIRE(result.items.size() == 1);
    REQUIRE(result.items[0].label == "fast");
}

TEST_CASE("mergeCompletion works when metadata is nullptr", "[lsp][completion]")
{
    auto ctx = lsp::analyzeContext("fa", 0, 2);

    std::vector<lsp::CompletionItem> lspItems;
    lsp::CompletionItem item;
    item.label = "fast";
    item.kind = lsp::CompletionItemKind::Function;
    item.insertText = "fast";
    lspItems.push_back(item);

    auto result = lsp::mergeCompletion(lspItems, nullptr, nullptr, ctx);

    REQUIRE(result.items.size() == 1);
    REQUIRE(result.items[0].label == "fast");
}

TEST_CASE("mergeCompletion works when both LSP and metadata are empty", "[lsp][completion]")
{
    auto ctx = lsp::analyzeContext("xyz", 0, 3);

    auto result = lsp::mergeCompletion({}, nullptr, nullptr, ctx);

    REQUIRE(result.items.empty());
}

// ===========================================================================
// Diagnostics merging
// ===========================================================================

TEST_CASE("mergeDiagnostics adds unsupported function warnings", "[lsp][completion][diagnostics]")
{
    language::LanguageMetadata metadata;
    metadata.schemaVersion = language::kSchemaVersion;
    metadata.hathorEngineCompat = std::string(language::kHathorEngineCompat);
    metadata.strudelMiniNotationCompat = std::string(language::kStrudelMiniNotationCompat);
    metadata.chuckLibVersion = std::string(language::kChuckLibVersion);
    metadata.chuckIntegrationSurface = std::string(language::kChuckIntegrationSurface);

    metadata.functions.push_back({
        "rev", "rev()", "Reverse (unsupported)", "pattern", false, std::nullopt
    });
    metadata.functions.push_back({
        "fast", "fast(n)", "Supported", "pattern", true, std::nullopt
    });

    language::MetadataCompatibility compat;
    compat.compatible = true;

    std::string text = "fast(2) rev";
    auto diags = lsp::mergeDiagnostics({}, &metadata, &compat, text);

    // Should have a warning for "rev" being unsupported
    bool hasRevWarning = false;
    for (const auto& d : diags)
    {
        if (d.code.has_value() && d.code.value() == "UNSUPPORTED_FUNCTION")
            hasRevWarning = true;
    }
    REQUIRE(hasRevWarning);
}

TEST_CASE("mergeDiagnostics does not check when metadata incompatible", "[lsp][completion][diagnostics]")
{
    language::LanguageMetadata metadata;
    metadata.functions.push_back({
        "rev", "rev()", "Unsupported", "pattern", false, std::nullopt
    });

    language::MetadataCompatibility compat;
    compat.compatible = false;

    std::string text = "rev";
    auto diags = lsp::mergeDiagnostics({}, &metadata, &compat, text);

    REQUIRE(diags.empty());
}

TEST_CASE("mergeDiagnostics passes through LSP diagnostics", "[lsp][completion][diagnostics]")
{
    language::MetadataCompatibility compat;
    compat.compatible = false;

    std::vector<lsp::Diagnostic> lspDiags;
    lsp::Diagnostic d;
    d.message = "Parse error";
    d.severity = lsp::DiagnosticSeverity::Error;
    d.range = {{0, 0}, {0, 5}};
    lspDiags.push_back(d);

    std::string text = "hello";
    auto result = lsp::mergeDiagnostics(lspDiags, nullptr, nullptr, text);

    REQUIRE(result.size() == 1);
    REQUIRE(result[0].message == "Parse error");
    REQUIRE(result[0].severity.value() == lsp::DiagnosticSeverity::Error);
}

// ===========================================================================
// LspDiagnosticsDisplay tests
// ===========================================================================

TEST_CASE("LspDiagnosticsDisplay stores and retrieves diagnostics", "[lsp][diagnostics]")
{
    LspDiagnosticsDisplay display;

    std::vector<lsp::Diagnostic> diags;
    lsp::Diagnostic d;
    d.message = "Test error";
    d.severity = lsp::DiagnosticSeverity::Error;
    d.range = {{0, 0}, {0, 5}};
    d.source = "test";
    diags.push_back(d);

    display.setDiagnostics("file:///test.hathor", diags);

    REQUIRE(display.hasErrors("file:///test.hathor"));
    REQUIRE(display.errorCount("file:///test.hathor") == 1);
}

TEST_CASE("LspDiagnosticsDisplay returns empty for unknown URI", "[lsp][diagnostics]")
{
    LspDiagnosticsDisplay display;
    REQUIRE_FALSE(display.hasErrors("file:///unknown.hathor"));
    REQUIRE(display.errorCount("file:///unknown.hathor") == 0);
    REQUIRE(display.getAllDiagnostics("file:///unknown.hathor").empty());
}

TEST_CASE("LspDiagnosticsDisplay per-line filtering", "[lsp][diagnostics]")
{
    LspDiagnosticsDisplay display;

    std::vector<lsp::Diagnostic> diags;
    lsp::Diagnostic d1;
    d1.message = "Line 0 error";
    d1.severity = lsp::DiagnosticSeverity::Error;
    d1.range = {{0, 0}, {0, 3}};
    diags.push_back(d1);

    lsp::Diagnostic d2;
    d2.message = "Line 2 error";
    d2.severity = lsp::DiagnosticSeverity::Error;
    d2.range = {{2, 0}, {2, 3}};
    diags.push_back(d2);

    display.setDiagnostics("file:///test.hathor", diags);

    auto line0 = display.getDiagnosticsForLine("file:///test.hathor", 0);
    REQUIRE(line0.size() == 1);
    REQUIRE(line0[0].message == "Line 0 error");

    auto line1 = display.getDiagnosticsForLine("file:///test.hathor", 1);
    REQUIRE(line1.empty());

    auto line2 = display.getDiagnosticsForLine("file:///test.hathor", 2);
    REQUIRE(line2.size() == 1);
    REQUIRE(line2[0].message == "Line 2 error");
}

TEST_CASE("LspDiagnosticsDisplay clear operations", "[lsp][diagnostics]")
{
    LspDiagnosticsDisplay display;

    std::vector<lsp::Diagnostic> diags;
    lsp::Diagnostic d;
    d.severity = lsp::DiagnosticSeverity::Error;
    d.range = {{0, 0}, {0, 1}};
    diags.push_back(d);

    display.setDiagnostics("file:///a.hathor", diags);
    display.setDiagnostics("file:///b.hathor", diags);

    REQUIRE(display.errorCount("file:///a.hathor") == 1);
    REQUIRE(display.errorCount("file:///b.hathor") == 1);

    display.clearDiagnostics("file:///a.hathor");
    REQUIRE(display.errorCount("file:///a.hathor") == 0);
    REQUIRE(display.errorCount("file:///b.hathor") == 1);

    display.clearAll();
    REQUIRE(display.errorCount("file:///b.hathor") == 0);
}

TEST_CASE("LspDiagnosticsDisplay summary string", "[lsp][diagnostics]")
{
    LspDiagnosticsDisplay display;

    std::vector<lsp::Diagnostic> diags;

    lsp::Diagnostic e1;
    e1.severity = lsp::DiagnosticSeverity::Error;
    e1.range = {{0, 0}, {0, 1}};
    diags.push_back(e1);

    lsp::Diagnostic e2;
    e2.severity = lsp::DiagnosticSeverity::Error;
    e2.range = {{1, 0}, {1, 1}};
    diags.push_back(e2);

    lsp::Diagnostic w1;
    w1.severity = lsp::DiagnosticSeverity::Warning;
    w1.range = {{2, 0}, {2, 1}};
    diags.push_back(w1);

    display.setDiagnostics("file:///test.hathor", diags);

    std::string s = display.summary("file:///test.hathor");
    REQUIRE(s.find("2 errors") != std::string::npos);
    REQUIRE(s.find("1 warning") != std::string::npos);
}

// ===========================================================================
// AI-G7: ChucK deterministic completion (no LSP server exists)
// ===========================================================================

namespace {
    language::LanguageMetadata makeChuckTestMetadata()
    {
        language::LanguageMetadata metadata;
        metadata.schemaVersion = language::kSchemaVersion;
        metadata.hathorEngineCompat = std::string(language::kHathorEngineCompat);
        metadata.strudelMiniNotationCompat = std::string(language::kStrudelMiniNotationCompat);
        metadata.chuckLibVersion = std::string(language::kChuckLibVersion);
        metadata.chuckIntegrationSurface = std::string(language::kChuckIntegrationSurface);

        metadata.chuckApi.push_back({
            "SinOsc", "ugen", "SinOsc osc => dac",
            "Sinusoidal oscillator UGen", true,
            "SinOsc s => dac"
        });
        metadata.chuckApi.push_back({
            "dac", "constant", "SinOsc s => dac",
            "Digital-to-analog converter output", true,
            std::nullopt
        });
        metadata.chuckApi.push_back({
            "Phasor", "ugen", "Phasor p => dac",
            "Sawtooth-wave phasor UGen", true,
            std::nullopt
        });
        metadata.chuckApi.push_back({
            "Shakers", "ugen", "Shakers s => dac",
            "STK Shakers instrument", false,
            std::nullopt
        });
        metadata.chuckApi.push_back({
            "Machine", "library", "Machine.add(\"foo.ck\")",
            "Runtime library for machine management", true,
            std::nullopt
        });

        return metadata;
    }
}

TEST_CASE("chuckMetadataFallback returns supported UGens matching prefix",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    auto ctx = lsp::analyzeContext("Sin", 0, 3);

    auto candidates = lsp::chuckMetadataFallback(metadata, compat, ctx);

    bool hasSinOsc = false;
    bool hasPhasor = false;
    bool hasShakers = false;
    for (const auto& c : candidates)
    {
        if (c.label == "SinOsc")     hasSinOsc = true;
        if (c.label == "Phasor")     hasPhasor = true;
        if (c.label == "Shakers")    hasShakers = true;
    }

    // SinOsc matches prefix "Sin" and is supported.
    REQUIRE(hasSinOsc);
    // Phasor does NOT match prefix "Sin".
    REQUIRE_FALSE(hasPhasor);
    // Shakers is unsupported — should NOT appear even if prefix matched.
    REQUIRE_FALSE(hasShakers);
}

TEST_CASE("chuckMetadataFallback excludes unsupported APIs",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    auto ctx = lsp::analyzeContext("Sha", 0, 3);

    auto candidates = lsp::chuckMetadataFallback(metadata, compat, ctx);

    // Shakers is in metadata but unsupported — must not appear.
    bool hasShakers = false;
    for (const auto& c : candidates)
    {
        if (c.label == "Shakers") hasShakers = true;
    }
    REQUIRE_FALSE(hasShakers);
}

TEST_CASE("chuckMetadataFallback includes built-in keywords",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    auto ctx = lsp::analyzeContext("wh", 0, 2);

    auto candidates = lsp::chuckMetadataFallback(metadata, compat, ctx);

    bool hasWhile = false;
    for (const auto& c : candidates)
    {
        if (c.label == "while") hasWhile = true;
    }
    REQUIRE(hasWhile);
}

TEST_CASE("chuckMetadataFallback returns empty when metadata incompatible",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = false;

    auto ctx = lsp::analyzeContext("", 0, 0);

    auto candidates = lsp::chuckMetadataFallback(metadata, compat, ctx);
    REQUIRE(candidates.empty());
}

TEST_CASE("chuckMetadataFallback returns all supported APIs when prefix empty",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    // Empty prefix matches everything.
    auto ctx = lsp::analyzeContext("", 0, 0);

    auto candidates = lsp::chuckMetadataFallback(metadata, compat, ctx);

    // Should include SinOsc, dac, Phasor, Machine (all supported metadata APIs)
    // plus built-in keywords, types, constants, UGens, libraries.
    bool hasSinOsc = false;
    bool hasDac = false;
    bool hasPhasor = false;
    bool hasMachine = false;
    for (const auto& c : candidates)
    {
        if (c.label == "SinOsc")     hasSinOsc = true;
        if (c.label == "dac")        hasDac = true;
        if (c.label == "Phasor")     hasPhasor = true;
        if (c.label == "Machine")    hasMachine = true;
    }
    REQUIRE(hasSinOsc);
    REQUIRE(hasDac);
    REQUIRE(hasPhasor);
    REQUIRE(hasMachine);
}

TEST_CASE("chuckMetadataFallback dedups metadata APIs with built-in UGens",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    // SinOsc is both in metadata.chuckApi and in chuckUgenSet().
    auto ctx = lsp::analyzeContext("Sin", 0, 3);

    auto candidates = lsp::chuckMetadataFallback(metadata, compat, ctx);

    // Should appear only once.
    int count = 0;
    for (const auto& c : candidates)
    {
        if (c.label == "SinOsc")
            ++count;
    }
    REQUIRE(count == 1);
}

// ===========================================================================
// AI-G7: ChucK diagnostics
// ===========================================================================

TEST_CASE("chuckDiagnostics emits error diagnostic from compiler",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    lsp::ChuckCompileDiagnostic cd;
    cd.ok = false;
    cd.errorLine = 3;
    cd.errorColumn = 10;
    cd.message = "unexpected token '}'";

    auto diags = lsp::chuckDiagnostics(cd, &metadata, &compat, "SinOsc s => dac\n");

    REQUIRE(diags.size() == 1);
    REQUIRE(diags[0].severity == lsp::DiagnosticSeverity::Error);
    REQUIRE(diags[0].code == "CK_COMPILE_ERROR");
    REQUIRE(diags[0].source == "chuck_compiler");
    REQUIRE(diags[0].message == "unexpected token '}'");
    // libchuck uses 1-based line/column; LSP uses 0-based.
    REQUIRE(diags[0].range.start.line == 2);
    REQUIRE(diags[0].range.start.character == 9);
}

TEST_CASE("chuckDiagnostics adds warning for unsupported ChucK API usage",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    // Compiler says OK (no error), but the source references Shakers (unsupported).
    lsp::ChuckCompileDiagnostic cd;
    cd.ok = true;
    cd.errorLine = 0;
    cd.errorColumn = 0;
    cd.message = "";

    std::string source = "Shakers s => dac\n";

    auto diags = lsp::chuckDiagnostics(cd, &metadata, &compat, source);

    // Should have at least one warning about Shakers being unsupported.
    bool hasWarning = false;
    for (const auto& d : diags)
    {
        if (d.severity == lsp::DiagnosticSeverity::Warning &&
            d.code == "UNSUPPORTED_CHUCK_API" &&
            d.message.find("Shakers") != std::string::npos)
        {
            hasWarning = true;
        }
    }
    REQUIRE(hasWarning);
}

TEST_CASE("chuckDiagnostics does not warn for supported ChucK API usage",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    lsp::ChuckCompileDiagnostic cd;
    cd.ok = true;
    cd.errorLine = 0;
    cd.errorColumn = 0;
    cd.message = "";

    std::string source = "SinOsc s => dac\n";

    auto diags = lsp::chuckDiagnostics(cd, &metadata, &compat, source);

    // No warnings — SinOsc and dac are both supported.
    for (const auto& d : diags)
    {
        INFO("Unexpected diagnostic: " << d.message);
        REQUIRE_FALSE(d.severity == lsp::DiagnosticSeverity::Warning);
    }
}

TEST_CASE("chuckDiagnostics skips metadata checks when incompatible",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = false;

    // Even though source references an unsupported API, diagnostics should
    // only contain compiler results (no metadata-aware warnings).
    lsp::ChuckCompileDiagnostic cd;
    cd.ok = true;
    cd.errorLine = 0;
    cd.errorColumn = 0;
    cd.message = "";

    std::string source = "Shakers s => dac\n";

    auto diags = lsp::chuckDiagnostics(cd, &metadata, &compat, source);

    REQUIRE(diags.empty());
}

TEST_CASE("chuckDiagnostics handles multi-line source for unsupported APIs",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    lsp::ChuckCompileDiagnostic cd;
    cd.ok = true;
    cd.errorLine = 0;
    cd.errorColumn = 0;
    cd.message = "";

    std::string source = "SinOsc osc => dac\nShakers sh => Pan2 p => dac\n";

    auto diags = lsp::chuckDiagnostics(cd, &metadata, &compat, source);

    // Shakers on line 1 should trigger a warning with line=1.
    bool hasWarning = false;
    for (const auto& d : diags)
    {
        if (d.severity == lsp::DiagnosticSeverity::Warning)
        {
            REQUIRE(d.code == "UNSUPPORTED_CHUCK_API");
            REQUIRE(d.range.start.line == 1);
            hasWarning = true;
        }
    }
    REQUIRE(hasWarning);
}

TEST_CASE("MetadataCompatibility defaults to compatible=false",
          "[lsp][chuck][ai-g7]")
{
    language::MetadataCompatibility compat;
    REQUIRE_FALSE(compat.compatible);
    REQUIRE_FALSE(compat); // operator bool()
}

// ===========================================================================
// AI-G7: Metadata version block for ChucK
// ===========================================================================

TEST_CASE("chuckMetadataFallback does not duplicate library classes",
          "[lsp][chuck][ai-g7]")
{
    auto metadata = makeChuckTestMetadata();
    language::MetadataCompatibility compat;
    compat.compatible = true;

    // "Machine" is in metadata.chuckApi (supported, library kind) and also
    // in chuckLibrarySet().
    auto ctx = lsp::analyzeContext("Mac", 0, 3);

    auto candidates = lsp::chuckMetadataFallback(metadata, compat, ctx);

    int count = 0;
    for (const auto& c : candidates)
    {
        if (c.label == "Machine")
            ++count;
    }
    REQUIRE(count == 1);
}
