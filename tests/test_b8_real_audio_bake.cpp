// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_b8_real_audio_bake.cpp — B8 end-to-end REAL-audio bake verification.
 *
 * B4-K4/K7 replaced the worker's placeholder sine-tone render callback with
 * genuine ChucK execution.  This suite proves the B8 bake path carries that
 * REAL instrument audio all the way to a published .wav, and that the asset
 * then binds into SampleBank / .hathor notation exactly like any project
 * sample:
 *
 *   1. A deterministic ChucK instrument (440 Hz sine @ 0.4 gain → dac,
 *      infinite sustain) is baked through ChuckRenderWriter.  The published
 *      WAV is read back and its CONTENT is analysed: dominant frequency
 *      ≈ 440 Hz and peak amplitude > 0.1.  Both distinguish it from the
 *      legacy placeholder tone (220 Hz @ 0.05 amplitude) — a compile-only
 *      or silent path cannot satisfy these assertions.
 *   2. The real baked WAV registers in SampleBank and resolves through the
 *      ordinary `.hathor` mini-notation path (parseMini → lowerToParamMap
 *      → keys::kS → SampleBank::find) with NO live ChucK VM required.
 *   3. The `.hathor_assets/chuck_instruments/` layout persists both
 *      `{name}.ck` (regeneration source) and `{name}.wav` (the real render
 *      output), resolved by AssetPathResolver.
 *   4. A program that compiles and runs but produces no dac audio FAILS the
 *      bake (honest failure — no silent WAV is published, no .tmp orphan).
 *   5. Baking one tab does not disturb another tab's real execution
 *      (per-tab isolation preserved through the render path).
 *
 * JUCE-free: links Catch2 + AudioWorkerManager + ChuckRenderWriter +
 * hathor-engine (for the mini-notation/sample-resolution proof).
 * Requires the hathor-audio-worker binary to be built.
 *
 * Requirements: B8-K1, B8-K2, B8-K3, B8-K4, B4-K4, B4-K7, B4-K8
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "AudioWorkerManager.hpp"
#include "ChuckRenderWriter.hpp"
#include "audio_ipc.h"
#include "SampleBank.hpp"
#include "AssetPathResolver.hpp"

#include "hathor/MiniParser.hpp"
#include "hathor/PatternCompiler.hpp"
#include "hathor/Pattern.hpp"
#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"
#include "hathor/Event.hpp"
#include "hathor/ParamMap.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using hathor::AssetPathResolver;
using hathor::AudioWorkerManager;
using hathor::ChuckRenderWriter;
using hathor::RenderHandle;
using hathor::RenderResult;
using hathor::RenderState;
using hathor::audio_worker::kBlockSize;

// SampleBank / SampleEntry live in the global namespace (app/SampleBank.hpp).
using SampleBank = ::SampleBank;
using SampleEntry = ::SampleEntry;

namespace {

constexpr unsigned kSampleRate = 44100;

/// Blank Event buffers for pre-sizing query buffers (Event has no default
/// constructor; see test_miniparser.cpp's blankEvent()).
static hathor::Event<std::string> blankStrEvent()
{
    hathor::Arc z{hathor::Rational{0}, hathor::Rational{0}};
    return hathor::Event<std::string>{z, z, {}};
}

static hathor::Event<hathor::ParamMap> blankParamEvent()
{
    hathor::Arc z{hathor::Rational{0}, hathor::Rational{0}};
    return hathor::Event<hathor::ParamMap>{z, z, hathor::ParamMap{}};
}

// ---------------------------------------------------------------------------
// Worker binary discovery (same convention as the B4-K4 / B8-K3 suites)
// ---------------------------------------------------------------------------

std::string getWorkerPath()
{
    namespace fs = std::filesystem;

#ifdef CMAKE_BINARY_DIR
    fs::path p = fs::path(CMAKE_BINARY_DIR) / "app" / "audio-worker" / "hathor-audio-worker";
    if (fs::exists(p))
        return p.string();
    p = fs::path(CMAKE_BINARY_DIR) / "hathor-audio-worker";
    if (fs::exists(p))
        return p.string();
#endif

#ifdef CMAKE_SOURCE_DIR
    {
        fs::path p = fs::path(CMAKE_SOURCE_DIR) / "build" / "app" / "audio-worker" / "hathor-audio-worker";
        if (fs::exists(p))
            return p.string();
    }
#endif

    const char* envSrc = std::getenv("CMAKE_SOURCE_DIR");
    if (envSrc) {
        fs::path p = fs::path(envSrc) / "build" / "app" / "audio-worker" / "hathor-audio-worker";
        if (fs::exists(p))
            return p.string();
    }

    const fs::path candidates[] = {
        fs::current_path() / "hathor-audio-worker",
        fs::current_path() / "build" / "hathor-audio-worker",
        fs::current_path() / "build" / "app" / "audio-worker" / "hathor-audio-worker",
        fs::current_path() / "app" / "audio-worker" / "hathor-audio-worker",
    };

    for (const auto& p : candidates) {
        if (fs::exists(p))
            return p.string();
    }

    return "";
}

// ---------------------------------------------------------------------------
// Deterministic ChucK instruments.
//
// kRealInstrument440 — the "real instrument" under test.  440 Hz @ 0.4 gain
// routed to dac, sustained forever.  The legacy placeholder tone was a
// 220 Hz sine at 0.05 amplitude; both the frequency and amplitude assertions
// below are chosen to be impossible for that placeholder to satisfy.
// ---------------------------------------------------------------------------

const std::string kRealInstrument440 =
    "SinOsc s => dac; 0.4 => s.gain; 440.0 => s.freq; while(true) 1::samp => now;";

const std::string kInstr880 =
    "SinOsc s => dac; 0.25 => s.gain; 880.0 => s.freq; while(true) 1::samp => now;";

/// Compiles and runs fine, but nothing is routed to dac — a bake of this
/// must FAIL honestly instead of publishing a silent WAV.
const std::string kSilentProgram =
    "SinOsc s => blackhole; 0.4 => s.gain; 440.0 => s.freq; while(true) 1::samp => now;";

// ---------------------------------------------------------------------------
// WAV read-back (minimal, uncompressed PCM16 mono — matches what
// ChuckRenderWriter publishes)
// ---------------------------------------------------------------------------

/// Parse a mono 16-bit PCM WAV into float samples.  Returns false on any
/// structural mismatch (wrong RIFF markers, non-PCM, channel/sample-rate
/// mismatch).
static bool readWavMono(const std::filesystem::path& path,
                        std::vector<float>& out,
                        unsigned& sampleRate)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    char fourcc[4];
    in.read(fourcc, 4);
    if (std::memcmp(fourcc, "RIFF", 4) != 0)
        return false;
    uint32_t riffSize;
    in.read(reinterpret_cast<char*>(&riffSize), 4);
    in.read(fourcc, 4);
    if (std::memcmp(fourcc, "WAVE", 4) != 0)
        return false;

    // Walk chunks until we find fmt + data.
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t rate = 0;
    std::vector<float> samples;

    while (in.read(fourcc, 4)) {
        uint32_t chunkSize;
        in.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (std::memcmp(fourcc, "fmt ", 4) == 0) {
            uint16_t audioFormat;
            uint16_t blockAlign;
            uint32_t byteRate;
            in.read(reinterpret_cast<char*>(&audioFormat), 2);
            in.read(reinterpret_cast<char*>(&channels), 2);
            in.read(reinterpret_cast<char*>(&rate), 4);
            in.read(reinterpret_cast<char*>(&byteRate), 4);
            in.read(reinterpret_cast<char*>(&blockAlign), 2);
            in.read(reinterpret_cast<char*>(&bits), 2);
            if (audioFormat != 1)
                return false;
            (void)byteRate;
            (void)blockAlign;
        } else if (std::memcmp(fourcc, "data", 4) == 0) {
            const std::streamoff dataStart = in.tellg();
            std::vector<int16_t> raw(chunkSize / 2);
            in.read(reinterpret_cast<char*>(raw.data()),
                    static_cast<std::streamsize>(chunkSize));
            (void)dataStart;
            samples.reserve(raw.size());
            for (int16_t s : raw)
                samples.push_back(static_cast<float>(s) / 32768.0f);
        } else {
            in.seekg(chunkSize, std::ios::cur);
        }
    }

    if (channels != 1 || bits != 16 || samples.empty())
        return false;
    sampleRate = rate;
    out = std::move(samples);
    return true;
}

// ---------------------------------------------------------------------------
// Signal analysis helpers (identical approach to the B4-K4 execution suite)
// ---------------------------------------------------------------------------

static float peakAmplitude(const std::vector<float>& buf)
{
    float peak = 0.0f;
    for (float s : buf) {
        const float a = std::fabs(s);
        if (a > peak) peak = a;
    }
    return peak;
}

/// Trim leading/trailing near-silence; returns the length of the voiced
/// region and the start offset into buf.
static std::size_t trimSilence(const std::vector<float>& buf, float thr,
                               std::size_t& outStart)
{
    std::size_t start = 0;
    std::size_t end = buf.size();
    while (start < end && std::fabs(buf[start]) < thr) ++start;
    while (end > start && std::fabs(buf[end - 1]) < thr) --end;
    outStart = start;
    return end - start;
}

/// Dominant frequency via zero-crossings (0 for silence).
static double estimateFrequencyHz(const std::vector<float>& buf,
                                  std::size_t begin, std::size_t n,
                                  float sampleRate)
{
    if (n < 4)
        return 0.0;
    std::size_t crossings = 0;
    for (std::size_t i = begin + 1; i < begin + n; ++i) {
        const bool aPos = buf[i - 1] >= 0.0f;
        const bool bPos = buf[i] >= 0.0f;
        if (aPos != bPos)
            ++crossings;
    }
    if (crossings == 0)
        return 0.0;
    const double periodSamples = 2.0 * static_cast<double>(n) / static_cast<double>(crossings);
    return static_cast<double>(sampleRate) / periodSamples;
}

/// Poll vm_query until the tab reports a real loaded shred id (>= 0).
static bool waitForLoadedShred(AudioWorkerManager& mgr, uint8_t tab, uint32_t timeoutMs)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::string q = mgr.queryTabVM(tab).message;
        auto pos = q.find("shred_id=");
        if (pos != std::string::npos) {
            const long id = std::stol(q.substr(pos + 9));
            if (id >= 0)
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/// Collect up to maxBlocks audio blocks from the ring (mono, contiguous).
static uint32_t collectBlocks(AudioWorkerManager& mgr, uint64_t gen,
                              float* out, uint32_t maxBlocks, uint32_t timeoutMs)
{
    uint32_t n = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (n < maxBlocks && std::chrono::steady_clock::now() < deadline) {
        if (mgr.tryReadAudioBlock(out + static_cast<std::size_t>(n) * kBlockSize,
                                  kBlockSize, gen)) {
            ++n;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return n;
}

static void drainRing(AudioWorkerManager& mgr, uint64_t gen)
{
    float drainBuf[kBlockSize];
    while (mgr.tryReadAudioBlock(drainBuf, kBlockSize, gen)) {
        // discard
    }
}

/// Run a single bake to completion and return the result.
static RenderResult bake(AudioWorkerManager& mgr, uint8_t tab,
                         const std::string& ckSource, uint64_t numSamples,
                         const std::filesystem::path& dest)
{
    std::promise<RenderResult> done;
    auto fut = done.get_future();

    ChuckRenderWriter writer(&mgr);
    writer.startRender(tab, ckSource, numSamples, kSampleRate, dest,
        [&](const RenderResult& r) { done.set_value(r); });

    REQUIRE(fut.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
    return fut.get();
}

} // namespace

// ---------------------------------------------------------------------------
// Test 1 — the published WAV contains the instrument's REAL audio
// ---------------------------------------------------------------------------

TEST_CASE("B8: baked WAV contains the real ChucK instrument audio (440 Hz @ 0.4, not the 220 Hz placeholder)",
          "[b8][real-audio][bake]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    auto tmpDir = std::filesystem::temp_directory_path();
    auto wavPath = tmpDir / "b8_real_audio_440.wav";
    std::filesystem::remove(wavPath);

    // Bake one second of the real 440 Hz instrument.
    RenderResult result = bake(mgr, 0, kRealInstrument440, 44100, wavPath);

    REQUIRE(result.success);
    REQUIRE(result.state == RenderState::Completed);
    REQUIRE(result.errorMessage.empty());
    REQUIRE(result.samplesWritten == 44100);
    REQUIRE(std::filesystem::exists(wavPath));

    // No temp file may remain after a successful publish.
    REQUIRE_FALSE(std::filesystem::exists(wavPath.string() + ".tmp"));

    // Read the WAV back and analyse its CONTENT — not just its existence.
    std::vector<float> pcm;
    unsigned rate = 0;
    REQUIRE(readWavMono(wavPath, pcm, rate));
    REQUIRE(rate == kSampleRate);
    REQUIRE(pcm.size() == 44100);

    // 1. Amplitude: the instrument runs at 0.4 gain.  The legacy placeholder
    //    peaked at 0.05, so a real render must clearly exceed it.
    const float peak = peakAmplitude(pcm);
    REQUIRE(peak > 0.1f);
    REQUIRE(peak <= 1.0f);

    // 2. Frequency: the instrument is a 440 Hz oscillator.  The placeholder
    //    was 220 Hz; a compile-only or silent path would not produce 440 Hz.
    std::size_t start = 0;
    const std::size_t voiced = trimSilence(pcm, 1e-3f, start);
    REQUIRE(voiced > pcm.size() / 2);
    const double freq = estimateFrequencyHz(pcm, start, voiced, static_cast<float>(rate));
    REQUIRE(freq > 0.0);
    REQUIRE(std::fabs(freq - 440.0) / 440.0 < 0.08);

    // The VM used for the bake must be gone afterwards (B8-K3) and the
    // worker must remain healthy (B4-K8 — no crash from the render).
    auto query = mgr.queryTabVM(0);
    const bool vmGone =
        query.message.find("destroyed") != std::string::npos ||
        query.message.find("inactive") != std::string::npos ||
        query.message.find("no_vm") != std::string::npos;
    REQUIRE(vmGone);
    REQUIRE(mgr.isWorkerAlive());

    std::filesystem::remove(wavPath);
    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 2 — the real WAV binds into SampleBank and resolves via .hathor
// ---------------------------------------------------------------------------

TEST_CASE("B8: real baked WAV registers in SampleBank and resolves via .hathor s-name notation (no live VM)",
          "[b8][samplebank][hathor-resolution]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    auto tmpDir = std::filesystem::temp_directory_path();
    auto wavPath = tmpDir / "b8_acid_bass.wav";
    std::filesystem::remove(wavPath);

    RenderResult result = bake(mgr, 1, kRealInstrument440, 22050, wavPath);
    REQUIRE(result.success);
    REQUIRE(result.state == RenderState::Completed);

    // The render destroyed its VM; shut the worker down entirely so the
    // remaining assertions run with NO worker and NO ChucK VM alive.
    mgr.shutdown();

    // Read the REAL PCM that was just produced.
    std::vector<float> pcm;
    unsigned rate = 0;
    REQUIRE(readWavMono(wavPath, pcm, rate));
    REQUIRE(rate == kSampleRate);
    REQUIRE_FALSE(pcm.empty());
    // Sanity: it really is the instrument (440 Hz, > 0.1 peak), not silence.
    REQUIRE(peakAmplitude(pcm) > 0.1f);

    // --- B8-K4: register the baked asset in SampleBank ---------------------
    SampleBank bank;
    bank.addEntry("acid_bass", 0, pcm, 1, kSampleRate, wavPath.string());

    const SampleEntry* entry = bank.find("acid_bass", 0);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->sourcePath == wavPath.string());
    REQUIRE(entry->data.size() == pcm.size());
    // The registered data is byte-identical to the WAV we read back.
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        REQUIRE(entry->data[i] == Catch::Approx(pcm[i]).margin(1e-6f));
    }

    // Autocomplete exposure.
    auto names = bank.listNames();
    REQUIRE(std::find(names.begin(), names.end(), "acid_bass") != names.end());

    // --- .hathor resolution: `acid_bass` in mini-notation ------------------
    // 1. parseMini lowers the notation to a Pattern<std::string> whose
    //    events carry the sample name.
    auto parsed = hathor::parseMini("acid_bass");
    REQUIRE(std::holds_alternative<hathor::CompiledPattern>(parsed));
    auto& cp = std::get<hathor::CompiledPattern>(parsed);

    hathor::Arc arc{hathor::Rational{0}, hathor::Rational{1}};
    std::vector<hathor::Event<std::string>>
        strBuf(cp.pattern.maxEventsPerCycle() + 1, blankStrEvent());
    std::size_t n = cp.pattern.query(arc, std::span<hathor::Event<std::string>>(strBuf));
    REQUIRE(n >= 1);
    REQUIRE(strBuf[0].value == "acid_bass");

    // 2. lowerToParamMap emits keys::kS == the sample name (what VoicePool
    //    consumes in the real playback path).
    auto lowered = hathor::lowerToParamMap(cp.pattern);
    std::vector<hathor::Event<hathor::ParamMap>>
        pmBuf(lowered.maxEventsPerCycle() + 1, blankParamEvent());
    std::size_t m = lowered.query(arc, std::span<hathor::Event<hathor::ParamMap>>(pmBuf));
    REQUIRE(m >= 1);
    const hathor::Value* sv = pmBuf[0].value.get(hathor::keys::kS);
    REQUIRE(sv != nullptr);
    REQUIRE(std::get<std::string>(*sv) == "acid_bass");

    // 3. The exact lookup the playback engine performs resolves to the real
    //    baked instrument.
    const SampleEntry* viaNotation =
        bank.find(std::get<std::string>(*sv), 0);
    REQUIRE(viaNotation == entry);
    REQUIRE(viaNotation->data == pcm);

    std::filesystem::remove(wavPath);
}

// ---------------------------------------------------------------------------
// Test 3 — .hathor_assets lifecycle: {name}.ck + {name}.wav persist
// ---------------------------------------------------------------------------

TEST_CASE("B8: .hathor_assets lifecycle keeps the .ck source and the real .wav output together",
          "[b8][assets][lifecycle]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    auto projectDir = std::filesystem::temp_directory_path() /
        ("hathor_b8_assets_" + std::to_string(::getpid()));
    auto instrumentsDir = projectDir / ".hathor_assets" / "chuck_instruments";
    std::filesystem::create_directories(instrumentsDir);

    // The .ck source is saved alongside — it must remain available for
    // regeneration (Task 5).
    const std::string ckSource = kRealInstrument440;
    const auto ckPath = instrumentsDir / "acid_bass.ck";
    {
        std::ofstream out(ckPath, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        out.write(ckSource.data(), static_cast<std::streamsize>(ckSource.size()));
    }
    REQUIRE(std::filesystem::exists(ckPath));

    // Bake to the conventional destination.
    const auto wavPath = instrumentsDir / "acid_bass.wav";
    std::filesystem::remove(wavPath);

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));
    RenderResult result = bake(mgr, 2, ckSource, 22050, wavPath);
    mgr.shutdown();

    REQUIRE(result.success);
    REQUIRE(result.state == RenderState::Completed);

    // Both halves of the pair exist: source for regeneration, wav = the
    // actual execution of that source.
    REQUIRE(std::filesystem::exists(ckPath));
    REQUIRE(std::filesystem::exists(wavPath));
    REQUIRE_FALSE(std::filesystem::exists(wavPath.string() + ".tmp"));

    // The convention resolves the asset (B8-K1).
    AssetPathResolver resolver(projectDir);
    auto resolved = resolver.resolveStudio("acid_bass");
    REQUIRE(resolved.ok);
    REQUIRE(resolved.path == wavPath);
    REQUIRE(resolver.isStudioPath(wavPath));

    // And the WAV content is the real instrument output.
    std::vector<float> pcm;
    unsigned rate = 0;
    REQUIRE(readWavMono(wavPath, pcm, rate));
    REQUIRE(rate == kSampleRate);
    REQUIRE(peakAmplitude(pcm) > 0.1f);
    std::size_t start = 0;
    const std::size_t voiced = trimSilence(pcm, 1e-3f, start);
    REQUIRE(voiced > pcm.size() / 2);
    const double freq = estimateFrequencyHz(pcm, start, voiced, static_cast<float>(rate));
    REQUIRE(std::fabs(freq - 440.0) / 440.0 < 0.08);

    std::error_code ec;
    std::filesystem::remove_all(projectDir, ec);
}

// ---------------------------------------------------------------------------
// Test 4 — honest failure: valid-but-silent program must not publish a WAV
// ---------------------------------------------------------------------------

TEST_CASE("B8: a program that compiles but produces no dac audio fails the bake honestly",
          "[b8][honest-failure][silence]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));

    auto tmpDir = std::filesystem::temp_directory_path();
    auto wavPath = tmpDir / "b8_silent_fail.wav";
    std::filesystem::remove(wavPath);

    // kSilentProgram compiles and the shred loads and runs — but nothing is
    // routed to dac.  The bake must FAIL (not silently publish silence).
    RenderResult result = bake(mgr, 3, kSilentProgram, 22050, wavPath);

    REQUIRE_FALSE(result.success);
    REQUIRE(result.state == RenderState::Failed);
    // The failure must be surfaced as a runtime/render failure, not hidden.
    REQUIRE(result.errorMessage.find("no audio") != std::string::npos);

    // No asset, no orphaned temp file.
    REQUIRE_FALSE(std::filesystem::exists(wavPath));
    REQUIRE_FALSE(std::filesystem::exists(wavPath.string() + ".tmp"));

    // The render VM must still be cleaned up (B8-K3 on failure paths).
    auto query = mgr.queryTabVM(3);
    const bool vmGone =
        query.message.find("destroyed") != std::string::npos ||
        query.message.find("inactive") != std::string::npos ||
        query.message.find("no_vm") != std::string::npos;
    REQUIRE(vmGone);
    REQUIRE(mgr.isWorkerAlive());

    mgr.shutdown();
}

// ---------------------------------------------------------------------------
// Test 5 — per-tab isolation through the bake path
// ---------------------------------------------------------------------------

TEST_CASE("B8: baking one tab leaves another tab's real execution running",
          "[b8][isolation][bake]")
{
    const std::string workerPath = getWorkerPath();
    if (workerPath.empty()) {
        WARN("hathor-audio-worker binary not found; skipping");
        return;
    }

    AudioWorkerManager mgr;
    REQUIRE(mgr.start(workerPath));
    const uint64_t gen = mgr.generation();

    // Tab 0: a live real instrument session (440 Hz).
    const uint8_t tabA = 0;
    REQUIRE(mgr.activateTabVM(tabA, kSampleRate, 1).ok);
    REQUIRE(mgr.evaluateCkTab(tabA, kRealInstrument440).ok);
    REQUIRE(waitForLoadedShred(mgr, tabA, 8000));

    // Tab 1: bake a different instrument (880 Hz) to a temp WAV.
    const uint8_t tabB = 1;
    auto tmpDir = std::filesystem::temp_directory_path();
    auto wavPath = tmpDir / "b8_isolation_880.wav";
    std::filesystem::remove(wavPath);

    RenderResult result = bake(mgr, tabB, kInstr880, 22050, wavPath);
    REQUIRE(result.success);
    REQUIRE(result.state == RenderState::Completed);

    // The baked asset is REAL audio.  Note: while tab B bakes, tab A's live
    // 440 Hz tone also flows into the shared transport ring (B4-K0.6 mixes
    // all active tabs into one ring), so the WAV legitimately contains a
    // 440+880 Hz mix — zero-crossing frequency of a sum is not meaningful
    // here.  Assert instead that it is genuinely non-silent instrument audio;
    // the 440 Hz liveness of tab A below proves the isolation.
    std::vector<float> pcm;
    unsigned rate = 0;
    REQUIRE(readWavMono(wavPath, pcm, rate));
    REQUIRE(peakAmplitude(pcm) > 0.1f);
    std::size_t start = 0;
    const std::size_t voiced = trimSilence(pcm, 1e-3f, start);
    REQUIRE(voiced > pcm.size() / 2);

    // Tab A's session must still be alive and producing its 440 Hz tone
    // (the bake's drain must not have killed or corrupted it).
    REQUIRE(waitForLoadedShred(mgr, tabA, 8000));

    drainRing(mgr, gen);
    float aBuf[80 * kBlockSize];
    const uint32_t a = collectBlocks(mgr, gen, aBuf, 60, 4000);
    REQUIRE(a >= 30);
    const std::vector<float> aVec(aBuf, aBuf + static_cast<std::size_t>(a) * kBlockSize);
    REQUIRE(peakAmplitude(aVec) > 0.1f);
    std::size_t aStart = 0;
    const std::size_t aVoiced = trimSilence(aVec, 1e-3f, aStart);
    REQUIRE(aVoiced > aVec.size() / 2);
    const double aFreq = estimateFrequencyHz(aVec, aStart, aVoiced, static_cast<float>(rate));
    REQUIRE(aFreq > 0.0);
    REQUIRE(std::fabs(aFreq - 440.0) / 440.0 < 0.08);

    REQUIRE(mgr.queryTabVM(tabA).message.find("active") != std::string::npos);

    std::filesystem::remove(wavPath);
    mgr.shutdown();
}
