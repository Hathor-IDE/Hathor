// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckDiagnostics.cpp — real libchuck compiler diagnostics (B4-K4, AI-5).
 *
 * This translation unit provides validateChuckSource(), the SINGLE canonical
 * ChucK diagnostic entry point used by:
 *   - ChuckCompiler::dispatcherLoop() during ck_compile (B4-K4)
 *   - ProjectReadFacade::getDiagnostics (AI-2)
 *   - ChuckSessionService::getDiagnostics (AI-5)
 *
 * When libchuck is linked (CHUCK_AVAILABLE=1), this calls the REAL vendored
 * compiler (ChucK::compileCode) and parses the libchuck EM_lasterror() output.
 * When libchuck is unavailable, it falls back to a bracket-balancing heuristic
 * so the codebase remains buildable on CI without bison/flex.
 */

#include "ChuckDiagnostics.hpp"

#ifdef CHUCK_AVAILABLE
#include "chuck.h"
#include "chuck_errmsg.h"
#endif

#include <atomic>
#include <cstdint>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>

namespace hathor::audio_worker {

// ---------------------------------------------------------------------------
// libchuck thread-safety guard
//
// libchuck's error state (g_lasterror in chuck_errmsg.cpp) is a global
// variable, NOT per-instance. Concurrent calls to compileCode() from multiple
// threads can clobber the shared error buffer. We serialize all compile
// calls with a global mutex to ensure correct diagnostics.
//
// This is NOT the performance-sensitive audio path — the per-VM ChuckCompiler
// dispatcherLoop() already serializes compilation per VM, and the
// control-layer validateChuckSource() is called on the JobTracker worker
// thread. The mutex only matters when multiple tabs compile simultaneously
// or when AI-2 diagnostics run concurrently with a compile job.
// ---------------------------------------------------------------------------
namespace {
    std::mutex& chuckCompileMutex() {
        static std::mutex m;
        return m;
    }
}

// ---------------------------------------------------------------------------
// Error string parsing
//
// libchuck's EM_error produces output appended to g_lasterror in the format:
//   [filename]:line:char: message
// or (without filename):
//   [chuck:0:0]: message
//
// EM_error2 uses:
//   [chuck:0:0]:message
//   or [filename]:0:message
//
// We parse the line/column from the error string if present. If they
// cannot be parsed, we return line=0, column=0 (per AI-5 §9).
// ---------------------------------------------------------------------------

static ChuckDiagnostic parseLibchuckErrors(const std::string& errorStr,
                                           std::string_view /*src*/)
{
    if (errorStr.empty()) {
        return {true, 0, 0, {}};
    }

    // libchuck's g_lasterror contains the accumulated error output. The
    // format from EM_error() is typically:
    //   [filename]:line:col: message
    // or for code literals (no filename, using a pseudo-filename):
    //   [compiled.code]:line:col: message
    // Multiple errors may be present, one per line. We parse the first
    // error line that matches.
    // Use ECMAScript regex (default) — POSIX extended regex (std::regex::extended)
    // does not support (?:...) non-capturing groups or \s.
    //
    // libchuck error format from EM_error:
    //   <filename>:line:col: message
    // where <filename> can be:
    //   [compiled.code]
    //   test.ck
    //   [chuck]
    // (angle brackets for code compiled with a literal filename, square brackets
    // for pseudo-filenames, or bare filenames)
    static const std::regex lineColRe(
        R"((.+):(\d+):(\d+):\s*(.+))"
    );

    // First, extract the first non-empty error line from the accumulated output.
    // libchuck accumulates errors separated by newlines in g_lasterror.
    std::string firstLine;
    {
        std::istringstream iss(errorStr);
        std::string line;
        while (std::getline(iss, line)) {
            // Skip empty/blank lines
            line.erase(0, line.find_first_not_of(" \t"));
            if (!line.empty()) {
                firstLine = line;
                break;
            }
        }
    }

    if (firstLine.empty()) {
        return {true, 0, 0, {}};
    }

    std::smatch match;
    if (std::regex_match(firstLine, match, lineColRe) && match.size() >= 5) {
        int line = 0, col = 0;
        try { line = std::stoi(match[2].str()); } catch (...) { line = 0; }
        if (!match[3].str().empty()) {
            try { col = std::stoi(match[3].str()); } catch (...) { col = 0; }
        }
        std::string msg = match[4].str();
        return {false, line, col, std::move(msg)};
    }

    // If we can't parse line/col, return the raw error text with line=0, col=0
    // (per AI-5 §9: do not fabricate positions when none are provided)
    return {false, 0, 0, errorStr};
}

// ---------------------------------------------------------------------------
// Real libchuck validation (used when CHUCK_AVAILABLE is defined)
// ---------------------------------------------------------------------------

#ifdef CHUCK_AVAILABLE
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static ChuckDiagnostic validateWithLibchuck(std::string_view src)
{
    // Serialize access to global libchuck error state
    std::lock_guard<std::mutex> lock(chuckCompileMutex());

    // Create a transient ChucK instance for compile-only diagnostics.
    // We do NOT start the audio thread — we just need the compiler's
    // parse → type-check → emit pipeline.
    ChucK ck;

    // Configure for non-realtime, compile-only operation.
    ck.setParam(CHUCK_PARAM_SAMPLE_RATE, 44100);
    ck.setParam(CHUCK_PARAM_INPUT_CHANNELS, 0);
    ck.setParam(CHUCK_PARAM_OUTPUT_CHANNELS, 2);
    ck.setParam(CHUCK_PARAM_VM_HALT, TRUE);       // don't start the VM loop
    ck.setParam(CHUCK_PARAM_IS_REALTIME_AUDIO_HINT, FALSE);

    // Initialize the compiler (sets up the compile target, types, etc.)
    if (!ck.init()) {
        return {false, 0, 0, "libchuck init() failed"};
    }

    // Clear any stale error state before compiling
    EM_reset_msg();

    // Silence stdout/stderr callbacks to avoid polluting our error capture
    ck.setChoutCallback(nullptr);

    std::string codeStr(src);
    std::vector<t_CKUINT> shredIDs;

    // Use immediate=FALSE (deferred spork) per K0.5.
    // For diagnostics-only (no VM to spork into), this still does the
    // full parse → type-check → emit pipeline.
    t_CKBOOL ok = ck.compileCode(codeStr, "", 1, FALSE, &shredIDs, "test.ck");

    // Read the error output (even on success, there may be warnings)
    const char* errStr = EM_lasterror();

    if (ok && (!errStr || std::string(errStr).empty())) {
        // Clean compile, no warnings
        return {true, 0, 0, {}};
    }

    if (ok) {
        // Compilation succeeded but there may be warnings captured in errStr
        // Treat warnings as success (ok=true) with the warning text as message
        if (errStr && std::string(errStr).find_first_not_of(" \t\n\r") != std::string::npos) {
            return {true, 0, 0, errStr};
        }
        return {true, 0, 0, {}};
    }

    // Compilation failed — parse the error string
    return parseLibchuckErrors(errStr ? errStr : "", src);
}
#endif // CHUCK_AVAILABLE

// ---------------------------------------------------------------------------
// Fallback validation (used when libchuck is NOT available)
// ---------------------------------------------------------------------------

// Mark as potentially unused to avoid -Werror when CHUCK_AVAILABLE is set
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static ChuckDiagnostic validateFallback(std::string_view src)
{
    int parenDepth = 0, braceDepth = 0, bracketDepth = 0;
    bool hasSporkOrAssignment = false;
    int line = 1, col = 1;

    for (std::size_t i = 0; i < src.size(); ++i) {
        char c = src[i];

        if (c == '\n') { ++line; col = 1; continue; }
        ++col;

        if (c == '(') ++parenDepth;
        else if (c == ')') --parenDepth;
        else if (c == '{') ++braceDepth;
        else if (c == '}') --braceDepth;
        else if (c == '[') ++bracketDepth;
        else if (c == ']') --bracketDepth;

        if (c == '=' && i + 1 < src.size() && src[i + 1] == '>')
            hasSporkOrAssignment = true;

        if (parenDepth < 0 || braceDepth < 0 || bracketDepth < 0) {
            return {false, line, col,
                "unexpected ')' or '}' or ']' at mismatched position"};
        }
    }

    if (parenDepth > 0)
        return {false, line, col, "unbalanced parentheses: missing ')'"};
    if (braceDepth > 0)
        return {false, line, col, "unbalanced braces: missing '}'"};
    if (bracketDepth > 0)
        return {false, line, col, "unbalanced brackets: missing ']'"};
    if (parenDepth < 0)
        return {false, line, col, "unbalanced parentheses: extra ')'"};
    if (braceDepth < 0)
        return {false, line, col, "unbalanced braces: extra '}'"};
    if (bracketDepth < 0)
        return {false, line, col, "unbalanced brackets: extra ']'"};

    if (!hasSporkOrAssignment) {
        if (src.find(';') == std::string::npos) {
            return {false, 1, 1,
                "expected ChucK sporking operator (=>) or statement terminator (;)"};
        }
    }

    return {true, 0, 0, {}};
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

ChuckDiagnostic validateChuckSource(std::string_view src)
{
#ifdef CHUCK_AVAILABLE
    return validateWithLibchuck(src);
#else
    return validateFallback(src);
#endif
}

} // namespace hathor::audio_worker
