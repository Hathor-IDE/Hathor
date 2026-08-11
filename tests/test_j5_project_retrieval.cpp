// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_j5_project_retrieval.cpp — J-5 project-aware symbol indexing + retrieval tests.
 *
 * Verifies (JUCE-free, compiled into hathor-control-tests):
 *   1. ProjectSymbolIndex scans .hathor and .ck files, extracts symbols
 *   2. Version token changes on reindex with different files
 *   3. lookupSymbol finds exact matches, bounded by language
 *   4. searchByPrefix finds prefix matches
 *   5. ProjectRetrievalContext retrieve() returns bounded, ranked snippets
 *   6. Empty index returns ok=false
 *   7. Bounds are respected (maxSnippets, maxTotalChars)
 *   8. CompletionContextProvider assembles project_retrieval section
 *   9. AuthoringContext assembles project_retrieval section
 *  10. Thread-safety — safe to call off the JUCE audio thread
 *
 * Requirement references: J-5, AI-G3, AI-8
 */

#include "test_ai_fakes.hpp"

#include "CompletionContextProvider.hpp"
#include "ControlInterface.hpp"
#include "ProjectReadFacade.hpp"
#include "AuthoringContext.hpp"

#include "hathor/ProjectSymbolIndex.hpp"
#include "ProjectRetrievalContext.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using hathor::language::ProjectSymbolIndex;
using hathor::language::IndexedSymbol;
using hathor::language::IndexedFile;
using hathor::control::ProjectRetrievalContext;
using hathor::control::RetrievalContext;
using hathor::control::RetrievalBounds;
using hathor::control::CompletionContextProvider;
using hathor::control::CompletionRequest;
using hathor::control::ContextBounds;
using hathor::control::AuthoringContext;
using hathor::control::ContextRequest;

// ---------------------------------------------------------------------------
// Test fixture: creates a fake project directory with .hathor and .ck files
// ---------------------------------------------------------------------------

class J5TestProject {
public:
    explicit J5TestProject(const std::string& name)
    {
        dir_ = std::filesystem::temp_directory_path() / "hathor-j5-test" / name;
        std::filesystem::create_directories(dir_);

        // .hathor file with sample refs and pattern slots
        writeFile("main.hathor",
            "// Main pattern\n"
            "d1 $ s \"bd sn hh cp\"\n"
            "d2 $ s \"~ <bd cp>\"\n"
            "d3 $ s \"arpy (bd*2) ~ sn\"\n"
            "// Custom sample reference\n"
            "d4 $ s \"myKick\"\n");

        // .ck file with UGen declarations and routing
        writeFile("synth.ck",
            "// Simple synth\n"
            "SinOsc osc => LPF filt => dac;\n"
            "osc.freq(440);\n"
            "filt.cofreq(2000);\n"
            "// Class definition\n"
            "class MySynth extends Chubgraph {\n"
            "    SinOsc osc => outlet;\n"
            "}\n");

        // Second .hathor file
        writeFile("basses.hathor",
            "// Bass patterns\n"
            "d1 $ s \"bd*2\"\n"
            "d2 $ s \"bass1\"\n");

        // Second .ck file
        writeFile("effects.ck",
            "// Reverb effect\n"
            "JCRev rev => dac;\n"
            "rev.gain(0.3);\n");
    }

    ~J5TestProject()
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path dir_;

    std::string path() const { return dir_.string(); }

private:
    void writeFile(const std::string& name, const std::string& content)
    {
        std::ofstream f(dir_ / name);
        f << content;
    }
};

// ---------------------------------------------------------------------------
// 1. ProjectSymbolIndex basic indexing
// ---------------------------------------------------------------------------

TEST_CASE("J-5: ProjectSymbolIndex scans .hathor and .ck files", "[j5]")
{
    J5TestProject proj("scan_test");
    ProjectSymbolIndex index;

    index.reindex(proj.path());

    REQUIRE(index.symbolCount() > 0);
    REQUIRE(index.fileCount() >= 4);  // 2 .hathor + 2 .ck
    REQUIRE_FALSE(index.empty());
    REQUIRE_FALSE(index.versionToken().empty());
}

// ---------------------------------------------------------------------------
// 2. Version token reflects file changes
// ---------------------------------------------------------------------------

TEST_CASE("J-5: version token changes on reindex", "[j5]")
{
    J5TestProject proj("version_test");
    ProjectSymbolIndex index;

    index.reindex(proj.path());
    const std::string token1 = index.versionToken();
    REQUIRE_FALSE(token1.empty());

    // Re-index same files — token should be the same
    index.reindex(proj.path());
    const std::string token2 = index.versionToken();
    REQUIRE(token2 == token1);

    // Add a new file — token should change
    {
        std::ofstream f(proj.dir_ / "newpattern.hathor");
        f << "d1 $ s \"cp bd\"\n";
    }

    index.reindex(proj.dir_.string());
    const std::string token3 = index.versionToken();
    REQUIRE(token3 != token1);
}

// ---------------------------------------------------------------------------
// 3. lookupSymbol finds exact matches, bounded by language
// ---------------------------------------------------------------------------

TEST_CASE("J-5: lookupSymbol finds exact matches", "[j5]")
{
    J5TestProject proj("lookup_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    // Look up "bd" — should find SampleRef entries
    auto syms = index.lookupSymbol("bd");
    REQUIRE_FALSE(syms.empty());

    bool foundSample = false;
    for (const auto& s : syms) {
        if (s.kind == hathor::language::SymbolKind::SampleRef)
            foundSample = true;
    }
    REQUIRE(foundSample);

    // Look up "SinOsc" — should find UgenInstantiation
    syms = index.lookupSymbol("SinOsc");
    REQUIRE_FALSE(syms.empty());
    bool foundUgen = false;
    for (const auto& s : syms) {
        if (s.kind == hathor::language::SymbolKind::UgenInstantiation)
            foundUgen = true;
    }
    REQUIRE(foundUgen);

    // Language filtering
    syms = index.lookupSymbol("bd", "mininotation");
    REQUIRE_FALSE(syms.empty());
    for (const auto& s : syms)
        REQUIRE(s.language == "mininotation");

    // No matches for non-existent symbol
    syms = index.lookupSymbol("nonexistent_symbol_xyz");
    REQUIRE(syms.empty());
}

// ---------------------------------------------------------------------------
// 4. searchByPrefix finds prefix matches
// ---------------------------------------------------------------------------

TEST_CASE("J-5: searchByPrefix finds prefix matches", "[j5]")
{
    J5TestProject proj("prefix_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    // "my" prefix should find "myKick"
    auto syms = index.searchByPrefix("my");
    REQUIRE_FALSE(syms.empty());
    bool foundMyKick = false;
    for (const auto& s : syms) {
        if (s.name == "myKick")
            foundMyKick = true;
    }
    REQUIRE(foundMyKick);

    // "bas" prefix should find "bass1"
    syms = index.searchByPrefix("bas", "mininotation");
    bool foundBass = false;
    for (const auto& s : syms) {
        if (s.name == "bass1")
            foundBass = true;
    }
    REQUIRE(foundBass);
}

// ---------------------------------------------------------------------------
// 5. ProjectRetrievalContext retrieve() returns bounded, ranked snippets
// ---------------------------------------------------------------------------

TEST_CASE("J-5: ProjectRetrievalContext retrieve() returns bounded snippets", "[j5]")
{
    J5TestProject proj("retrieve_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    ProjectRetrievalContext retriever(&index);

    RetrievalContext ctx;
    ctx.language = "mininotation";
    ctx.currentFile = proj.path() + "/main.hathor";
    ctx.typedText = "s \"";
    ctx.cursorContextKind = "sample_expr";
    ctx.cursorContextLabel = "inside sample string";

    auto result = retriever.retrieve(ctx);

    REQUIRE(result["ok"] == true);
    REQUIRE(result.contains("snippets"));
    REQUIRE(result["snippets"].is_array());
    REQUIRE_FALSE(result["version_token"].empty());
    REQUIRE(result["count"] >= 0);
}

// ---------------------------------------------------------------------------
// 6. Empty index returns ok=false
// ---------------------------------------------------------------------------

TEST_CASE("J-5: empty index returns ok=false", "[j5]")
{
    ProjectSymbolIndex index;
    ProjectRetrievalContext retriever(&index);

    RetrievalContext ctx;
    ctx.language = "mininotation";
    ctx.typedText = "bd";
    ctx.cursorContextKind = "sample_expr";

    auto result = retriever.retrieve(ctx);

    REQUIRE(result["ok"] == false);
    REQUIRE(result["snippets"].empty());
}

// ---------------------------------------------------------------------------
// 7. Bounds are respected
// ---------------------------------------------------------------------------

TEST_CASE("J-5: retrieval bounds are respected", "[j5]")
{
    J5TestProject proj("bounds_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    ProjectRetrievalContext retriever(&index);

    RetrievalContext ctx;
    ctx.language = "mininotation";
    ctx.typedText = "s";  // short prefix, should match many

    RetrievalBounds bounds;
    bounds.maxSnippets = 3;
    bounds.maxSnippetChars = 100;
    bounds.maxTotalChars = 300;

    auto result = retriever.retrieve(ctx, bounds);

    REQUIRE(result["ok"] == true);
    REQUIRE(result["snippets"].size() <= 3);
    REQUIRE(result["count"] <= 3);

    // Check total snippet text chars don't exceed maxTotalChars
    int totalSnippetChars = 0;
    for (const auto& s : result["snippets"])
        totalSnippetChars += s["snippet"].get<std::string>().size();
    REQUIRE(totalSnippetChars <= bounds.maxTotalChars);
}

// ---------------------------------------------------------------------------
// 8. CompletionContextProvider assembles project_retrieval section
// ---------------------------------------------------------------------------

TEST_CASE("J-5: CompletionContextProvider assembles project_retrieval", "[j5]")
{
    J5TestProject proj("completion_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    // Use the fakes from test_ai_fakes.hpp
    auto facade = std::make_unique<Ai8FakeFacade>();
    facade->setProjectDir(proj.path());
    facade->addSampleName("bd");
    facade->addSampleName("sn");

    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(*facade, bank);

    // Load metadata
    auto metaResult = loadTestMetadata();

    CompletionContextProvider provider(readFacade, nullptr, nullptr,
                                       &metaResult.metadata, &metaResult.compatibility);
    provider.setProjectSymbolIndex(&index);

    // Use generous bounds to avoid truncation with full real metadata.
    provider.setBounds(ContextBounds{
        .maxSamples = 200,
        .maxInstruments = 100,
        .maxRegionLines = 5,
        .maxSurroundingChars = 512,
        .maxContextChars = 65536,
        .maxProjectSnippets = 5,
        .maxProjectSnippetChars = 200,
        .maxProjectRetrievalChars = 2048,
    });

    CompletionRequest req;
    req.file = proj.path() + "/main.hathor";
    req.line = 0;
    req.character = 5;
    req.language = "mininotation";
    req.documentText = "d1 $ s \"bd sn hh cp\"\n";

    auto ctx = provider.assemble(req);

    nlohmann::json prefixJson = nlohmann::json::parse(ctx.fimPrefix);
    REQUIRE(prefixJson.contains("project_retrieval"));
    REQUIRE(prefixJson["project_retrieval"]["ok"] == true);
    REQUIRE(prefixJson["project_retrieval"]["available"] == true);
    REQUIRE(prefixJson["project_retrieval"].contains("snippets"));
    REQUIRE(prefixJson["project_retrieval"].contains("version_token"));
    REQUIRE_FALSE(prefixJson["project_retrieval"]["version_token"].empty());
}

// ---------------------------------------------------------------------------
// 9. CompletionContextProvider without index returns ok=false
// ---------------------------------------------------------------------------

TEST_CASE("J-5: CompletionContextProvider without index reports unavailable", "[j5]")
{
    auto facade = std::make_unique<Ai8FakeFacade>();
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(*facade, bank);

    auto metaResult = loadTestMetadata();

    CompletionContextProvider provider(readFacade, nullptr, nullptr,
                                       &metaResult.metadata, &metaResult.compatibility);

    // Use generous bounds to avoid truncation with full real metadata.
    provider.setBounds(ContextBounds{
        .maxSamples = 200,
        .maxInstruments = 100,
        .maxRegionLines = 5,
        .maxSurroundingChars = 512,
        .maxContextChars = 65536,
        .maxProjectSnippets = 5,
        .maxProjectSnippetChars = 200,
        .maxProjectRetrievalChars = 2048,
    });

    CompletionRequest req;
    req.file = "/tmp/test.hathor";
    req.line = 0;
    req.character = 0;
    req.documentText = "d1 $ s \"bd\"\n";

    auto ctx = provider.assemble(req);
    nlohmann::json prefixJson = nlohmann::json::parse(ctx.fimPrefix);

    REQUIRE(prefixJson["project_retrieval"]["ok"] == false);
    REQUIRE(prefixJson["project_retrieval"]["available"] == false);
}

// ---------------------------------------------------------------------------
// 9b. AuthoringContext assembles project_retrieval section
// ---------------------------------------------------------------------------

TEST_CASE("J-5: AuthoringContext assembles project_retrieval", "[j5]")
{
    J5TestProject proj("authoring_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    auto facade = std::make_unique<Ai8FakeFacade>();
    facade->setProjectDir(proj.path());

    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(*facade, bank);

    auto metaResult = loadTestMetadata();

    AuthoringContext author(readFacade, nullptr, nullptr,
                            &metaResult.metadata, &metaResult.compatibility);
    author.setProjectSymbolIndex(&index);

    ContextRequest req;
    req.file = proj.path() + "/synth.ck";
    req.language = "chuck";

    auto response = author.assemble(req);

    REQUIRE(response["ok"] == true);
    REQUIRE(response["sections"].contains("project_retrieval"));
    REQUIRE(response["sections"]["project_retrieval"]["ok"] == true);
}

// ---------------------------------------------------------------------------
// 10. Thread-safety — safe to call off the audio thread
// ---------------------------------------------------------------------------

TEST_CASE("J-5: thread-safety — concurrent retrieve + reindex", "[j5]")
{
    J5TestProject proj("thread_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    // Verify retrieve is callable from a worker thread
    std::atomic<bool> workerDone{false};
    std::atomic<bool> workerOk{false};

    std::thread worker([&]() {
        ProjectRetrievalContext retriever(&index);
        RetrievalContext ctx;
        ctx.language = "mininotation";
        ctx.typedText = "s";

        auto result = retriever.retrieve(ctx);
        workerOk = result["ok"].get<bool>();
        workerDone = true;
    });

    // Concurrently reindex from the main thread
    index.reindex(proj.path());

    while (!workerDone.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    worker.join();

    REQUIRE(workerOk.load());
}

// ---------------------------------------------------------------------------
// 11. searchByContent finds symbols by snippet text
// ---------------------------------------------------------------------------

TEST_CASE("J-5: searchByContent finds symbols by snippet text", "[j5]")
{
    J5TestProject proj("content_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    // Search for "SinOsc" in content — should find it
    auto syms = index.searchByContent("SinOsc");
    REQUIRE_FALSE(syms.empty());

    bool found = false;
    for (const auto& s : syms) {
        if (s.name == "SinOsc")
            found = true;
    }
    REQUIRE(found);
}

// ---------------------------------------------------------------------------
// 12. listFiles returns file metadata
// ---------------------------------------------------------------------------

TEST_CASE("J-5: listFiles returns file metadata", "[j5]")
{
    J5TestProject proj("files_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    auto files = index.listFiles("mininotation");
    REQUIRE_FALSE(files.empty());

    bool foundHathor = false;
    for (const auto& f : files) {
        if (f.path.ends_with(".hathor"))
            foundHathor = true;
    }
    REQUIRE(foundHathor);

    // All returned files should be mininotation language
    for (const auto& f : files)
        REQUIRE(f.language == "mininotation");

    // Chuck files
    auto ckFiles = index.listFiles("chuck");
    REQUIRE_FALSE(ckFiles.empty());
    for (const auto& f : ckFiles)
        REQUIRE(f.language == "chuck");
}

// ---------------------------------------------------------------------------
// 13. maybeReindex returns false when files unchanged
// ---------------------------------------------------------------------------

TEST_CASE("J-5: maybeReindex returns false when unchanged", "[j5]")
{
    J5TestProject proj("maybe_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    bool changed = index.maybeReindex(proj.path());
    REQUIRE_FALSE(changed);

    // Touch a file — should detect change
    const auto filePath = proj.dir_ / "main.hathor";
    std::ofstream f(filePath, std::ios::app);
    f << "// new comment\n";
    f.close();

    // Update mtime
    auto now = std::filesystem::file_time_type::clock::now();
    std::filesystem::last_write_time(filePath, now);

    changed = index.maybeReindex(proj.path());
    REQUIRE(changed);
}

// ---------------------------------------------------------------------------
// 14. Relevance scoring — language match boosts score
// ---------------------------------------------------------------------------

TEST_CASE("J-5: relevance scoring prefers language-matching symbols", "[j5]")
{
    J5TestProject proj("ranking_test");
    ProjectSymbolIndex index;
    index.reindex(proj.path());

    ProjectRetrievalContext retriever(&index);

    RetrievalContext ctx;
    ctx.language = "mininotation";
    ctx.typedText = "bd";  // "bd" appears in .hathor files

    auto result = retriever.retrieve(ctx);

    REQUIRE(result["ok"] == true);
    REQUIRE(result["snippets"].size() > 0);

    // All returned snippets should be mininotation (language match)
    for (const auto& s : result["snippets"]) {
        REQUIRE(s["language"] == "mininotation");
        REQUIRE(s["relevance_score"].is_number());
        REQUIRE(s["relevance_score"].get<double>() >= 0.0);
        REQUIRE(s["relevance_score"].get<double>() <= 1.0);
    }
}
