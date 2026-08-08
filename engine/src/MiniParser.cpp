// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * MiniParser.cpp — single-pass tokeniser, recursive-descent parser,
 * AST lowering to Pattern<std::string>, and CompiledPattern special members.
 *
 * Requirement references: 5.1, 5.2, 5.3, 5.6
 */

#include "hathor/MiniParser.hpp"
#include "hathor/MiniTokeniser.hpp"
#include "MiniAst.hpp"
#include "hathor/Combinators.hpp"
#include "hathor/Pattern.hpp"

#include <cassert>
#include <cctype>
#include <charconv>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace hathor {

// ---------------------------------------------------------------------------
// CompiledPattern special members (MiniNode is complete here via MiniAst.hpp)
// ---------------------------------------------------------------------------

CompiledPattern::CompiledPattern()
    : pattern(Pattern<std::string>{
          [](Arc, std::span<Event<std::string>>) -> std::size_t { return 0; },
          0})
    , ast(nullptr)
{}

CompiledPattern::~CompiledPattern() = default;

CompiledPattern::CompiledPattern(CompiledPattern&&) noexcept = default;

CompiledPattern& CompiledPattern::operator=(CompiledPattern&&) noexcept = default;

// ---------------------------------------------------------------------------
// Tokeniser
// ---------------------------------------------------------------------------

/// Set of characters that are single-character special tokens (not part of atoms).
static bool isSpecial(char c) noexcept
{
    switch (c) {
    case '[': case ']':
    case '<': case '>':
    case '*': case '/': case '!':
    case '~':
    case '(': case ')': case ',':
        return true;
    default:
        return false;
    }
}

/// Tokenise `input` into a flat token list. Single-pass, no regex.
std::vector<Token> tokenise(std::string_view input)
{
    std::vector<Token> tokens;
    std::size_t i = 0;
    const std::size_t len = input.size();

    while (i < len) {
        // Skip whitespace.
        if (std::isspace(static_cast<unsigned char>(input[i]))) {
            ++i;
            continue;
        }

        char c = input[i];

        // Single-character special tokens.
        if (c == '[') { tokens.push_back({TokenKind::TK_LBRACKET, input.substr(i, 1), i}); ++i; continue; }
        if (c == ']') { tokens.push_back({TokenKind::TK_RBRACKET, input.substr(i, 1), i}); ++i; continue; }
        if (c == '<') { tokens.push_back({TokenKind::TK_LANGLE,   input.substr(i, 1), i}); ++i; continue; }
        if (c == '>') { tokens.push_back({TokenKind::TK_RANGLE,   input.substr(i, 1), i}); ++i; continue; }
        if (c == '*') { tokens.push_back({TokenKind::TK_STAR,     input.substr(i, 1), i}); ++i; continue; }
        if (c == '/') { tokens.push_back({TokenKind::TK_SLASH,    input.substr(i, 1), i}); ++i; continue; }
        if (c == '!') { tokens.push_back({TokenKind::TK_BANG,     input.substr(i, 1), i}); ++i; continue; }
        if (c == '~') { tokens.push_back({TokenKind::TK_TILDE,    input.substr(i, 1), i}); ++i; continue; }
        if (c == '(') { tokens.push_back({TokenKind::TK_LPAREN,   input.substr(i, 1), i}); ++i; continue; }
        if (c == ')') { tokens.push_back({TokenKind::TK_RPAREN,   input.substr(i, 1), i}); ++i; continue; }
        if (c == ',') { tokens.push_back({TokenKind::TK_COMMA,    input.substr(i, 1), i}); ++i; continue; }

        // Integer literal: one or more decimal digits.
        if (std::isdigit(static_cast<unsigned char>(c))) {
            std::size_t start = i;
            while (i < len && std::isdigit(static_cast<unsigned char>(input[i])))
                ++i;
            tokens.push_back({TokenKind::TK_INT, input.substr(start, i - start), start});
            continue;
        }

        // Atom: any non-whitespace, non-special character sequence.
        if (!std::isspace(static_cast<unsigned char>(c)) && !isSpecial(c)) {
            std::size_t start = i;
            while (i < len
                   && !std::isspace(static_cast<unsigned char>(input[i]))
                   && !isSpecial(input[i]))
            {
                ++i;
            }
            tokens.push_back({TokenKind::TK_ATOM, input.substr(start, i - start), start});
            continue;
        }

        // Unrecognised character — emit error token.
        tokens.push_back({TokenKind::TK_ERROR, input.substr(i, 1), i});
        ++i;
    }

    tokens.push_back({TokenKind::TK_EOF, {}, len});
    return tokens;
}

// ---------------------------------------------------------------------------
// Recursive-descent parser
// ---------------------------------------------------------------------------

struct Parser {
    const std::vector<Token>& tokens;
    std::size_t               pos = 0;

    const Token& peek() const noexcept { return tokens[pos]; }
    const Token& advance() noexcept    { return tokens[pos++]; }

    bool check(TokenKind k) const noexcept { return peek().kind == k; }

    bool at_end() const noexcept
    {
        return peek().kind == TokenKind::TK_EOF || peek().kind == TokenKind::TK_ERROR;
    }

    // -----------------------------------------------------------------------
    // Error helpers
    // -----------------------------------------------------------------------

    using Result = std::variant<MiniNodePtr, ParseError>;

    static Result err(std::size_t p, std::string msg)
    {
        return ParseError{p, std::move(msg)};
    }

    // -----------------------------------------------------------------------
    // Parse an integer after a modifier symbol.
    // Returns -1 on failure (caller emits the error).
    // -----------------------------------------------------------------------
    int parseModifierInt(std::size_t symPos, char symChar)
    {
        (void)symChar;
        if (!check(TokenKind::TK_INT)) return -1;
        const Token& t = advance();
        int val = 0;
        auto [ptr, ec] = std::from_chars(t.text.data(), t.text.data() + t.text.size(), val);
        if (ec != std::errc{} || val <= 0) return -1;
        (void)symPos;
        return val;
    }

    // -----------------------------------------------------------------------
    // parseElement — one "atom or group" with optional modifier suffix.
    // -----------------------------------------------------------------------
    Result parseElement()
    {
        const Token& t = peek();

        MiniNodePtr child;

        if (t.kind == TokenKind::TK_ATOM) {
            advance();
            child = std::make_unique<MiniNode>(MiniAtom{std::string(t.text), t.pos});
        }
        else if (t.kind == TokenKind::TK_TILDE) {
            advance();
            child = std::make_unique<MiniNode>(MiniAtom{std::string("~"), t.pos});
        }
        else if (t.kind == TokenKind::TK_LBRACKET) {
            std::size_t openPos = t.pos;
            advance(); // consume '['

            // Parse inner pattern (possibly empty).
            std::vector<MiniNodePtr> steps;
            while (!check(TokenKind::TK_RBRACKET)) {
                if (at_end() && !check(TokenKind::TK_RBRACKET)) {
                    return err(openPos, "unclosed '[': expected ']'");
                }
                auto res = parseElement();
                if (std::holds_alternative<ParseError>(res))
                    return res;
                steps.push_back(std::move(std::get<MiniNodePtr>(res)));
            }
            if (!check(TokenKind::TK_RBRACKET))
                return err(openPos, "unclosed '[': expected ']'");
            advance(); // consume ']'

            if (steps.empty())
                return err(openPos, "empty bracket group '[]'");

            MiniSeq seq;
            seq.steps = std::move(steps);
            seq.bracketed = true;
            child = std::make_unique<MiniNode>(std::move(seq));
        }
        else if (t.kind == TokenKind::TK_LANGLE) {
            std::size_t openPos = t.pos;
            advance(); // consume '<'

            std::vector<MiniNodePtr> steps;
            while (!check(TokenKind::TK_RANGLE)) {
                if (at_end() && !check(TokenKind::TK_RANGLE)) {
                    return err(openPos, "unclosed '<': expected '>'");
                }
                auto res = parseElement();
                if (std::holds_alternative<ParseError>(res))
                    return res;
                steps.push_back(std::move(std::get<MiniNodePtr>(res)));
            }
            if (!check(TokenKind::TK_RANGLE))
                return err(openPos, "unclosed '<': expected '>'");
            advance(); // consume '>'

            if (steps.empty())
                return err(openPos, "empty angle-bracket group '<>'");

            child = std::make_unique<MiniNode>(MiniSlowSeq{std::move(steps)});
        }
        else if (t.kind == TokenKind::TK_INT) {
            // A bare integer is a valid atom token (e.g. "0", "1").
            advance();
            child = std::make_unique<MiniNode>(MiniAtom{std::string(t.text), t.pos});
        }
        else {
            return err(t.pos, std::string("unexpected token '") + std::string(t.text) + "'");
        }

        // ---- Optional modifier suffix: *N, /N, !N ----
        while (true) {
            if (check(TokenKind::TK_STAR)) {
                std::size_t symPos = peek().pos;
                advance();
                int n = parseModifierInt(symPos, '*');
                if (n <= 0)
                    return err(symPos, "expected positive integer after '*'");
                child = std::make_unique<MiniNode>(MiniFast{std::move(child), n});
            }
            else if (check(TokenKind::TK_SLASH)) {
                std::size_t symPos = peek().pos;
                advance();
                int n = parseModifierInt(symPos, '/');
                if (n <= 0)
                    return err(symPos, "expected positive integer after '/'");
                child = std::make_unique<MiniNode>(MiniSlow{std::move(child), n});
            }
            else if (check(TokenKind::TK_BANG)) {
                std::size_t symPos = peek().pos;
                advance();
                int n = parseModifierInt(symPos, '!');
                if (n <= 0)
                    return err(symPos, "expected positive integer after '!'");
                child = std::make_unique<MiniNode>(MiniRep{std::move(child), n});
            }
            // ---- Euclid postfix: child(pulses, steps[, rotation]) ----
            else if (check(TokenKind::TK_LPAREN)) {
                std::size_t parenPos = peek().pos;
                advance(); // consume '('

                // Parse pulses: non-negative integer.
                if (!check(TokenKind::TK_INT))
                    return err(parenPos, "expected integer for euclid pulses after '('");
                const Token& tPulses = advance();
                int pulses = 0;
                {
                    auto [ptr, ec] = std::from_chars(
                        tPulses.text.data(),
                        tPulses.text.data() + tPulses.text.size(), pulses);
                    if (ec != std::errc{})
                        return err(tPulses.pos, "invalid integer for euclid pulses");
                    (void)ptr;
                }

                // Parse ','
                if (!check(TokenKind::TK_COMMA))
                    return err(parenPos, "expected ',' after euclid pulses");
                advance(); // consume ','

                // Parse steps: positive integer.
                if (!check(TokenKind::TK_INT))
                    return err(parenPos, "expected integer for euclid steps after ','");
                const Token& tSteps = advance();
                int steps = 0;
                {
                    auto [ptr, ec] = std::from_chars(
                        tSteps.text.data(),
                        tSteps.text.data() + tSteps.text.size(), steps);
                    if (ec != std::errc{} || steps <= 0)
                        return err(tSteps.pos, "expected positive integer for euclid steps");
                    (void)ptr;
                }

                // Parse optional rotation: ',' + integer
                int rotation = 0;
                if (check(TokenKind::TK_COMMA)) {
                    advance(); // consume ','
                    if (!check(TokenKind::TK_INT))
                        return err(parenPos, "expected integer for euclid rotation after ','");
                    const Token& tRot = advance();
                    auto [ptr, ec] = std::from_chars(
                        tRot.text.data(),
                        tRot.text.data() + tRot.text.size(), rotation);
                    if (ec != std::errc())
                        return err(tRot.pos, "invalid integer for euclid rotation");
                    (void)ptr;
                }

                // Parse ')'
                if (!check(TokenKind::TK_RPAREN))
                    return err(parenPos, "expected ')' after euclid parameters");
                advance(); // consume ')'

                child = std::make_unique<MiniNode>(
                    MiniEuclid{std::move(child), pulses, steps, rotation});
            }
            else {
                break;
            }
        }

        return child;
    }

    // -----------------------------------------------------------------------
    // parsePattern — top-level sequence of elements.
    //
    // Space-separated elements form a sequential pattern (stepcat).
    // Comma-separated elements form a stack (all play concurrently).
    // -----------------------------------------------------------------------
    std::variant<MiniNodePtr, ParseError> parsePattern()
    {
        // Collect comma groups: each is a vector of space-separated elements.
        std::vector<std::vector<MiniNodePtr>> groups;
        std::vector<MiniNodePtr> current;

        // Parse leading commas / first element.
        if (check(TokenKind::TK_COMMA)) {
            advance();
            groups.push_back({});
            current.clear();
        }

        while (!at_end()) {
            if (check(TokenKind::TK_RBRACKET) || check(TokenKind::TK_RANGLE))
                break;

            if (check(TokenKind::TK_COMMA)) {
                groups.push_back(std::move(current));
                current.clear();
                advance();
                continue;
            }

            auto res = parseElement();
            if (std::holds_alternative<ParseError>(res))
                return res;
            current.push_back(std::move(std::get<MiniNodePtr>(res)));
        }

        // Push the last group.
        groups.push_back(std::move(current));

        // Check for empty groups (leading/trailing/consecutive commas).
        std::size_t nonEmpty = 0;
        for (auto& g : groups)
            if (!g.empty()) ++nonEmpty;

        if (nonEmpty == 0)
            return ParseError{0, "empty pattern"};

        // If only one non-empty group, it's a space-separated sequence (no commas).
        if (nonEmpty == 1) {
            for (auto& g : groups) {
                if (g.empty()) continue;
                if (g.size() == 1)
                    return std::move(g[0]);
                return std::make_unique<MiniNode>(MiniSeq{std::move(g), false});
            }
        }

        // Multiple non-empty groups → MiniStack.
        std::vector<MiniNodePtr> stackChildren;
        for (auto& g : groups) {
            if (g.empty()) continue;
            if (g.size() == 1)
                stackChildren.push_back(std::move(g[0]));
            else
                stackChildren.push_back(
                    std::make_unique<MiniNode>(MiniSeq{std::move(g), true}));
        }

        return std::make_unique<MiniNode>(MiniStack{std::move(stackChildren)});
    }
};

// ---------------------------------------------------------------------------
// AST lowering to Pattern<std::string>
// ---------------------------------------------------------------------------

static Pattern<std::string> lowerNode(const MiniNode& node);

static std::vector<Pattern<std::string>> lowerChildren(const std::vector<MiniNodePtr>& steps)
{
    std::vector<Pattern<std::string>> pats;
    pats.reserve(steps.size());
    for (const auto& s : steps)
        pats.push_back(lowerNode(*s));
    return pats;
}

static Pattern<std::string> lowerNode(const MiniNode& node)
{
    return std::visit([](const auto& alt) -> Pattern<std::string> {
        using T = std::decay_t<decltype(alt)>;

        if constexpr (std::is_same_v<T, MiniAtom>) {
            // In Strudel, "~" (rest/silence) produces no events.
            if (alt.token == "~") {
                return Pattern<std::string>{
                    [](Arc, std::span<Event<std::string>>) -> std::size_t { return 0; },
                    0
                };
            }
            return pure(alt.token, alt.sourceOffset);
        }
        else if constexpr (std::is_same_v<T, MiniSeq>) {
            if (alt.steps.size() == 1)
                return lowerNode(*alt.steps[0]);

            // Lower to stepcat with weights: MiniRep contributes its count
            // as the weight, normal elements contribute weight 1.
            // "bd!3 sn" → stepcat([(3, fastcat(bd,bd,bd)), (1, sn)])
            std::vector<std::pair<int, Pattern<std::string>>> wp;
            for (const auto& step : alt.steps) {
                if (std::holds_alternative<MiniRep>(*step)) {
                    const auto& rep = std::get<MiniRep>(*step);
                    // MiniRep!N lowers to fastcat of N copies of the child.
                    std::vector<Pattern<std::string>> copies;
                    copies.reserve(static_cast<std::size_t>(rep.count));
                    for (int i = 0; i < rep.count; ++i)
                        copies.push_back(lowerNode(*rep.child));
                    wp.emplace_back(rep.count, fastcat(std::move(copies)));
                } else {
                    wp.emplace_back(1, lowerNode(*step));
                }
            }
            return stepcat(std::move(wp));
        }
        else if constexpr (std::is_same_v<T, MiniStack>) {
            // Comma-separated → stack
            return stack(lowerChildren(alt.steps));
        }
        else if constexpr (std::is_same_v<T, MiniSlowSeq>) {
            if (alt.steps.size() == 1)
                return lowerNode(*alt.steps[0]);
            return slowcat(lowerChildren(alt.steps));
        }
        else if constexpr (std::is_same_v<T, MiniFast>) {
            return fast(Rational{static_cast<int64_t>(alt.factor)}, lowerNode(*alt.child));
        }
        else if constexpr (std::is_same_v<T, MiniSlow>) {
            return slow(Rational{static_cast<int64_t>(alt.factor)}, lowerNode(*alt.child));
        }
        else if constexpr (std::is_same_v<T, MiniRep>) {
            // Standalone MiniRep (not inside MiniSeq) → fastcat of N copies.
            std::vector<Pattern<std::string>> copies;
            copies.reserve(static_cast<std::size_t>(alt.count));
            for (int i = 0; i < alt.count; ++i)
                copies.push_back(lowerNode(*alt.child));
            return fastcat(std::move(copies));
        }
        else if constexpr (std::is_same_v<T, MiniEuclid>) {
            // child(pulses, steps[, rotation]) → euclid
            return euclid(alt.pulses, alt.steps, alt.rotation,
                          lowerNode(*alt.child));
        }
        else {
            // Should never reach here.
            return pure(std::string{}, 0);
        }
    }, static_cast<const std::variant<MiniAtom, MiniSeq, MiniStack, MiniSlowSeq, MiniFast, MiniSlow, MiniRep, MiniEuclid>&>(node));
}


// ---------------------------------------------------------------------------
// parseMini — public entry point
// ---------------------------------------------------------------------------

std::variant<CompiledPattern, ParseError> parseMini(std::string_view input)
{
    // Tokenise.
    auto tokens = tokenise(input);

    // Check for tokeniser errors.
    for (const auto& tok : tokens) {
        if (tok.kind == TokenKind::TK_ERROR) {
            return ParseError{tok.pos,
                std::string("unrecognised character '") + std::string(tok.text) + "'"};
        }
    }

    // Parse.
    Parser p{tokens, 0};
    auto parseResult = p.parsePattern();

    if (std::holds_alternative<ParseError>(parseResult))
        return std::get<ParseError>(parseResult);

    // Check nothing is left over (ignoring EOF).
    if (!p.check(TokenKind::TK_EOF) && !p.at_end()) {
        const Token& leftover = p.peek();
        return ParseError{leftover.pos,
            std::string("unexpected token '") + std::string(leftover.text) + "' after pattern"};
    }

    MiniNodePtr astRoot = std::move(std::get<MiniNodePtr>(parseResult));

    // Lower AST → Pattern<std::string>.
    Pattern<std::string> compiled = lowerNode(*astRoot);

    // Pack into CompiledPattern.
    CompiledPattern cp;
    cp.pattern = std::move(compiled);
    cp.ast     = std::move(astRoot);

    return cp;
}

} // namespace hathor
