// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_lsp_protocol.cpp — unit tests for LSP type definitions (LspProtocol.hpp).
 *
 * JUCE-free tests compiled into the hathor-ui-tests target (req 31.1).
 */

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("Position constructs with values via aggregate init", "[lsp][protocol]")
{
    Position p{5, 12};
    REQUIRE(p.line == 5);
    REQUIRE(p.character == 12);
}

TEST_CASE("Position equality operator", "[lsp][protocol]")
{
    Position a{1, 2};
    Position b{1, 2};
    Position c{1, 3};

    REQUIRE(a == b);
    REQUIRE_FALSE(a == c);
}

TEST_CASE("Range default constructs empty", "[lsp][protocol]")
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
    REQUIRE(d.source.has_value() == false);
    REQUIRE(d.code.has_value() == false);
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
    REQUIRE(d.source.value() == "hathor-lsp");
    REQUIRE(d.code.value() == "PARSE_ERROR");
    REQUIRE(d.message == "Unexpected token");
    REQUIRE(d.range.start.line == 2);
    REQUIRE(d.range.end.character == 10);
}

TEST_CASE("DiagnosticSeverity values", "[lsp][protocol]")
{
    REQUIRE(static_cast<int>(DiagnosticSeverity::Error) == 1);
    REQUIRE(static_cast<int>(DiagnosticSeverity::Warning) == 2);
    REQUIRE(static_cast<int>(DiagnosticSeverity::Info) == 3);
    REQUIRE(static_cast<int>(DiagnosticSeverity::Hint) == 4);
}

// ===========================================================================
// CompletionItem / CompletionCandidate
// ===========================================================================

TEST_CASE("CompletionItem defaults", "[lsp][protocol]")
{
    CompletionItem item;
    REQUIRE(item.label.empty());
    REQUIRE(!item.kind.has_value());
    REQUIRE(!item.detail.has_value());
    REQUIRE(!item.documentation.has_value());
    REQUIRE(!item.insertText.has_value());
    REQUIRE(!item.sortText.has_value());
}

TEST_CASE("CompletionItem with fields", "[lsp][protocol]")
{
    CompletionItem item;
    item.label = "fast";
    item.kind = CompletionItemKind::Function;
    item.detail = "pattern operator";
    item.documentation = MarkupContent{"markdown", "Speed up or slow down a pattern."};
    item.insertText = "fast(2)";
    item.sortText = "fast";

    REQUIRE(item.label == "fast");
    REQUIRE(item.kind.value() == CompletionItemKind::Function);
    REQUIRE(item.detail.value() == "pattern operator");
    REQUIRE(item.documentation.value().value == "Speed up or slow down a pattern.");
    REQUIRE(item.insertText.value() == "fast(2)");
    REQUIRE(item.sortText.value() == "fast");
}

TEST_CASE("CompletionCandidate default constructs", "[lsp][protocol]")
{
    CompletionCandidate c;
    REQUIRE(c.label.empty());
    REQUIRE(c.kind == CompletionItemKind::Text);
    REQUIRE(c.detail.empty());
    REQUIRE(c.documentation.empty());
    REQUIRE(c.insertText.empty());
    REQUIRE(c.source.empty());
}

TEST_CASE("CompletionCandidate populated via field assignment", "[lsp][protocol]")
{
    CompletionCandidate c;
    c.label = "stack";
    c.kind = CompletionItemKind::Function;
    c.detail = "pattern combiner";
    c.documentation = "Stack patterns.";
    c.insertText = "stack(";
    c.source = "metadata";

    REQUIRE(c.label == "stack");
    REQUIRE(c.kind == CompletionItemKind::Function);
    REQUIRE(c.documentation == "Stack patterns.");
    REQUIRE(c.source == "metadata");
}

TEST_CASE("CompletionItemKind values", "[lsp][protocol]")
{
    REQUIRE(static_cast<int>(CompletionItemKind::Text) == 1);
    REQUIRE(static_cast<int>(CompletionItemKind::Method) == 2);
    REQUIRE(static_cast<int>(CompletionItemKind::Function) == 3);
    REQUIRE(static_cast<int>(CompletionItemKind::Variable) == 6);
    REQUIRE(static_cast<int>(CompletionItemKind::Value) == 12);
    REQUIRE(static_cast<int>(CompletionItemKind::Enum) == 13);
    REQUIRE(static_cast<int>(CompletionItemKind::Keyword) == 14);
    REQUIRE(static_cast<int>(CompletionItemKind::Snippet) == 15);
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

    CompletionCandidate a;
    a.label = "fast";
    a.kind = CompletionItemKind::Function;
    a.source = "lsp";

    CompletionCandidate b;
    b.label = "slow";
    b.kind = CompletionItemKind::Function;
    b.source = "lsp";

    result.items.push_back(std::move(a));
    result.items.push_back(std::move(b));

    REQUIRE(result.isIncomplete == true);
    REQUIRE(result.items.size() == 2);
    REQUIRE(result.items[0].label == "fast");
    REQUIRE(result.items[1].label == "slow");
}

TEST_CASE("CompletionList default constructs empty", "[lsp][protocol]")
{
    CompletionList list;
    REQUIRE(list.isIncomplete == false);
    REQUIRE(list.items.empty());
}

// ===========================================================================
// Hover
// ===========================================================================

TEST_CASE("Hover defaults to empty", "[lsp][protocol]")
{
    Hover h;
    REQUIRE(h.contents.empty());
    REQUIRE(!h.range.has_value());
}

TEST_CASE("Hover with range and contents", "[lsp][protocol]")
{
    Hover h;
    h.contents.push_back({"markdown", "fast(multiplier: number) — speed up a pattern"});
    h.range = Range{{0, 0}, {0, 10}};

    REQUIRE(h.contents.size() == 1);
    REQUIRE(h.contents[0].kind == "markdown");
    REQUIRE(h.contents[0].value.find("speed up") != std::string::npos);
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

    SignatureInformation sig;
    sig.label = "fast(multiplier: number)";
    ParameterInformation param;
    param.label = "multiplier";
    sig.parameters.push_back(std::move(param));

    sh.signatures.push_back(std::move(sig));

    REQUIRE(sh.signatures.size() == 1);
    REQUIRE(sh.signatures[0].label == "fast(multiplier: number)");
    REQUIRE(sh.signatures[0].parameters.size() == 1);
    REQUIRE(sh.signatures[0].parameters[0].label == "multiplier");
}

// ===========================================================================
// MarkupContent
// ===========================================================================

TEST_CASE("MarkupContent default constructs empty", "[lsp][protocol]")
{
    MarkupContent mc;
    REQUIRE(mc.kind.empty());
    REQUIRE(mc.value.empty());
}

TEST_CASE("MarkupContent with values", "[lsp][protocol]")
{
    MarkupContent mc{"plaintext", "Hello world"};
    REQUIRE(mc.kind == "plaintext");
    REQUIRE(mc.value == "Hello world");
}

// ===========================================================================
// TextDocumentItem
// ===========================================================================

TEST_CASE("TextDocumentItem default constructs empty", "[lsp][protocol]")
{
    TextDocumentItem td;
    REQUIRE(td.uri.empty());
    REQUIRE(td.languageId.empty());
    REQUIRE(td.version == 0);
    REQUIRE(td.text.empty());
}

TEST_CASE("TextDocumentItem with values", "[lsp][protocol]")
{
    TextDocumentItem td;
    td.uri = "file:///test.hathor";
    td.languageId = "hathor";
    td.version = 1;
    td.text = "bd sn";

    REQUIRE(td.uri == "file:///test.hathor");
    REQUIRE(td.languageId == "hathor");
    REQUIRE(td.version == 1);
    REQUIRE(td.text == "bd sn");
}

// ===========================================================================
// CompletionContext
// ===========================================================================

TEST_CASE("CompletionContext default constructs", "[lsp][protocol]")
{
    CompletionContext ctx;
    REQUIRE(ctx.kind == CompletionContextKind::Code);
    REQUIRE(ctx.prefix.empty());
    REQUIRE(ctx.fullText.empty());
}

TEST_CASE("CompletionContext with values", "[lsp][protocol]")
{
    CompletionContext ctx;
    ctx.kind = CompletionContextKind::FunctionArgs;
    ctx.prefix = "fast";
    ctx.fullText = "fast(2)";
    ctx.position = {0, 4};
    ctx.uri = "file:///test.hathor";

    REQUIRE(ctx.kind == CompletionContextKind::FunctionArgs);
    REQUIRE(ctx.prefix == "fast");
    REQUIRE(ctx.position.line == 0);
    REQUIRE(ctx.position.character == 4);
}
