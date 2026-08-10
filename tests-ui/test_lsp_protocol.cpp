// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_lsp_protocol.cpp — unit tests for LSP type definitions (LspProtocol.hpp).
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "LspProtocol.hpp"

using namespace hathor::lsp;

// ===========================================================================
// Position and Range
// ===========================================================================

TEST_CASE("Position defaults to 0/0", "[lsp][protocol]")
{
    Position p;
    REQUIRE(p.line == 0);
    REQUIRE(p.character == 0);
}

TEST_CASE("Position constructs with values", "[lsp][protocol]")
{
    Position p{5, 12};
    REQUIRE(p.line == 5);
    REQUIRE(p.character == 12);
}

TEST_CASE("Range defaults to 0/0-0/0", "[lsp][protocol]")
{
    Range r;
    REQUIRE(r.start.line == 0);
    REQUIRE(r.start.character == 0);
    REQUIRE(r.end.line == 0);
    REQUIRE(r.end.character == 0);
}

TEST_CASE("Range constructs with two positions", "[lsp][protocol]")
{
    Range r{{1, 2}, {3, 4}};
    REQUIRE(r.start.line == 1);
    REQUIRE(r.start.character == 2);
    REQUIRE(r.end.line == 3);
    REQUIRE(r.end.character == 4);
}

// ===========================================================================
// Diagnostic
// ===========================================================================

TEST_CASE("Diagnostic defaults", "[lsp][protocol]")
{
    Diagnostic d;
    REQUIRE(!d.severity.has_value());
    REQUIRE(d.source.empty());
    REQUIRE(d.code.empty());
    REQUIRE(d.message.empty());
    REQUIRE(d.range.start.line == 0);
}

TEST_CASE("Diagnostic with Error severity", "[lsp][protocol]")
{
    Diagnostic d;
    d.range = {{2, 0}, {2, 10}};
    d.severity = DiagnosticSeverity::Error;
    d.source = "hathor-lsp";
    d.code = "PARSE_ERROR";
    d.message = "Unexpected token";

    REQUIRE(d.severity.value() == DiagnosticSeverity::Error);
    REQUIRE(d.source == "hathor-lsp");
    REQUIRE(d.code == "PARSE_ERROR");
    REQUIRE(d.message == "Unexpected token");
    REQUIRE(d.range.start.line == 2);
    REQUIRE(d.range.end.character == 10);
}

TEST_CASE("DiagnosticSeverity values", "[lsp][protocol]")
{
    REQUIRE(DiagnosticSeverity::Error == 1);
    REQUIRE(DiagnosticSeverity::Warning == 2);
    REQUIRE(DiagnosticSeverity::Information == 3);
    REQUIRE(DiagnosticSeverity::Hint == 4);
}

// ===========================================================================
// CompletionItem / CompletionCandidate
// ===========================================================================

TEST_CASE("CompletionItem defaults", "[lsp][protocol]")
{
    CompletionItem item;
    REQUIRE(item.label.empty());
    REQUIRE(item.kind == CompletionItemKind::Unspecified);
    REQUIRE(item.detail.empty());
    REQUIRE(item.documentation.empty());
    REQUIRE(item.insertText.empty());
    REQUIRE(item.filterText.empty());
}

TEST_CASE("CompletionItem constructs with fields", "[lsp][protocol]")
{
    CompletionItem item;
    item.label = "fast";
    item.kind = CompletionItemKind::Function;
    item.detail = "pattern operator";
    item.documentation = "Speed up or slow down a pattern.";
    item.insertText = "fast(2)";
    item.filterText = "fast";

    REQUIRE(item.label == "fast");
    REQUIRE(item.kind == CompletionItemKind::Function);
    REQUIRE(item.detail == "pattern operator");
    REQUIRE(item.documentation == "Speed up or slow down a pattern.");
    REQUIRE(item.insertText == "fast(2)");
    REQUIRE(item.filterText == "fast");
}

TEST_CASE("CompletionCandidate is convertible from CompletionItem", "[lsp][protocol]")
{
    CompletionItem item;
    item.label = "stack";
    item.kind = CompletionItemKind::Function;
    item.insertText = "stack(";
    item.documentation = "Stack patterns.";

    CompletionCandidate candidate(item);
    REQUIRE(candidate.label == "stack");
    REQUIRE(candidate.insertText == "stack(");
    REQUIRE(candidate.kind == CompletionItemKind::Function);
    REQUIRE(candidate.documentation == "Stack patterns.");
}

TEST_CASE("CompletionCandidate insertText defaults to label", "[lsp][protocol]")
{
    CompletionItem item;
    item.label = "bd";
    item.kind = CompletionItemKind::Value;

    CompletionCandidate candidate(item);
    REQUIRE(candidate.insertText == "bd");
}

TEST_CASE("CompletionItemKind values", "[lsp][protocol]")
{
    REQUIRE(CompletionItemKind::Unspecified == 0);
    REQUIRE(CompletionItemKind::Text == 1);
    REQUIRE(CompletionItemKind::Method == 2);
    REQUIRE(CompletionItemKind::Function == 3);
    REQUIRE(CompletionItemKind::Variable == 6);
    REQUIRE(CompletionItemKind::Value == 21);
}

// ===========================================================================
// CompletionResult
// ===========================================================================

TEST_CASE("CompletionResult defaults to empty", "[lsp][protocol]")
{
    CompletionResult result;
    REQUIRE(result.items.empty());
    REQUIRE(result.isIncomplete == false);
}

TEST_CASE("CompletionResult holds items", "[lsp][protocol]")
{
    CompletionResult result;
    result.isIncomplete = true;

    CompletionCandidate a("fast");
    a.kind = CompletionItemKind::Function;
    CompletionCandidate b("slow");
    b.kind = CompletionItemKind::Function;

    result.items.push_back(std::move(a));
    result.items.push_back(std::move(b));

    REQUIRE(result.isIncomplete == true);
    REQUIRE(result.items.size() == 2);
    REQUIRE(result.items[0].label == "fast");
    REQUIRE(result.items[1].label == "slow");
}

// ===========================================================================
// Hover
// ===========================================================================

TEST_CASE("Hover defaults to empty", "[lsp][protocol]")
{
    Hover h;
    REQUIRE(h.contents.empty());
    REQUIRE(h.range.has_value() == false);
}

TEST_CASE("Hover with range", "[lsp][protocol]")
{
    Hover h;
    h.contents = "fast(multiplier: number) — speed up a pattern";
    h.range = Range{{0, 0}, {0, 10}};

    REQUIRE(h.contents == "fast(multiplier: number) — speed up a pattern");
    REQUIRE(h.range.has_value());
    REQUIRE(h.range->start.line == 0);
    REQUIRE(h.range->end.character == 10);
}

// ===========================================================================
// SignatureHelp
// ===========================================================================

TEST_CASE("SignatureHelp defaults empty", "[lsp][protocol]")
{
    SignatureHelp sh;
    REQUIRE(sh.signatures.empty());
    REQUIRE(sh.activeSignature == 0);
    REQUIRE(sh.activeParameter == 0);
}

TEST_CASE("SignatureHelp with signatures", "[lsp][protocol]")
{
    SignatureHelp sh;
    sh.activeSignature = 0;

    Signature sig;
    sig.label = "fast(multiplier: number)";
    sig.parameters.push_back({"multiplier", {0, 0}, {0, 10}});

    sh.signatures.push_back(std::move(sig));

    REQUIRE(sh.signatures.size() == 1);
    REQUIRE(sh.signatures[0].label == "fast(multiplier: number)");
    REQUIRE(sh.signatures[0].parameters.size() == 1);
    REQUIRE(sh.signatures[0].parameters[0].label == "multiplier");
}
