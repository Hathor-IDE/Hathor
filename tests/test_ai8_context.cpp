// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ai8_context.cpp — AI-8 dynamic authoring context assembly tests.
 *
 * Verifies:
 *   1. Context assembly for .hathor (mini-notation) files
 *   2. Context assembly for .ck (ChucK) files
 *   3. Cursor/selection changes reflected in context
 *   4. Project edits — no stale context (metadata version tracking)
 *   5. Changed BPM, samples, instruments in context
 *   6. LSP-derived diagnostics in context
 *   7. Hathor-specific metadata injection + version identification
 *   8. Missing/incompatible LSP or metadata — graceful degradation
 *   9. Targeted context (not the whole project)
 *  10. No mutations through AI-8 context assembly
 *
 * Architecture: JUCE-free tests using fake providers injected into
 * AuthoringContext. Metadata is loaded from the canonical JSON file.
 *
 * Requirement references: AI-8
 */

#include "AuthoringContext.hpp"
#include "ControlInterface.hpp"
#include "ProjectReadFacade.hpp"
#include "EditorContextProvider.hpp"
#include "LspContextProvider.hpp"

#include "AudioEngineFacade.hpp"
#include "SlotState.hpp"
#include "MasterEq.hpp"
#include "AssetTarget.hpp"
#include "ChuckRenderWriter.hpp"
#include "SampleBank.hpp"
#include "MasterEq.hpp"
#include "AssetTarget.hpp"
#include "ChuckRenderWriter.hpp"

#include "hathor/LanguageMetadata.hpp"
#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef LANGUAGE_METADATA_DIR
#error "LANGUAGE_METADATA_DIR compile definition is not set. Check CMakeLists.txt."
#endif

using hathor::control::AuthoringContext;
using hathor::control::ContextRequest;
using hathor::control::EditorContextProvider;
using hathor::control::EditorContextSnapshot;
using hathor::control::LspContextProvider;

// ---------------------------------------------------------------------------
// Fake AudioEngineFacade (JUCE-free stand-in)
// ---------------------------------------------------------------------------

class Ai8FakeFacade final : public AudioEngineFacade {
public:
    void play() noexcept override                       { running_ = true; }
    void stop() noexcept override                         { running_ = false; }
    void setBpm(double b) noexcept override               { bpm_ = b; }
    double getBpm() const noexcept override               { return bpm_; }
    bool isRunning() const noexcept override              { return running_; }
    void slotPlay(int s) noexcept override                         { slotRunning_[s] = true; }
    void slotStop(int s) noexcept override                         { slotRunning_[s] = false; }
    bool isSlotRunning(int s) const noexcept override              { return slotRunning_[s]; }
    void setMasterGain(float g) noexcept override                  { gain_ = g; }
    float getMasterGain() const noexcept override                  { return gain_; }
    void setMasterEqPreset(hathor::EqPreset) noexcept override     {}
    hathor::EqPreset getMasterEqPreset() const noexcept override   { return hathor::EqPreset::Flat; }

    int findOrAddSlot(const std::string& name) override {
        for (size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == name) return static_cast<int>(i);
        if (names_.size() < 16) {
            names_.push_back(name);
            states_.emplace_back();
            slotRunning_.push_back(false);
            return static_cast<int>(names_.size()) - 1;
        }
        return -1;
    }
    void storeSlot(int idx, std::shared_ptr<SlotState> state) noexcept override {
        if (idx >= 0 && static_cast<size_t>(idx) < states_.size())
            states_[idx] = std::move(state);
    }
    bool clearSlot(int idx) noexcept override {
        if (idx >= 0 && static_cast<size_t>(idx) < states_.size()) {
            states_[idx].reset();
            return true;
        }
        return false;
    }
    int slotCount() const noexcept override               { return static_cast<int>(names_.size()); }
    std::string slotName(int idx) const override {
        if (idx >= 0 && static_cast<size_t>(idx) < names_.size())
            return names_[idx];
        return {};
    }
    std::shared_ptr<SlotState> loadSlot(int idx) const noexcept override {
        if (idx >= 0 && static_cast<size_t>(idx) < states_.size())
            return states_[idx];
        return nullptr;
    }

    bool hasWorker() const noexcept override              { return false; }
    bool ckEval(int, const std::string&) noexcept override { return false; }
    bool stopCkTab(int) noexcept override                 { return false; }
    std::string queryCkTab(int) const override            { return {}; }
    uint64_t startAsyncCkCompile(int, const std::string&,
                                 std::function<void(bool,const std::string&)>) override { return 0; }
    nlohmann::json queryCkJob(uint64_t) const override   { return {{"ok",false}}; }
    bool cancelCkJob(uint64_t) override                    { return false; }

    std::filesystem::path resolveRenderPath(hathor::AssetTarget,
                                            std::string_view,
                                            const std::filesystem::path&) override { return {}; }
    void setLiveJamSessionDir(std::filesystem::path)      override {}
    void cleanupLiveJamAssets() noexcept                  override {}
    bool isStudioAssetPath(const std::filesystem::path&) const override { return false; }
    hathor::RenderHandle startBakeRender(uint8_t, std::string, uint64_t, unsigned,
                                         const std::filesystem::path&,
                                         hathor::ChuckRenderWriter::CompletionCallback) override { return {}; }
    hathor::RenderHandle startBakeRenderRaw(uint8_t, std::string, uint64_t, unsigned,
                                            const std::filesystem::path&,
                                            hathor::ChuckRenderWriter::CompletionCallback) override { return {}; }
    int activeRenderCount() const noexcept     override { return 0; }
    void shutdownRender() noexcept               override {}
    bool registerBakedAsset(std::string, const std::filesystem::path&) override { return false; }

    std::vector<std::string> listSamples() const override  { return bank_.names_; }
    void addSampleName(const std::string& name)            { bank_.names_.push_back(name); }

    // AI-2 read-only introspection
    std::vector<SlotInfo> listSlots() const noexcept override {
        std::vector<SlotInfo> out;
        for (size_t i = 0; i < names_.size(); ++i) {
            SlotInfo si;
            si.slotIndex = static_cast<int>(i);
            si.slotName = names_[i];
            si.active = states_[i] != nullptr;
            si.running = slotRunning_[i];
            si.notation = "";
            si.eventCount = 0;
            out.push_back(std::move(si));
        }
        return out;
    }
    SlotInfo getSlotInfo(int idx) const noexcept override {
        SlotInfo si;
        si.slotIndex = idx;
        if (idx >= 0 && static_cast<size_t>(idx) < names_.size()) {
            si.slotName = names_[idx];
            si.active = states_[idx] != nullptr;
            si.running = slotRunning_[idx];
        }
        return si;
    }
    VmStatus getVmStatus(int) const noexcept override      { return VmStatus{}; }
    AudioStatus getAudioStatus() const noexcept override {
        return AudioStatus{running_, bpm_, 44100, gain_, "flat", 0, true, 0};
    }
    std::vector<SlotPlayback> listSlotPlayback() const noexcept override { return {}; }
    std::vector<InstrumentInfo> listChuckInstruments(
        const std::filesystem::path&) const noexcept override { return {}; }
    std::filesystem::path studioInstrumentsDir(
        const std::filesystem::path&) const noexcept override { return {}; }
    std::filesystem::path currentProjectDir() const noexcept override { return projectDir_; }
    void setProjectDir(std::filesystem::path dir) override     { projectDir_ = std::move(dir); }

    // Mutators for test configuration
    void setBpm_val(double b)       { bpm_ = b; }
    void setRunning(bool r)         { running_ = r; }

private:
    double bpm_ = 140.0;
    bool running_ = true;
    float gain_ = 0.8f;
    std::vector<std::string> names_;
    std::vector<std::shared_ptr<SlotState>> states_;
    std::vector<bool> slotRunning_ = std::vector<bool>(16, false);
    std::filesystem::path projectDir_ = std::filesystem::current_path();

    struct BankInfo {
        std::vector<std::string> names_;
    } bank_;
};

// ---------------------------------------------------------------------------
// Fake EditorContextProvider
// ---------------------------------------------------------------------------

class FakeEditorContextProvider final : public EditorContextProvider {
public:
    EditorContextSnapshot snapshot() const override
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return current_;
    }

    void setSnapshot(const EditorContextSnapshot& snap)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current_ = snap;
    }

    void setCursor(int line, int ch)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current_.cursorLine = line;
        current_.cursorChar = ch;
    }

    void setFile(const std::string& path, const std::string& lang,
                 const std::string& content = "")
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current_.file = path;
        current_.language = lang;
        current_.content = content;
        if (lang == "mininotation")
            current_.uri = "file://" + path;
        else
            current_.uri = "file://" + path;
        current_.hasContent = !path.empty();
    }

    void setSelection(int startLine, int startChar, int endLine, int endChar,
                      const std::string& text)
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current_.hasSelection = true;
        current_.selStartLine = startLine;
        current_.selStartChar = startChar;
        current_.selEndLine = endLine;
        current_.selEndChar = endChar;
        current_.selectedText = text;
    }

    void clearSelection()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current_.hasSelection = false;
    }

    void clear()
    {
        std::lock_guard<std::mutex> lock(mtx_);
        current_ = EditorContextSnapshot{};
    }

private:
    mutable std::mutex mtx_;
    EditorContextSnapshot current_;
};

// ---------------------------------------------------------------------------
// Fake LspContextProvider
// ---------------------------------------------------------------------------

class FakeLspContextProvider final : public LspContextProvider {
public:
    nlohmann::json lspStatus() const override
    {
        return nlohmann::json{
            {"ok", ok_},
            {"reason", ok_ ? std::string{} : reason_}
        };
    }

    nlohmann::json diagnosticsForDocument(std::string_view uri) const override
    {
        nlohmann::json result;
        result["ok"] = ok_;
        result["source"] = "strudel_lsp";
        result["uri"] = std::string(uri);

        if (!ok_) {
            result["reason"] = reason_;
            result["diagnostics"] = nlohmann::json::array();
            return result;
        }

        result["diagnostics"] = nlohmann::json::array();
        for (const auto& d : diags_)
            result["diagnostics"].push_back(d);
        result["count"] = diags_.size();
        return result;
    }

    nlohmann::json completionsAt(std::string_view, int, int, std::string_view) const override
    {
        return nlohmann::json{{"ok", false}, {"reason", "not available"}};
    }

    nlohmann::json hoverAt(std::string_view, int, int) const override
    {
        return nlohmann::json{{"ok", false}, {"reason", "not available"}};
    }

    void setOk(bool ok, const std::string& reason = {}) {
        ok_ = ok;
        reason_ = reason;
    }

    void addDiagnostic(const nlohmann::json& diag)
    {
        diags_.push_back(diag);
    }

    void clearDiagnostics()
    {
        diags_.clear();
    }

private:
    bool ok_ = false;
    std::string reason_ = "LSP not started";
    std::vector<nlohmann::json> diags_;
};

// ---------------------------------------------------------------------------
// Metadata loading helper
// ---------------------------------------------------------------------------

static hathor::language::LoadResult loadTestMetadata()
{
    const std::string path = std::string(LANGUAGE_METADATA_DIR) + "/HathorLanguageMetadata.json";
    return hathor::language::loadAndValidate(path);
}

// ===========================================================================
// TESTS
// ===========================================================================

// ---------------------------------------------------------------------------
// 1. Context assembly for .hathor (mini-notation) files
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: context assembly for .hathor files includes relevant sections",
          "[ai8][hathor]")
{
    auto metaResult = loadTestMetadata();
    REQUIRE(metaResult.compatibility.compatible);

    Ai8FakeFacade audio;
    SampleBank bank;
    bank.addTestEntry(SampleEntry{"bd", 0, {}, 1, 44100.0, "bd.wav"});
    bank.addTestEntry(SampleEntry{"sn", 1, {}, 1, 44100.0, "sn.wav"});

    hathor::control::ProjectReadFacade readFacade(audio, bank);

    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    // Simulate an active .hathor editor tab.
    FakeEditorContextProvider editor;
    EditorContextSnapshot snap;
    snap.capturedAt = "2026-08-10T11:27:01Z";
    snap.hasContent = true;
    snap.file = "/project/song.hathor";
    snap.uri = "file:///project/song.hathor";
    snap.language = "mininotation";
    snap.content = "bd sn hh cp";
    snap.cursorLine = 0;
    snap.cursorChar = 5;
    snap.slotName = "d0";
    snap.slotIndex = 0;
    snap.hasSelection = false;
    editor.setSnapshot(snap);
    ctx.setEditorContextProvider(&editor);

    ContextRequest req;
    req.file = snap.file;
    auto result = ctx.assemble(req);

    REQUIRE(result["ok"] == true);
    REQUIRE(result["metadata_version"]["schema"] == 1);
    REQUIRE(result["metadata_version"]["compatible"] == true);

    auto& sections = result["sections"];
    REQUIRE(sections.contains("editor"));
    REQUIRE(sections["editor"]["active"] == true);
    REQUIRE(sections["editor"]["file"] == "/project/song.hathor");
    REQUIRE(sections["editor"]["language"] == "mininotation");
    REQUIRE(sections["editor"]["cursor"]["line"] == 0);
    REQUIRE(sections["editor"]["cursor"]["character"] == 5);
    REQUIRE(sections["editor"]["pattern"]["slot_name"] == "d0");

    REQUIRE(sections.contains("diagnostics"));
    REQUIRE(sections["diagnostics"]["sources"].is_array());
    REQUIRE(sections["diagnostics"]["compiler_diagnostics"].is_array());

    REQUIRE(sections.contains("metadata"));
    REQUIRE(sections["metadata"]["available"] == true);
    REQUIRE(sections["metadata"]["functions"].is_array());
    REQUIRE(sections["metadata"]["functions"].size() > 0);
    REQUIRE(sections["metadata"]["operators"].is_array());

    REQUIRE(sections.contains("runtime"));
    REQUIRE(sections["runtime"]["transport"]["bpm"] == 140.0);
    REQUIRE(sections["runtime"]["transport"]["running"] == true);

    REQUIRE(sections.contains("samples"));
    REQUIRE(sections["samples"]["ok"] == true);

    REQUIRE(sections.contains("lsp"));
    REQUIRE(sections["lsp"]["available"] == false);
}

// ---------------------------------------------------------------------------
// 2. Context assembly for .ck (ChucK) files
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: context assembly for .ck files includes relevant sections",
          "[ai8][chuck]")
{
    auto metaResult = loadTestMetadata();
    REQUIRE(metaResult.compatibility.compatible);

    Ai8FakeFacade audio;
    SampleBank bank;

    hathor::control::ProjectReadFacade readFacade(audio, bank);

    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    FakeEditorContextProvider editor;
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/project/instruments/bass.ck";
    snap.uri = "file:///project/instruments/bass.ck";
    snap.language = "chuck";
    snap.content = "SinOsc s => dac; 440 => s.freq;";
    snap.cursorLine = 0;
    snap.cursorChar = 13;
    snap.slotName = "d1";
    snap.slotIndex = 1;
    editor.setSnapshot(snap);
    ctx.setEditorContextProvider(&editor);

    ContextRequest req;
    req.file = snap.file;
    req.language = "chuck";
    auto result = ctx.assemble(req);

    REQUIRE(result["ok"] == true);

    auto& sections = result["sections"];
    REQUIRE(sections.contains("editor"));
    REQUIRE(sections["editor"]["language"] == "chuck");
    REQUIRE(sections["editor"]["cursor"]["line"] == 0);
    REQUIRE(sections["editor"]["cursor"]["character"] == 13);

    REQUIRE(sections.contains("diagnostics"));
    REQUIRE(sections.contains("metadata"));
    REQUIRE(sections["metadata"]["chuck_api"].is_array());
    REQUIRE(sections["metadata"]["chuck_api"].size() > 0);

    REQUIRE(sections.contains("runtime"));
    REQUIRE(sections.contains("instruments"));
    REQUIRE(sections.contains("project"));
}

// ---------------------------------------------------------------------------
// 3. Cursor/selection changes reflected in context
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: cursor and selection changes are reflected in context",
          "[ai8][cursor][selection]")
{
    auto metaResult = loadTestMetadata();
    Ai8FakeFacade audio;
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    FakeEditorContextProvider editor;
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/project/test.hathor";
    snap.uri = "file:///project/test.hathor";
    snap.language = "mininotation";
    snap.content = "bd sn hh cp\ncp bd sn hh";
    snap.slotName = "d0";
    editor.setSnapshot(snap);
    ctx.setEditorContextProvider(&editor);

    SECTION("Cursor at line 0, char 5") {
        editor.setCursor(0, 5);
        ContextRequest req;
        req.includeContent = true;
        auto result = ctx.assemble(req);
        REQUIRE(result["ok"] == true);
        REQUIRE(result["sections"]["editor"]["cursor"]["line"] == 0);
        REQUIRE(result["sections"]["editor"]["cursor"]["character"] == 5);
    }

    SECTION("Cursor at line 1, char 3") {
        editor.setCursor(1, 3);
        ContextRequest req;
        req.includeContent = true;
        auto result = ctx.assemble(req);
        REQUIRE(result["sections"]["editor"]["cursor"]["line"] == 1);
        REQUIRE(result["sections"]["editor"]["cursor"]["character"] == 3);
    }

    SECTION("Selection from (0,0) to (1,2)") {
        editor.setSelection(0, 0, 1, 2, "bd sn hh cp\ncp");
        ContextRequest req;
        auto result = ctx.assemble(req);
        REQUIRE(result["sections"]["editor"]["hasSelection"] == true);
        REQUIRE(result["sections"]["editor"]["selection"]["start_line"] == 0);
        REQUIRE(result["sections"]["editor"]["selection"]["start_char"] == 0);
        REQUIRE(result["sections"]["editor"]["selection"]["end_line"] == 1);
        REQUIRE(result["sections"]["editor"]["selection"]["end_char"] == 2);
        REQUIRE(result["sections"]["editor"]["selection"]["text"] == "bd sn hh cp\ncp");
    }

    SECTION("Selection cleared after clearSelection") {
        editor.setSelection(0, 0, 1, 2, "bd sn hh cp\ncp");
        editor.clearSelection();
        ContextRequest req;
        auto result = ctx.assemble(req);
        REQUIRE(result["sections"]["editor"]["hasSelection"] == false);
    }
}

// ---------------------------------------------------------------------------
// 4. Metadata version tracking — no stale metadata
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: metadata version is identified in context response",
          "[ai8][metadata-version]")
{
    auto metaResult = loadTestMetadata();
    REQUIRE(metaResult.compatibility.compatible);

    Ai8FakeFacade audio;
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    ContextRequest req;
    auto result = ctx.assemble(req);

    REQUIRE(result["ok"] == true);
    auto& mv = result["metadata_version"];
    REQUIRE(mv["schema"] == hathor::language::kSchemaVersion);
    REQUIRE(mv["engine"] == std::string(hathor::language::kHathorEngineCompat));
    REQUIRE(mv["strudel"] == std::string(hathor::language::kStrudelMiniNotationCompat));
    REQUIRE(mv["chuck"] == std::string(hathor::language::kChuckLibVersion));
    REQUIRE(mv["surface"] == std::string(hathor::language::kChuckIntegrationSurface));
}

// ---------------------------------------------------------------------------
// 5. Changed BPM / samples / instruments reflected
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: runtime state changes (BPM, samples) are reflected dynamically",
          "[ai8][runtime-dynamics]")
{
    auto metaResult = loadTestMetadata();
    Ai8FakeFacade audio;
    SampleBank bank;
    bank.addTestEntry(SampleEntry{"bd", 0, {}, 1, 44100.0, "bd.wav"});
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    // Initial state: BPM = 140, one sample
    {
        ContextRequest req;
        req.scope = {"runtime"};
        auto result = ctx.assemble(req);
        REQUIRE(result["sections"]["runtime"]["transport"]["bpm"] == 140.0);
    }

    // Change BPM — the next request should reflect the new value.
    audio.setBpm_val(160.0);
    {
        ContextRequest req;
        req.scope = {"runtime"};
        auto result = ctx.assemble(req);
        REQUIRE(result["sections"]["runtime"]["transport"]["bpm"] == 160.0);
    }

    // Add a sample — the next request should include it.
    bank.addTestEntry(SampleEntry{"sn", 1, {}, 1, 44100.0, "sn.wav"});
    {
        ContextRequest req;
        req.scope = {"samples"};
        auto result = ctx.assemble(req);
        auto& samples = result["sections"]["samples"];
        REQUIRE(samples["ok"] == true);
        // Should have at least 2 sample names
        REQUIRE(samples["samples"].size() >= 2);
    }
}

// ---------------------------------------------------------------------------
// 6. LSP-derived diagnostics in context
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: LSP diagnostics are included when LSP is available",
          "[ai8][lsp-diagnostics]")
{
    auto metaResult = loadTestMetadata();
    Ai8FakeFacade audio;
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    FakeLspContextProvider lsp;
    lsp.setOk(true);
    lsp.addDiagnostic(nlohmann::json{
        {"severity", "warning"},
        {"code", "LSP_W001"},
        {"message", "Unknown function 'foo'"},
        {"line", 0},
        {"column", 5}
    });
    ctx.setLspContextProvider(&lsp);

    FakeEditorContextProvider editor;
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/project/test.hathor";
    snap.uri = "file:///project/test.hathor";
    snap.language = "mininotation";
    snap.content = "foo bd";
    snap.slotName = "d0";
    editor.setSnapshot(snap);
    ctx.setEditorContextProvider(&editor);

    ContextRequest req;
    auto result = ctx.assemble(req);

    REQUIRE(result["ok"] == true);
    auto& lspSection = result["sections"]["lsp"];
    REQUIRE(lspSection["available"] == true);
    REQUIRE(lspSection["source"] == "strudel_lsp");
    REQUIRE(lspSection["diagnostics"]["count"] == 1);
    REQUIRE(lspSection["diagnostics"]["diagnostics"][0]["message"] == "Unknown function 'foo'");
}

// ---------------------------------------------------------------------------
// 7. Missing / incompatible LSP
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: missing LSP provider degrades gracefully",
          "[ai8][lsp-missing]")
{
    auto metaResult = loadTestMetadata();
    Ai8FakeFacade audio;
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    // No LSP provider set (nullptr).
    ContextRequest req;
    auto result = ctx.assemble(req);

    REQUIRE(result["ok"] == true);
    auto& lspSection = result["sections"]["lsp"];
    REQUIRE(lspSection["available"] == false);
}

TEST_CASE("AI-8: LSP unavailable (server not running) is surfaced explicitly",
          "[ai8][lsp-unavailable]")
{
    auto metaResult = loadTestMetadata();
    Ai8FakeFacade audio;
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    FakeLspContextProvider lsp;
    lsp.setOk(false, "LSP server not running");
    ctx.setLspContextProvider(&lsp);

    ContextRequest req;
    auto result = ctx.assemble(req);

    auto& lspSection = result["sections"]["lsp"];
    REQUIRE(lspSection["available"] == false);
    REQUIRE(lspSection["reason"] == "LSP server not running");
}

// ---------------------------------------------------------------------------
// 8. Missing / incompatible metadata
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: missing metadata is surfaced explicitly",
          "[ai8][metadata-missing]")
{
    Ai8FakeFacade audio;
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    // No metadata (nullptr).
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         nullptr, nullptr);

    ContextRequest req;
    auto result = ctx.assemble(req);

    REQUIRE(result["ok"] == true);
    REQUIRE(result["metadata_version"]["available"] == false);
    REQUIRE(result["metadata_version"]["reason"] == "LanguageMetadata not loaded");

    auto& metadataSection = result["sections"]["metadata"];
    REQUIRE(metadataSection["available"] == false);
}

TEST_CASE("AI-8: incompatible metadata is surfaced explicitly",
          "[ai8][metadata-incompatible]")
{
    Ai8FakeFacade audio;
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);

    // Simulate incompatible metadata.
    hathor::language::LanguageMetadata metadata;
    metadata.schemaVersion = 99;
    metadata.hathorEngineCompat = "0.0.1";
    metadata.strudelMiniNotationCompat = "99.9.9";
    metadata.chuckLibVersion = "0.0.0";
    metadata.chuckIntegrationSurface = "unknown";

    hathor::language::MetadataCompatibility compat;
    compat.compatible = false;
    compat.errors = {"Schema version mismatch", "Engine compatibility mismatch"};

    AuthoringContext ctx(readFacade, nullptr, nullptr, &metadata, &compat);

    ContextRequest req;
    req.scope = {"metadata"};
    auto result = ctx.assemble(req);

    REQUIRE(result["metadata_version"]["compatible"] == false);
    REQUIRE(result["metadata_version"]["errors"].is_array());
    REQUIRE(result["metadata_version"]["errors"].size() == 2);

    auto& metadataSection = result["sections"]["metadata"];
    REQUIRE(metadataSection["available"] == false);
    REQUIRE(metadataSection["reason"].get<std::string>().find("incompatible") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 9. Targeted context — specific scope only
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: explicit scope limits sections to what is requested",
          "[ai8][targeted]")
{
    auto metaResult = loadTestMetadata();
    Ai8FakeFacade audio;
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    FakeEditorContextProvider editor;
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/project/test.hathor";
    snap.uri = "file:///project/test.hathor";
    snap.language = "mininotation";
    snap.content = "bd sn";
    snap.slotName = "d0";
    editor.setSnapshot(snap);
    ctx.setEditorContextProvider(&editor);

    SECTION("Request only editor + metadata") {
        ContextRequest req;
        req.scope = {"editor", "metadata"};
        auto result = ctx.assemble(req);
        auto& sections = result["sections"];
        REQUIRE(sections.contains("editor"));
        REQUIRE(sections.contains("metadata"));
        REQUIRE_FALSE(sections.contains("runtime"));
        REQUIRE_FALSE(sections.contains("samples"));
        REQUIRE_FALSE(sections.contains("lsp"));
        REQUIRE_FALSE(sections.contains("project"));
    }

    SECTION("Request only runtime") {
        ContextRequest req;
        req.scope = {"runtime"};
        auto result = ctx.assemble(req);
        auto& sections = result["sections"];
        REQUIRE(sections.contains("runtime"));
        REQUIRE_FALSE(sections.contains("editor"));
        REQUIRE_FALSE(sections.contains("metadata"));
    }
}

// ---------------------------------------------------------------------------
// 10. No mutations through context assembly
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: context assembly is read-only — no state mutations",
          "[ai8][read-only]")
{
    auto metaResult = loadTestMetadata();
    Ai8FakeFacade audio;
    SampleBank bank;
    bank.addTestEntry(SampleEntry{"bd", 0, {}, 1, 44100.0, "bd.wav"});
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    FakeEditorContextProvider editor;
    EditorContextSnapshot snap;
    snap.hasContent = true;
    snap.file = "/project/test.hathor";
    snap.uri = "file:///project/test.hathor";
    snap.language = "mininotation";
    snap.content = "bd sn";
    snap.slotName = "d0";
    snap.cursorLine = 0;
    snap.cursorChar = 0;
    editor.setSnapshot(snap);
    ctx.setEditorContextProvider(&editor);

    // Record state before.
    const double bpmBefore = audio.getBpm();
    const int sampleCountBefore = static_cast<int>(bank.listNames().size());
    const auto snapshotBefore = editor.snapshot();

    // Assemble context multiple times.
    ContextRequest req;
    for (int i = 0; i < 5; ++i)
        ctx.assemble(req);

    // Verify state is unchanged.
    REQUIRE(audio.getBpm() == bpmBefore);
    REQUIRE(static_cast<int>(bank.listNames().size()) == sampleCountBefore);

    // Editor snapshot is unchanged (we didn't modify it).
    auto snapAfter = editor.snapshot();
    REQUIRE(snapAfter.content == snapshotBefore.content);
    REQUIRE(snapAfter.cursorLine == snapshotBefore.cursorLine);
    REQUIRE(snapAfter.cursorChar == snapshotBefore.cursorChar);
}

// ---------------------------------------------------------------------------
// 11. Full get-context command through ControlInterface dispatch
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: get-context command routes through ControlInterface",
          "[ai8][dispatch]")
{
    auto metaResult = loadTestMetadata();
    REQUIRE(metaResult.compatibility.compatible);

    Ai8FakeFacade audio;
    SampleBank bank;
    bank.addTestEntry(SampleEntry{"bd", 0, {}, 1, 44100.0, "bd.wav"});

    hathor::control::ControlInterface ci(audio, bank);

    // Inject providers.
    ci.setEditorContextProvider(nullptr);  // no editor provider in this test
    ci.setLspContextProvider(nullptr);
    ci.setLanguageMetadata(&metaResult.metadata, &metaResult.compatibility);

    // Build the get-context command with JSON arguments.
    nlohmann::json args;
    args["file"] = "/project/song.hathor";
    args["language"] = "mininotation";
    args["scope"] = {"metadata", "runtime"};
    const std::string cmd = "get-context " + args.dump();

    nlohmann::json response;
    bool callbackCalled = false;

    ci.dispatchWithCallback(cmd,
        [&response, &callbackCalled](nlohmann::json result)
        {
            response = std::move(result);
            callbackCalled = true;
        });

    REQUIRE(callbackCalled);
    REQUIRE(response["ok"] == true);
    REQUIRE(response["metadata_version"]["schema"] == 1);
    REQUIRE(response["sections"].contains("metadata"));
    REQUIRE(response["sections"].contains("runtime"));
    // Should NOT contain editor (no editor provider, and not in scope).
    REQUIRE_FALSE(response["sections"].contains("editor"));
    REQUIRE_FALSE(response["sections"].contains("samples"));
}

// ---------------------------------------------------------------------------
// 12. No active editor — context assembly without editor provider
// ---------------------------------------------------------------------------

TEST_CASE("AI-8: context assembly with no editor provider returns available=false",
          "[ai8][no-editor]")
{
    auto metaResult = loadTestMetadata();
    Ai8FakeFacade audio;
    SampleBank bank;
    hathor::control::ProjectReadFacade readFacade(audio, bank);
    AuthoringContext ctx(readFacade, nullptr, nullptr,
                         &metaResult.metadata, &metaResult.compatibility);

    // No editor or LSP provider set.
    ContextRequest req;
    auto result = ctx.assemble(req);

    REQUIRE(result["ok"] == true);
    auto& editor = result["sections"]["editor"];
    REQUIRE(editor["active"] == false);
    REQUIRE(editor["file"] == nullptr);
}
