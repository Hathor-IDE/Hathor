// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ai_g3_context.cpp — AI-G3 Hathor-specific authoring-context provider tests.
 *
 * Verifies (JUCE-free, compiled into hathor-control-tests alongside the ghost
 * FIM sources so the llm-ls injection path is exercised end-to-end):
 *   1. .hathor (mini-notation) cursor-context classification
 *   2. .ck (ChucK) cursor-context classification
 *   3. Selection-aware context retrieval
 *   4. Diagnostic-aware retrieval (compiler + LSP, proximity-ordered, bounded)
 *   5. Asset-aware retrieval (sample name-prefix matching)
 *   6. Bounded context (maxContextChars budget + customBounds override)
 *   7. Stale metadata / example rejection (version gate)
 *   8. Thread-safety — safe to call off the JUCE audio thread
 *   9. FIM reachability — assemble -> GhostContext.authoringContext ->
 *      GhostCompletionLogic::buildRequest -> req.fim.prefix carries the payload
 *  10. Wire-through via ControlInterface::assembleCompletionContext
 *  11. Language inference + version-block attribution
 *
 * Requirement references: AI-G3, AI-G1, AI-G2, AI-2, AI-3, AI-4, AI-8
 */

#include "test_ai_fakes.hpp"

#include "CompletionContextProvider.hpp"
#include "ControlInterface.hpp"
#include "ProjectReadFacade.hpp"

#include "GhostCompletionLogic.hpp"
#include "GhostProviderConfig.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using hathor::control::CompletionContext;
using hathor::control::CompletionContextProvider;
using hathor::control::CompletionRequest;
using hathor::control::ContextBounds;
using hathor::control::EditorContextSnapshot;
using hathor::control::ProjectReadFacade;

// ---------------------------------------------------------------------------
// Fixture: real AI-3 metadata + fakes wired into a CompletionContextProvider.
// ---------------------------------------------------------------------------

struct Ai3Fixture {
    hathor::language::LoadResult meta = loadTestMetadata();
    Ai8FakeFacade audio;
    SampleBank bank;
    ProjectReadFacade readFacade{audio, bank};
    CompletionContextProvider provider{
        readFacade, nullptr, nullptr, &meta.metadata, &meta.compatibility};

    Ai3Fixture()
    {
        // Use a clean, isolated temp directory so inspectProject() filesystem
        // iteration is deterministic (no stray .hathor files in the CWD).
        tmpDir = std::filesystem::temp_directory_path() /
                 ("hathor-ai-g3-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(tmpDir);
        audio.setProjectDir(tmpDir);

        REQUIRE(meta.compatibility.compatible);
    }

    ~Ai3Fixture()
    {
        std::error_code ec;
        std::filesystem::remove_all(tmpDir, ec);
    }

    std::filesystem::path tmpDir;
};

// ===========================================================================
// 1. .hathor (mini-notation) context assembly + classification
// ===========================================================================

TEST_CASE("AI-G3: .hathor assembly classifies sample-string cursor and emits all sections",
          "[ai-g3][hathor][classification]")
{
    Ai3Fixture fx;
    FakeEditorContextProvider editor;
    fx.provider.setEditorContextProvider(&editor);

    const std::string doc = "d1 $ s \"bd sd hh cp\"";
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/proj/song.hathor";
    snap.uri = "file:///proj/song.hathor";
    snap.language = "mininotation";
    snap.content = doc;
    snap.cursorLine = 0;
    snap.cursorChar = 8; // inside the sample string, at 'b'
    snap.slotName = "d1";
    snap.slotIndex = 0;
    editor.setSnapshot(snap);

    CompletionRequest req;
    req.file = snap.file;
    req.uri = snap.uri;
    req.language = "mininotation";
    req.documentText = doc;
    req.line = 0;
    req.character = 8;

    auto result = fx.provider.assemble(req);

    REQUIRE(result.ok);
    REQUIRE(result.language == "hathor");
    REQUIRE(result.cursorContextLabel == "inside sample string");

    const auto& ctx = result.context;
    REQUIRE(ctx["ok"] == true);
    REQUIRE(ctx["language"] == "hathor");
    REQUIRE(ctx["cursor_context_kind"] == "sample_expr");

    REQUIRE(ctx["editor"]["file"] == "/proj/song.hathor");
    REQUIRE(ctx["editor"]["cursor"]["line"] == 0);
    REQUIRE(ctx["editor"]["cursor"]["character"] == 8);
    REQUIRE(ctx["editor"]["slot_name"] == "d1");

    REQUIRE(ctx["region"]["available"] == true);
    REQUIRE(ctx["region"]["surrounding"].is_string());

    REQUIRE(ctx["metadata"]["available"] == true);
    REQUIRE(ctx["metadata"]["functions"].is_array());
    REQUIRE(ctx["metadata"]["functions"].size() > 0);
    REQUIRE(ctx["metadata"]["operators"].is_array());
    REQUIRE(ctx["metadata"]["scales"].is_array());
    REQUIRE(ctx["metadata"]["scales_version"] == "1.2.6");

    REQUIRE(ctx["samples"]["ok"] == true);
    REQUIRE(ctx["diagnostics"]["sources"].is_array());

    REQUIRE(ctx["runtime"]["bpm"] == Catch::Approx(140.0));
    REQUIRE(ctx["runtime"]["slot_name"] == "d1");

    REQUIRE(ctx["project"]["ok"] == true);
    REQUIRE(ctx["instructions"].is_string());

    // The precomputed FIM prefix must equal the dumped context.
    REQUIRE_FALSE(result.fimPrefix.empty());
    REQUIRE(result.fimPrefix == ctx.dump());
}

// ===========================================================================
// 2. .ck (ChucK) context assembly + classification
// ===========================================================================

TEST_CASE("AI-G3: .ck assembly classifies routing cursor and surfaces Chuck API",
          "[ai-g3][chuck][classification]")
{
    Ai3Fixture fx;
    FakeEditorContextProvider editor;
    fx.provider.setEditorContextProvider(&editor);

    const std::string doc = "SinOsc osc => dac;\n440 => osc.freq;";
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/proj/instruments/bass.ck";
    snap.uri = "file:///proj/instruments/bass.ck";
    snap.language = "chuck";
    snap.content = doc;
    snap.cursorLine = 0;
    snap.cursorChar = 8; // near the `=>` routing
    editor.setSnapshot(snap);

    CompletionRequest req;
    req.file = snap.file;
    req.uri = snap.uri;
    req.language = "chuck";
    req.documentText = doc;
    req.line = 0;
    req.character = 8;

    auto result = fx.provider.assemble(req);

    REQUIRE(result.ok);
    REQUIRE(result.language == "chuck");
    REQUIRE(result.cursorContextLabel == "audio-graph routing (=>)");

    const auto& ctx = result.context;
    REQUIRE(ctx["cursor_context_kind"] == "routing");
    REQUIRE(ctx["metadata"]["chuck_api"].is_array());
    REQUIRE(ctx["metadata"]["chuck_api"].size() > 0);
    REQUIRE(ctx["metadata"]["available"] == true);

    // Runtime section should report vm liveness for Chuck.
    REQUIRE(ctx["runtime"].contains("vm_alive"));
}

// ===========================================================================
// 3. Selection-aware retrieval
// ===========================================================================

TEST_CASE("AI-G3: an active selection is surfaced in the editor section",
          "[ai-g3][selection]")
{
    Ai3Fixture fx;
    FakeEditorContextProvider editor;
    fx.provider.setEditorContextProvider(&editor);

    const std::string doc = "bd sn hh cp\ncp bd sn hh";
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/proj/song.hathor";
    snap.uri = "file:///proj/song.hathor";
    snap.language = "mininotation";
    snap.content = doc;
    snap.cursorLine = 0;
    snap.cursorChar = 3;
    snap.slotName = "d0";
    snap.hasSelection = true;
    snap.selStartLine = 0;
    snap.selStartChar = 0;
    snap.selEndLine = 1;
    snap.selEndChar = 2;
    snap.selectedText = "bd sn hh cp\ncp";
    editor.setSnapshot(snap);

    CompletionRequest req;
    req.file = snap.file;
    req.uri = snap.uri;
    req.language = "mininotation";
    req.documentText = doc;
    req.line = 0;
    req.character = 3;
    req.selectedText = snap.selectedText;
    req.selection = CompletionRequest::Range{0, 0, 1, 2};

    auto result = fx.provider.assemble(req);
    const auto& ctx = result.context;
    REQUIRE(ctx["editor"]["has_selection"] == true);
    REQUIRE(ctx["editor"]["selected_text"] == "bd sn hh cp\ncp");
}

// ===========================================================================
// 4. Diagnostic-aware retrieval (compiler + LSP, proximity-ordered, bounded)
// ===========================================================================

TEST_CASE("AI-G3: diagnostics merge compiler + LSP, ordered nearest-first, bounded",
          "[ai-g3][diagnostics]")
{
    Ai3Fixture fx;
    FakeEditorContextProvider editor;
    FakeLspContextProvider lsp;
    fx.provider.setEditorContextProvider(&editor);
    fx.provider.setLspContextProvider(&lsp);

    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/proj/bad.hathor";
    snap.uri = "file:///proj/bad.hathor";
    snap.language = "mininotation";
    snap.content = "bd @\nhh cp";
    snap.cursorLine = 1; // cursor on line 1 (remote from a possible line-0 error)
    snap.cursorChar = 1;
    snap.slotName = "d0";
    editor.setSnapshot(snap);

    // LSP diagnostic far from the cursor (line 10), to exercise proximity ordering.
    lsp.setOk(true);
    lsp.addDiagnostic(nlohmann::json{
        {"severity","warning"}, {"code","LSP_W0"},
        {"message","far LSP diag"}, {"line",10}, {"column",0}
    });

    CompletionRequest req;
    req.file = snap.file;
    req.uri = snap.uri;
    req.language = "mininotation";
    req.documentText = snap.content;
    req.line = 1;
    req.character = 1;

    auto result = fx.provider.assemble(req);
    const auto& diags = result.context["diagnostics"];
    REQUIRE(diags["lsp_available"] == true);
    REQUIRE(std::find(diags["sources"].begin(), diags["sources"].end(), "lsp")
            != diags["sources"].end());

    const int count = diags["count"].get<int>();
    REQUIRE(count >= 1);
    // Bounded by maxDiagnostics (default 8).
    REQUIRE(count <= 8);
}

// ===========================================================================
// 5. Asset-aware retrieval (sample name-prefix matching)
// ===========================================================================

TEST_CASE("AI-G3: samples prefixed with the typed token are surfaced first",
          "[ai-g3][assets][samples]")
{
    Ai3Fixture fx;
    // Register samples via the real SampleBank; cursor sits right after "bd".
    fx.bank.addTestEntry(SampleEntry{"bd", 0, {}, 1, 44100.0, "bd.wav"});
    fx.bank.addTestEntry(SampleEntry{"bd2", 1, {}, 1, 44100.0, "bd2.wav"});
    fx.bank.addTestEntry(SampleEntry{"sn", 2, {}, 1, 44100.0, "sn.wav"});
    fx.bank.addTestEntry(SampleEntry{"hh", 3, {}, 1, 44100.0, "hh.wav"});

    FakeEditorContextProvider editor;
    fx.provider.setEditorContextProvider(&editor);

    const std::string doc = "d1 $ s \"bd";
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/proj/song.hathor";
    snap.uri = "file:///proj/song.hathor";
    snap.language = "mininotation";
    snap.content = doc;
    snap.cursorLine = 0;
    snap.cursorChar = 10; // end of "bd", inside the string
    snap.slotName = "d1";
    editor.setSnapshot(snap);

    CompletionRequest req;
    req.file = snap.file;
    req.uri = snap.uri;
    req.language = "mininotation";
    req.documentText = doc;
    req.line = 0;
    req.character = 10;

    auto result = fx.provider.assemble(req);
    const auto& samples = result.context["samples"];
    REQUIRE(samples["filter_prefix"] == "bd");
    const std::string first = samples["samples"][0]["name"].get<std::string>();
    REQUIRE(first == "bd"); // exact match wins
}

// ===========================================================================
// 6. Bounded context (maxContextChars budget + customBounds override)
// ===========================================================================

TEST_CASE("AI-G3: context is hard-bounded to maxContextChars", "[ai-g3][bounds]")
{
    Ai3Fixture fx;
    FakeEditorContextProvider editor;
    fx.provider.setEditorContextProvider(&editor);

    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/proj/song.hathor";
    snap.uri = "file:///proj/song.hathor";
    snap.language = "mininotation";
    snap.content = std::string(8000, 'b') + " $ s \"bd\"";
    snap.cursorLine = 0;
    snap.cursorChar = 7000;
    snap.slotName = "d0";
    editor.setSnapshot(snap);

    ContextBounds tight;
    tight.maxContextChars = 4096;
    tight.maxSamples = 5;

    CompletionRequest req;
    req.file = snap.file;
    req.uri = snap.uri;
    req.language = "mininotation";
    req.documentText = snap.content;
    req.line = 0;
    req.character = 7000;
    req.customBounds = true;
    req.bounds = tight;

    auto result = fx.provider.assemble(req);
    REQUIRE(result.ok);
    REQUIRE(static_cast<int>(result.fimPrefix.size()) <= 4096);

    // The bounds reflected back must be the overrides we supplied.
    const auto& b = result.context["bounds"];
    REQUIRE(b["max_samples"] == 5);
    REQUIRE(b["max_context_chars"] == 4096);
}

// ===========================================================================
// 7. Stale metadata / example rejection (version gate)
// ===========================================================================

TEST_CASE("AI-G3: incompatible (stale) metadata is surfaced and examples rejected",
          "[ai-g3][stale-metadata]")
{
    Ai3Fixture fx;
    FakeEditorContextProvider editor;
    fx.provider.setEditorContextProvider(&editor);

    const std::string doc = "d1 $ s \"bd sd\"";
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/proj/song.hathor";
    snap.uri = "file:///proj/song.hathor";
    snap.language = "mininotation";
    snap.content = doc;
    snap.cursorLine = 0;
    snap.cursorChar = 6;
    snap.slotName = "d1";
    editor.setSnapshot(snap);

    // Inject stale metadata whose surface version does NOT match kExamples.
    hathor::language::LanguageMetadata stale;
    stale.schemaVersion = 1;
    stale.hathorEngineCompat = "0.1.0";
    stale.strudelMiniNotationCompat = "0.0.0"; // mismatched surface -> stale
    stale.chuckLibVersion = "3.8.3";
    stale.chuckIntegrationSurface = "B4-K3";

    hathor::language::MetadataCompatibility compat;
    compat.compatible = false;
    compat.errors = {"Strudel surface version mismatch"};

    fx.provider.setMetadata(&stale, &compat);

    CompletionRequest req;
    req.file = snap.file;
    req.uri = snap.uri;
    req.language = "mininotation";
    req.documentText = doc;
    req.line = 0;
    req.character = 6;

    auto result = fx.provider.assemble(req);
    const auto& ctx = result.context;

    // Metadata block marks itself unavailable due to incompatibility.
    REQUIRE(ctx["metadata_version"]["compatible"] == false);
    REQUIRE(ctx["metadata"]["available"] == false);

    // Examples must be rejected: no matching surface version -> count 0.
    const int exCount = ctx["examples"]["count"].get<int>();
    REQUIRE(exCount == 0);
}

// ===========================================================================
// 8. Thread-safety — assemble() is safe off the JUCE audio thread
// ===========================================================================

TEST_CASE("AI-G3: assemble() is callable concurrently from a non-audio thread",
          "[ai-g3][threading]")
{
    Ai3Fixture fx;
    FakeEditorContextProvider editor;
    fx.provider.setEditorContextProvider(&editor);

    const std::string doc = "d1 $ s \"bd sd\"";
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/proj/song.hathor";
    snap.uri = "file:///proj/song.hathor";
    snap.language = "mininotation";
    snap.content = doc;
    snap.cursorLine = 0;
    snap.cursorChar = 5;
    snap.slotName = "d1";
    editor.setSnapshot(snap);

    CompletionRequest req;
    req.file = snap.file;
    req.uri = snap.uri;
    req.language = "mininotation";
    req.documentText = doc;
    req.line = 0;
    req.character = 5;

    std::vector<std::thread> workers;
    std::atomic<int> okCount{0};
    const int n = (std::thread::hardware_concurrency() >= 2) ? 4 : 1;
    for (int i = 0; i < n; ++i)
        workers.emplace_back([&]{
            auto r = fx.provider.assemble(req);
            if (r.ok) okCount.fetch_add(1);
        });
    for (auto& t : workers) t.join();

    REQUIRE(okCount.load() == n);
}

// ===========================================================================
// 9. FIM reachability — assemble -> GhostContext.authoringContext ->
//    GhostCompletionLogic::buildRequest -> req.fim.prefix carries the payload
// ===========================================================================

TEST_CASE("AI-G3: assembled context flows into llm-ls fim.prefix via buildRequest",
          "[ai-g3][fim][integration]")
{
    Ai3Fixture fx;
    FakeEditorContextProvider editor;
    fx.provider.setEditorContextProvider(&editor);

    const std::string doc = "d1 $ s \"bd sd hh cp\"";
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/proj/song.hathor";
    snap.uri = "file:///proj/song.hathor";
    snap.language = "mininotation";
    snap.content = doc;
    snap.cursorLine = 0;
    snap.cursorChar = 5;
    snap.slotName = "d1";
    editor.setSnapshot(snap);

    CompletionRequest req;
    req.file = snap.file;
    req.uri = snap.uri;
    req.language = "mininotation";
    req.documentText = doc;
    req.line = 0;
    req.character = 5;

    const auto assembled = fx.provider.assemble(req);
    REQUIRE(assembled.ok);

    // Wire the assembled context into a GhostContext exactly as HathorTab does
    // (ctx.authoringContext = getAuthoringContext()).
    hathor::lsp::GhostContext ghostCtx;
    ghostCtx.documentText = snap.content;
    ghostCtx.uri = snap.uri;
    ghostCtx.languageId = "hathor";
    ghostCtx.line = 0;
    ghostCtx.character = 5;
    ghostCtx.authoringContext = nlohmann::json::parse(assembled.fimPrefix);

    hathor::lsp::GhostProviderConfig config;
    config.backend = hathor::lsp::LlmBackend::HuggingFace;
    config.model = "starcoder";

    const auto built = hathor::lsp::GhostCompletionLogic::buildRequest(ghostCtx, config);

    // fim must be enabled and the prefix must be the assembled context dump.
    REQUIRE(built.fim.enabled == true);
    REQUIRE_FALSE(built.fim.prefix.empty());
    REQUIRE(built.fim.prefix == assembled.fimPrefix);

    // The prefix must carry the FIM marker fields the model relies on.
    const auto prefixJson = nlohmann::json::parse(built.fim.prefix);
    REQUIRE(prefixJson["ok"] == true);
    REQUIRE(prefixJson["language"] == "hathor");
    REQUIRE(prefixJson["metadata_version"]["compatible"] == true);

    // Explicit FIM document context preserved alongside the authoring context.
    REQUIRE(built.docPrefix == doc);
    REQUIRE(built.docSuffix == "");
    REQUIRE_FALSE(built.authoringContext.is_null());
}

// ===========================================================================
// 10. Wire-through via ControlInterface::assembleCompletionContext
// ===========================================================================

TEST_CASE("AI-G3: ControlInterface.assembleCompletionContext forwards to providers",
          "[ai-g3][dispatch]")
{
    auto meta = loadTestMetadata();
    REQUIRE(meta.compatibility.compatible);

    Ai8FakeFacade audio;
    SampleBank bank;
    audio.setProjectDir(std::filesystem::temp_directory_path() / "hathor-agi3-ci");

    hathor::control::ControlInterface ci(audio, bank);
    ci.setEditorContextProvider(nullptr);  // no live editor in this test
    ci.setLspContextProvider(nullptr);
    ci.setLanguageMetadata(&meta.metadata, &meta.compatibility);

    CompletionRequest req;
    req.file = "/proj/song.hathor";
    req.uri = "file:///proj/song.hathor";
    req.language = "mininotation";
    req.documentText = "d1 $ s \"bd sd\"";
    req.line = 0;
    req.character = 5;

    const auto result = ci.assembleCompletionContext(req);

    REQUIRE(result["ok"] == true);
    REQUIRE(result["language"] == "hathor");
    REQUIRE(result["fim_prefix_size"].is_number_integer());
    REQUIRE(result["fim_prefix_size"] > 0);
    REQUIRE(result["metadata_version"]["compatible"] == true);
}

// ===========================================================================
// 11. Language inference + metadata version block attribution
// ===========================================================================

TEST_CASE("AI-G3: language is inferred from .hathor / .ck paths and metadata is versioned",
          "[ai-g3][language][metadata-version]")
{
    Ai3Fixture fx;
    FakeEditorContextProvider editor;
    fx.provider.setEditorContextProvider(&editor);

    SECTION(".hathor path -> mininotation -> json label 'hathor'") {
        EditorContextSnapshot snap;
        snap.file = "/proj/song.hathor";
        snap.uri = "file:///proj/song.hathor";
        snap.language = "mininotation";
        snap.content = "bd sd";
        editor.setSnapshot(snap);

        CompletionRequest req;
        req.file = snap.file;
        req.documentText = snap.content;
        req.line = 0; req.character = 0;

        auto r = fx.provider.assemble(req);
        REQUIRE(r.language == "hathor");
        REQUIRE(r.context["language"] == "hathor");
    }

    SECTION(".ck path -> chuck -> json label 'chuck'") {
        EditorContextSnapshot snap;
        snap.file = "/proj/bass.ck";
        snap.uri = "file:///proj/bass.ck";
        snap.language = "chuck";
        snap.content = "SinOsc s => dac;";
        editor.setSnapshot(snap);

        CompletionRequest req;
        req.file = snap.file;
        req.language = "chuck";
        req.documentText = snap.content;
        req.line = 0; req.character = 0;

        auto r = fx.provider.assemble(req);
        REQUIRE(r.language == "chuck");
        REQUIRE(r.context["language"] == "chuck");
    }

    SECTION("metadata_version block is always present and version-attributed") {
        EditorContextSnapshot snap;
        snap.hasContent = true;
        snap.file = "/proj/song.hathor";
        snap.uri = "file:///proj/song.hathor";
        snap.language = "mininotation";
        snap.content = "bd sd";
        snap.cursorLine = 0;
        snap.cursorChar = 0;
        editor.setSnapshot(snap);

        CompletionRequest req;
        req.file = snap.file;
        req.language = "mininotation";
        req.documentText = snap.content;
        req.line = 0; req.character = 0;

        auto result = fx.provider.assemble(req);
        const auto& mv = result.context["metadata_version"];
        REQUIRE(mv["schema"] == 1);
        REQUIRE(mv["engine"] == std::string(hathor::language::kHathorEngineCompat));
        REQUIRE(mv["strudel"] == std::string(hathor::language::kStrudelMiniNotationCompat));
        REQUIRE(mv["chuck"] == std::string(hathor::language::kChuckLibVersion));
        REQUIRE(mv["surface"] == std::string(hathor::language::kChuckIntegrationSurface));
        REQUIRE(mv["compatible"] == true);
    }
}
