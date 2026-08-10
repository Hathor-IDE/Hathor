// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_ai_fakes.hpp — JUCE-free test doubles for AI-3/AI-8 control-layer tests.
 *
 * Shared between test_ai8_context.cpp and test_ai_g3_context.cpp so both the
 * AI-8 dynamic authoring context tests and the AI-G3 completion-context tests
 * exercise the SAME fakes against the REAL ProjectReadFacade + SampleBank.
 *
 * Requirement references: AI-8, AI-G3
 */

#pragma once

#include "AudioEngineFacade.hpp"
#include "SampleBank.hpp"
#include "EditorContextProvider.hpp"
#include "LspContextProvider.hpp"

#include "hathor/LanguageMetadata.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#ifndef LANGUAGE_METADATA_DIR
#error "LANGUAGE_METADATA_DIR compile definition is not set. Check CMakeLists.txt."
#endif

// ---------------------------------------------------------------------------
// Fake AudioEngineFacade (JUCE-free stand-in for the real AudioEngine)
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

class FakeEditorContextProvider final : public hathor::control::EditorContextProvider {
public:
    hathor::control::EditorContextSnapshot snapshot() const override
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return current_;
    }

    void setSnapshot(const hathor::control::EditorContextSnapshot& snap)
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
        current_ = hathor::control::EditorContextSnapshot{};
    }

private:
    mutable std::mutex mtx_;
    hathor::control::EditorContextSnapshot current_;
};

// ---------------------------------------------------------------------------
// Fake LspContextProvider
// ---------------------------------------------------------------------------

class FakeLspContextProvider final : public hathor::control::LspContextProvider {
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

inline hathor::language::LoadResult loadTestMetadata()
{
    const std::string path = std::string(LANGUAGE_METADATA_DIR) + "/HathorLanguageMetadata.json";
    return hathor::language::loadAndValidate(path);
}
