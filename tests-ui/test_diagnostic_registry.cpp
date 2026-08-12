// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_diagnostic_registry.cpp — unit tests for DiagnosticRegistry.
 *
 * JUCE-free tests compiled into the hathor-ui-tests target.
 *
 * Requirement references: L-3 §2, §3
 */

#include <catch2/catch_test_macros.hpp>

#include "Diagnostic.hpp"
#include "DiagnosticRegistry.hpp"

using namespace hathor::control;

// ===========================================================================
// Diagnostic model — static helpers
// ===========================================================================

TEST_CASE("sourceLabel returns human-readable names", "[diagnostic]")
{
    REQUIRE(sourceLabel(DiagSource::StrudelLsp) == "Strudel LSP");
    REQUIRE(sourceLabel(DiagSource::ChuckCompiler) == "ChucK Compiler");
    REQUIRE(sourceLabel(DiagSource::HathorValidation) == "Hathor Validation");
    REQUIRE(sourceLabel(DiagSource::BuildSystem) == "Build System");
    REQUIRE(sourceLabel(DiagSource::TaskTestFailure) == "Task / Test");
    REQUIRE(sourceLabel(DiagSource::ChuckWorker) == "ChucK Worker");
    REQUIRE(sourceLabel(DiagSource::Runtime) == "Runtime");
}

TEST_CASE("severityLabel returns correct labels", "[diagnostic]")
{
    REQUIRE(severityLabel(DiagSeverity::Error) == "Error");
    REQUIRE(severityLabel(DiagSeverity::Warning) == "Warning");
    REQUIRE(severityLabel(DiagSeverity::Info) == "Info");
    REQUIRE(severityLabel(DiagSeverity::Hint) == "Hint");
}

TEST_CASE("isLanguageDiagnostic identifies LSP + ChucK", "[diagnostic]")
{
    REQUIRE(isLanguageDiagnostic(DiagSource::StrudelLsp) == true);
    REQUIRE(isLanguageDiagnostic(DiagSource::ChuckCompiler) == true);
    REQUIRE(isLanguageDiagnostic(DiagSource::HathorValidation) == false);
    REQUIRE(isLanguageDiagnostic(DiagSource::BuildSystem) == false);
    REQUIRE(isLanguageDiagnostic(DiagSource::Runtime) == false);
}

TEST_CASE("parseSeverity round-trips", "[diagnostic]")
{
    DiagSeverity out;
    REQUIRE((parseSeverity("Error", out) && out == DiagSeverity::Error));
    REQUIRE((parseSeverity("Warning", out) && out == DiagSeverity::Warning));
    REQUIRE((parseSeverity("Info", out) && out == DiagSeverity::Info));
    REQUIRE((parseSeverity("Hint", out) && out == DiagSeverity::Hint));
    REQUIRE_FALSE(parseSeverity("unknown", out));
}

TEST_CASE("parseSource round-trips", "[diagnostic]")
{
    DiagSource out;
    REQUIRE((parseSource("Strudel LSP", out) && out == DiagSource::StrudelLsp));
    REQUIRE((parseSource("ChucK Compiler", out) && out == DiagSource::ChuckCompiler));
    REQUIRE((parseSource("Hathor Validation", out) && out == DiagSource::HathorValidation));
    REQUIRE((parseSource("Build System", out) && out == DiagSource::BuildSystem));
    REQUIRE((parseSource("Task / Test", out) && out == DiagSource::TaskTestFailure));
    REQUIRE((parseSource("ChucK Worker", out) && out == DiagSource::ChuckWorker));
    REQUIRE((parseSource("Runtime", out) && out == DiagSource::Runtime));
    REQUIRE_FALSE(parseSource("bogus", out));
}

// ===========================================================================
// DiagnosticRegistry — basic set / clear
// ===========================================================================

TEST_CASE("DiagnosticRegistry: starts empty", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;
    REQUIRE(registry.total() == 0);
    REQUIRE(registry.errorCount() == 0);
    REQUIRE(registry.warningCount() == 0);
    REQUIRE(registry.allDiagnostics().empty());
    REQUIRE(registry.urisWithDiagnostics().empty());
}

TEST_CASE("DiagnosticRegistry: setDiagnostics stores and counts", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> diags;
    diags.push_back({0, DiagSeverity::Error, DiagSource::StrudelLsp, "Strudel LSP",
                     "E001", "Undefined variable", "file:///test.hathor", 10, 5, ""});
    diags.push_back({0, DiagSeverity::Warning, DiagSource::StrudelLsp, "Strudel LSP",
                     "W001", "Unused variable", "file:///test.hathor", 12, 3, ""});

    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///test.hathor", diags);

    REQUIRE(registry.total() == 2);
    REQUIRE(registry.errorCount() == 1);
    REQUIRE(registry.warningCount() == 1);
    REQUIRE(registry.urisWithDiagnostics().size() == 1);
    REQUIRE(registry.urisWithDiagnostics()[0] == "file:///test.hathor");
}

TEST_CASE("DiagnosticRegistry: setDiagnostics replaces existing (no duplicates)", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> first;
    first.push_back({0, DiagSeverity::Error, DiagSource::StrudelLsp, "Strudel LSP",
                     "E001", "First error", "file:///test.hathor", 1, 1, ""});
    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///test.hathor", first);

    std::vector<Diagnostic> second;
    second.push_back({0, DiagSeverity::Warning, DiagSource::StrudelLsp, "Strudel LSP",
                      "W001", "Second error", "file:///test.hathor", 5, 3, ""});
    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///test.hathor", second);

    // Must be exactly 1 — the replace cleared the old one.
    REQUIRE(registry.total() == 1);
    auto all = registry.allDiagnostics();
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].message == "Second error");
}

TEST_CASE("DiagnosticRegistry: setDiagnostics with empty vector clears", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> diags;
    diags.push_back({0, DiagSeverity::Error, DiagSource::ChuckCompiler, "ChucK Compiler",
                     "CK001", "Syntax error", "file:///test.ck", 1, 1, ""});
    registry.setDiagnostics(DiagSource::ChuckCompiler, "file:///test.ck", diags);
    REQUIRE(registry.total() == 1);

    registry.setDiagnostics(DiagSource::ChuckCompiler, "file:///test.ck", {});
    REQUIRE(registry.total() == 0);
    REQUIRE(registry.urisWithDiagnostics().empty());
}

TEST_CASE("DiagnosticRegistry: clearDiagnostics clears one source+uri", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> lspDiags;
    lspDiags.push_back({0, DiagSeverity::Error, DiagSource::StrudelLsp, "Strudel LSP",
                        "E001", "LSP error", "file:///a.hathor", 1, 1, ""});
    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///a.hathor", lspDiags);

    std::vector<Diagnostic> ckDiags;
    ckDiags.push_back({0, DiagSeverity::Error, DiagSource::ChuckCompiler, "ChucK Compiler",
                       "CK001", "CK error", "file:///a.ck", 1, 1, ""});
    registry.setDiagnostics(DiagSource::ChuckCompiler, "file:///a.ck", ckDiags);

    REQUIRE(registry.total() == 2);

    registry.clearDiagnostics(DiagSource::StrudelLsp, "file:///a.hathor");
    REQUIRE(registry.total() == 1);
    auto all = registry.allDiagnostics();
    REQUIRE(all.size() == 1);
    REQUIRE(all[0].source == DiagSource::ChuckCompiler);
}

TEST_CASE("DiagnosticRegistry: clearSource clears all URIs for a source", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> diags;
    diags.push_back({0, DiagSeverity::Error, DiagSource::StrudelLsp, "Strudel LSP",
                     "E001", "error 1", "file:///a.hathor", 1, 1, ""});
    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///a.hathor", diags);
    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///b.hathor", diags);

    REQUIRE(registry.total() == 2);
    registry.clearSource(DiagSource::StrudelLsp);
    REQUIRE(registry.total() == 0);
}

TEST_CASE("DiagnosticRegistry: clearAll wipes everything", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> lspDiags;
    lspDiags.push_back({0, DiagSeverity::Error, DiagSource::StrudelLsp, "Strudel LSP",
                        "E001", "LSP error", "file:///a.hathor", 1, 1, ""});
    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///a.hathor", lspDiags);

    std::vector<Diagnostic> ckDiags;
    ckDiags.push_back({0, DiagSeverity::Error, DiagSource::ChuckCompiler, "ChucK Compiler",
                       "CK001", "CK error", "file:///a.ck", 1, 1, ""});
    registry.setDiagnostics(DiagSource::ChuckCompiler, "file:///a.ck", ckDiags);

    registry.clearAll();
    REQUIRE(registry.total() == 0);
}

// ===========================================================================
// DiagnosticRegistry — addDiagnostic
// ===========================================================================

TEST_CASE("DiagnosticRegistry: addDiagnostic appends with stable ID", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    Diagnostic d1;
    d1.severity = DiagSeverity::Error;
    d1.source = DiagSource::ChuckWorker;
    d1.sourceLabel = "ChucK Worker";
    d1.message = "Worker crashed";
    d1.uri = "file:///test.ck";
    d1.line = 5;
    d1.column = 2;
    registry.addDiagnostic(d1);

    Diagnostic d2;
    d2.severity = DiagSeverity::Warning;
    d2.source = DiagSource::Runtime;
    d2.sourceLabel = "Runtime";
    d2.message = "Buffer underrun";
    d2.uri = "file:///test.ck";
    d2.line = 10;
    d2.column = 1;
    registry.addDiagnostic(d2);

    REQUIRE(registry.total() == 2);
    REQUIRE(registry.errorCount() == 1);
    REQUIRE(registry.warningCount() == 1);

    auto all = registry.allDiagnostics();
    REQUIRE(all.size() == 2);
    REQUIRE(all[0].id == 1);
    REQUIRE(all[1].id == 2);
}

// ===========================================================================
// DiagnosticRegistry — queries
// ===========================================================================

TEST_CASE("DiagnosticRegistry: diagnosticsForUri returns matching diagnostics", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> diagsA;
    diagsA.push_back({0, DiagSeverity::Error, DiagSource::StrudelLsp, "Strudel LSP",
                      "E001", "error in A", "file:///a.hathor", 1, 1, ""});
    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///a.hathor", diagsA);

    std::vector<Diagnostic> diagsB;
    diagsB.push_back({0, DiagSeverity::Warning, DiagSource::ChuckCompiler, "ChucK Compiler",
                      "CK001", "warning in B", "file:///b.ck", 5, 3, ""});
    registry.setDiagnostics(DiagSource::ChuckCompiler, "file:///b.ck", diagsB);

    auto aDiags = registry.diagnosticsForUri("file:///a.hathor");
    REQUIRE(aDiags.size() == 1);
    REQUIRE(aDiags[0].message == "error in A");

    auto bDiags = registry.diagnosticsForUri("file:///b.ck");
    REQUIRE(bDiags.size() == 1);
    REQUIRE(bDiags[0].message == "warning in B");

    auto cDiags = registry.diagnosticsForUri("file:///c.hathor");
    REQUIRE(cDiags.empty());
}

TEST_CASE("DiagnosticRegistry: diagnosticsForSourceUri returns matching", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> diags;
    diags.push_back({0, DiagSeverity::Error, DiagSource::StrudelLsp, "Strudel LSP",
                     "E001", "error 1", "file:///a.hathor", 1, 1, ""});
    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///a.hathor", diags);
    registry.setDiagnostics(DiagSource::ChuckCompiler, "file:///a.hathor", diags);

    auto lspOnly = registry.diagnosticsForSourceUri(DiagSource::StrudelLsp, "file:///a.hathor");
    REQUIRE(lspOnly.size() == 1);

    auto ckOnly = registry.diagnosticsForSourceUri(DiagSource::ChuckCompiler, "file:///a.hathor");
    REQUIRE(ckOnly.size() == 1);

    auto none = registry.diagnosticsForSourceUri(DiagSource::Runtime, "file:///a.hathor");
    REQUIRE(none.empty());
}

TEST_CASE("DiagnosticRegistry: counts reflect severity distribution", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> diags;
    diags.push_back({0, DiagSeverity::Error, DiagSource::Runtime, "Runtime", "R001", "err", "f://a", 1, 1, ""});
    diags.push_back({0, DiagSeverity::Error, DiagSource::Runtime, "Runtime", "R002", "err", "f://a", 2, 1, ""});
    diags.push_back({0, DiagSeverity::Warning, DiagSource::Runtime, "Runtime", "R003", "warn", "f://a", 3, 1, ""});
    diags.push_back({0, DiagSeverity::Info, DiagSource::Runtime, "Runtime", "R004", "info", "f://a", 4, 1, ""});
    diags.push_back({0, DiagSeverity::Hint, DiagSource::Runtime, "Runtime", "R005", "hint", "f://a", 5, 1, ""});

    registry.setDiagnostics(DiagSource::Runtime, "f://a", diags);

    auto counts = registry.counts();
    REQUIRE(counts.errors == 2);
    REQUIRE(counts.warnings == 1);
    REQUIRE(counts.info == 2);  // Info + Hint
    REQUIRE(counts.total == 5);
}

// ===========================================================================
// DiagnosticRegistry — change callback
// ===========================================================================

TEST_CASE("DiagnosticRegistry: change callback fires on set", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    int callbackCount = 0;
    registry.setChangeCallback([&callbackCount]() { callbackCount++; });

    std::vector<Diagnostic> diags;
    diags.push_back({0, DiagSeverity::Error, DiagSource::Runtime, "Runtime", "R001", "err", "f://a", 1, 1, ""});
    registry.setDiagnostics(DiagSource::Runtime, "f://a", diags);

    REQUIRE(callbackCount == 1);

    registry.clearAll();
    REQUIRE(callbackCount == 2);
}

TEST_CASE("DiagnosticRegistry: change callback fires on add", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    int callbackCount = 0;
    registry.setChangeCallback([&callbackCount]() { callbackCount++; });

    Diagnostic d;
    d.severity = DiagSeverity::Info;
    d.source = DiagSource::Runtime;
    d.message = "test";
    registry.addDiagnostic(d);

    REQUIRE(callbackCount == 1);
}

TEST_CASE("DiagnosticRegistry: resetIds resets the ID counter", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    Diagnostic d;
    d.severity = DiagSeverity::Info;
    d.source = DiagSource::Runtime;
    d.message = "test1";
    registry.addDiagnostic(d);
    registry.addDiagnostic(d);

    auto all1 = registry.allDiagnostics();
    REQUIRE(all1.size() == 2);
    REQUIRE(all1[0].id == 1);
    REQUIRE(all1[1].id == 2);

    registry.resetIds();
    registry.clearAll();

    registry.addDiagnostic(d);
    auto all2 = registry.allDiagnostics();
    REQUIRE(all2.size() == 1);
    REQUIRE(all2[0].id == 1);
}

// ===========================================================================
// DiagnosticRegistry — multi-source mixing
// ===========================================================================

TEST_CASE("DiagnosticRegistry: multiple sources coexist", "[diagnostic-registry]")
{
    DiagnosticRegistry registry;

    std::vector<Diagnostic> lspDiags;
    lspDiags.push_back({0, DiagSeverity::Error, DiagSource::StrudelLsp, "Strudel LSP",
                        "E001", "LSP error", "file:///test.hathor", 1, 1, ""});
    registry.setDiagnostics(DiagSource::StrudelLsp, "file:///test.hathor", lspDiags);

    std::vector<Diagnostic> ckDiags;
    ckDiags.push_back({0, DiagSeverity::Warning, DiagSource::ChuckCompiler, "ChucK Compiler",
                       "CK001", "CK warning", "file:///test.ck", 3, 2, ""});
    registry.setDiagnostics(DiagSource::ChuckCompiler, "file:///test.ck", ckDiags);

    std::vector<Diagnostic> workerDiags;
    workerDiags.push_back({0, DiagSeverity::Error, DiagSource::ChuckWorker, "ChucK Worker",
                           "WK001", "Worker died", "file:///test.ck", 0, 0, "pid=42"});
    registry.setDiagnostics(DiagSource::ChuckWorker, "file:///test.ck", workerDiags);

    REQUIRE(registry.total() == 3);
    REQUIRE(registry.errorCount() == 2);
    REQUIRE(registry.warningCount() == 1);

    // diagnosticsForUri across sources
    auto testCk = registry.diagnosticsForUri("file:///test.ck");
    REQUIRE(testCk.size() == 2);
}
