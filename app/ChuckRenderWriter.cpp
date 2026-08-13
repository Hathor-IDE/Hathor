// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ChuckRenderWriter.cpp — B8-K2 implementation.
 *
 * See ChuckRenderWriter.hpp for the full design rationale.
 *
 * Requirements: B8-K2, B4-K1, B4-K2, B4-K3, B4-K4, B4-K6, B4-K7
 */

#include "ChuckRenderWriter.hpp"

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// POSIX for open/fsync
#include <fcntl.h>
#include <unistd.h>

namespace hathor {

using audio_worker::kBlockSize;
using audio_worker::kShmName;

// ---------------------------------------------------------------------------
// Internal: WAV file format helpers (minimal, uncompressed PCM)
// ---------------------------------------------------------------------------

namespace {

/// Write a 4-byte little-endian uint32 to an ostream.
void writeU32LE(std::ostream& os, uint32_t v)
{
    char buf[4];
    buf[0] = static_cast<char>(v & 0xFF);
    buf[1] = static_cast<char>((v >> 8) & 0xFF);
    buf[2] = static_cast<char>((v >> 16) & 0xFF);
    buf[3] = static_cast<char>((v >> 24) & 0xFF);
    os.write(buf, 4);
}

/// Write a 2-byte little-endian uint16 to an ostream.
void writeU16LE(std::ostream& os, uint16_t v)
{
    char buf[2];
    buf[0] = static_cast<char>(v & 0xFF);
    buf[1] = static_cast<char>((v >> 8) & 0xFF);
    os.write(buf, 2);
}

/// Write a 4-char FourCC tag.
void writeFourCC(std::ostream& os, const char cc[4])
{
    os.write(cc, 4);
}

/**
 * Write an uncompressed PCM WAV file (16-bit, interleaved float->int16).
 *
 * Layout: RIFF/WAVE/fmt /data chunks.
 * This follows the same format conventions as the existing SampleBank loader
 * (app/SampleBank.cpp) which expects WAV/AIFF/FLAC with standard PCM.
 *
 * @param path        Output file path (a temporary file; the caller renames on success).
 * @param samples     Interleaved float PCM in [-1.0, 1.0].
 * @param numSamples  Total sample frames (NOT total floats — frames).
 * @param channels    1 (mono) or 2 (stereo).
 * @param sampleRate  Sample rate.
 * @param errMsg      Filled with an error message on failure.
 *
 * @return true on success; false on failure.
 */
bool writeWavFile(const std::filesystem::path& path,
                  const std::vector<float>&    samples,
                  uint64_t                     numSamples,
                  unsigned                     channels,
                  unsigned                     sampleRate,
                  std::string&                 errMsg)
{
    if (samples.size() != static_cast<std::size_t>(numSamples) * channels) {
        errMsg = "sample count mismatch: samples.size()=" + std::to_string(samples.size())
               + " expected=" + std::to_string(numSamples * channels);
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        errMsg = "cannot create output directory: " + ec.message();
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        errMsg = "cannot open output file for writing: " + path.string();
        return false;
    }

    const uint32_t kSamplesPerSec  = sampleRate;
    const uint16_t kBitsPerSample   = 16;
    const uint16_t kBlockAlign      = static_cast<uint16_t>(channels * kBitsPerSample / 8);
    const uint32_t kByteRate        = kSamplesPerSec * kBlockAlign;
    const uint32_t kSubchunk2Size   = static_cast<uint32_t>(numSamples * kBlockAlign);
    const uint32_t kChunkSize       = 36 + kSubchunk2Size;

    // --- RIFF header ---
    writeFourCC(out, "RIFF");
    writeU32LE(out, kChunkSize);
    writeFourCC(out, "WAVE");

    // --- fmt sub-chunk ---
    writeFourCC(out, "fmt ");
    writeU32LE(out, 16);               // SubChunk1Size (16 for PCM)
    writeU16LE(out, 1);                // AudioFormat (1 = PCM)
    writeU16LE(out, channels);
    writeU32LE(out, kSamplesPerSec);
    writeU32LE(out, kByteRate);
    writeU16LE(out, kBlockAlign);
    writeU16LE(out, kBitsPerSample);

    // --- data sub-chunk ---
    writeFourCC(out, "data");
    writeU32LE(out, kSubchunk2Size);

    // Write sample data (clamp + dither-free float-to-int16 conversion).
    // This is off the audio thread — allocation and computation are allowed.
    for (uint64_t i = 0; i < numSamples * channels; ++i) {
        float f = samples[static_cast<std::size_t>(i)];
        if (f > 1.0f)  f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        int16_t s = static_cast<int16_t>(std::lroundf(f * 32767.0f));
        char buf[2];
        buf[0] = static_cast<char>(s & 0xFF);
        buf[1] = static_cast<char>((s >> 8) & 0xFF);
        out.write(buf, 2);
    }

    if (!out) {
        errMsg = "error while writing WAV data";
        return false;
    }

    out.flush();
    if (!out) {
        errMsg = "flush failed while writing WAV";
        return false;
    }

    return true;
}

/**
 * Validate a WAV file by reading its header and metadata (B8-K2 §11).
 *
 * This performs a lightweight header parse matching what JUCE's WAVAudioFormat
 * reader expects: RIFF/WAVE/fmt /data structure with PCM audio format 1,
 * 16-bit samples, and a non-zero data size.  When JUCE is available (production
 * app), the SampleBank loader performs the real validation; this header check
 * is used by the non-JUCE test binary.
 *
 * @return true if the file exists and has a structurally valid WAV header.
 */
bool validateWavHeader(const std::filesystem::path& path,
                       unsigned expectedChannels,
                       unsigned expectedSampleRate,
                       uint64_t expectedSamples,
                       std::string& errMsg)
{
    namespace fs = std::filesystem;
    std::error_code ec;

    if (!fs::exists(path, ec) || ec) {
        errMsg = "output file does not exist";
        return false;
    }

    const auto fileSize = fs::file_size(path, ec);
    if (ec || fileSize == 0) {
        errMsg = "output file is empty or inaccessible";
        return false;
    }

    if (fileSize < 44) {
        errMsg = "output file too small to be a valid WAV (< 44 bytes)";
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        errMsg = "cannot open output file for validation";
        return false;
    }

    char fourcc[4];

    // RIFF
    in.read(fourcc, 4);
    if (std::memcmp(fourcc, "RIFF", 4) != 0) {
        errMsg = "missing RIFF marker";
        return false;
    }

    uint32_t chunkSize;
    in.read(reinterpret_cast<char*>(&chunkSize), 4);

    // WAVE
    in.read(fourcc, 4);
    if (std::memcmp(fourcc, "WAVE", 4) != 0) {
        errMsg = "missing WAVE marker";
        return false;
    }

    // --- fmt chunk ---
    in.read(fourcc, 4);
    if (std::memcmp(fourcc, "fmt ", 4) != 0) {
        errMsg = "missing fmt chunk";
        return false;
    }

    uint32_t fmtSize;
    in.read(reinterpret_cast<char*>(&fmtSize), 4);
    if (fmtSize != 16) {
        errMsg = "unexpected fmt chunk size (expected 16 for PCM)";
        return false;
    }

    uint16_t audioFormat;
    in.read(reinterpret_cast<char*>(&audioFormat), 2);
    if (audioFormat != 1) {
        errMsg = "not PCM format (audioFormat != 1)";
        return false;
    }

    uint16_t channels;
    in.read(reinterpret_cast<char*>(&channels), 2);
    if (channels != expectedChannels && expectedChannels > 0) {
        errMsg = "channel count mismatch: got " + std::to_string(channels)
               + ", expected " + std::to_string(expectedChannels);
        return false;
    }
    if (channels == 0) {
        errMsg = "zero channels in WAV header";
        return false;
    }

    uint32_t sampleRate;
    in.read(reinterpret_cast<char*>(&sampleRate), 4);
    if (sampleRate != expectedSampleRate && expectedSampleRate > 0) {
        errMsg = "sample rate mismatch: got " + std::to_string(sampleRate)
               + ", expected " + std::to_string(expectedSampleRate);
        return false;
    }
    if (sampleRate == 0) {
        errMsg = "zero sample rate in WAV header";
        return false;
    }

    uint32_t byteRate;
    in.read(reinterpret_cast<char*>(&byteRate), 4);

    uint16_t blockAlign;
    in.read(reinterpret_cast<char*>(&blockAlign), 2);

    uint16_t bitsPerSample;
    in.read(reinterpret_cast<char*>(&bitsPerSample), 2);
    if (bitsPerSample != 16) {
        errMsg = "unexpected bits-per-sample: " + std::to_string(bitsPerSample) + " (expected 16)";
        return false;
    }

    // --- data chunk ---
    in.read(fourcc, 4);
    if (std::memcmp(fourcc, "data", 4) != 0) {
        errMsg = "missing data chunk (or non-standard chunk before data)";
        return false;
    }

    uint32_t dataSize;
    in.read(reinterpret_cast<char*>(&dataSize), 4);

    if (dataSize == 0) {
        errMsg = "data chunk is zero-length";
        return false;
    }

    // Allow rounding tolerance of up to one block (kBlockSize frames).
    const uint64_t expectedDataSize =
        static_cast<uint64_t>(expectedSamples) * channels * (bitsPerSample / 8);
    const uint64_t tolerance =
        static_cast<uint64_t>(kBlockSize) * channels * (bitsPerSample / 8);
    if (dataSize > expectedDataSize + tolerance) {
        errMsg = "data chunk larger than expected (possible corruption)";
        return false;
    }
    if (dataSize + tolerance < expectedDataSize) {
        errMsg = "data chunk smaller than expected (possible truncation)";
        return false;
    }

    (void)chunkSize;
    (void)byteRate;
    (void)blockAlign;

    in.close();
    return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// RenderJob — internal state for a single background render
// ---------------------------------------------------------------------------

namespace detail {

struct RenderJob {
    uint64_t                                               jobId;
    uint8_t                                                tabId;
    std::string                                            ckSource;
    uint64_t                                               numSamples;
    unsigned                                               sampleRate;
    unsigned                                               channels;
    std::filesystem::path                                  destPath;
    ChuckRenderWriter::CompletionCallback                  onComplete;

    // Shared state for lifecycle observation and cancellation.
    std::shared_ptr<std::atomic<RenderState>>  state;
    std::shared_ptr<std::atomic<bool>>         cancelFlag;
    std::shared_ptr<std::atomic<uint64_t>>     samplesProduced;

    // The background render thread (started in startRender, joined in shutdown).
    std::thread thread;

    RenderJob()
        : jobId(0)
        , tabId(0)
        , numSamples(0)
        , sampleRate(44100)
        , channels(1)
        , state(std::make_shared<std::atomic<RenderState>>(RenderState::Pending))
        , cancelFlag(std::make_shared<std::atomic<bool>>(false))
        , samplesProduced(std::make_shared<std::atomic<uint64_t>>(0))
    {}
};

} // namespace detail

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ChuckRenderWriter::ChuckRenderWriter(AudioWorkerManager* worker) noexcept
    : worker_(worker)
{
}

ChuckRenderWriter::~ChuckRenderWriter()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// B8-K2 §1: startRender — non-blocking entry point
// ---------------------------------------------------------------------------

RenderHandle ChuckRenderWriter::startRender(
    uint8_t                    tabId,
    std::string                ckSource,
    uint64_t                   numSamples,
    unsigned                   sampleRate,
    std::filesystem::path      destPath,
    CompletionCallback         onComplete)
{
    // B8-K2 §4: invalid duration must fail clearly.
    if (numSamples == 0) {
        if (onComplete) {
            onComplete(RenderResult{
                .success = false,
                .state = RenderState::Failed,
                .errorMessage = "invalid duration: numSamples is zero",
                .outputPath = destPath,
            });
        }
        return RenderHandle{};
    }

    // B8-K2 §6: destination must be non-empty.
    if (destPath.empty()) {
        if (onComplete) {
            onComplete(RenderResult{
                .success = false,
                .state = RenderState::Failed,
                .errorMessage = "invalid destination: path is empty",
            });
        }
        return RenderHandle{};
    }

    const uint64_t jobId = nextJobId_.fetch_add(1, std::memory_order_relaxed);

    auto job = std::make_shared<detail::RenderJob>();
    job->jobId = jobId;
    job->tabId = tabId;
    job->ckSource = std::move(ckSource);
    job->numSamples = numSamples;
    job->sampleRate = sampleRate;
    job->channels = 1; // ChucK mono instrument → mono WAV
    job->destPath = std::move(destPath);
    job->onComplete = std::move(onComplete);

    // Register for tracking (under lock).
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        jobs_.push_back(job);
    }

    // Spawn the background render thread.
    job->thread = std::thread(&ChuckRenderWriter::runRender, this, job);

    return RenderHandle(jobId, job->state, job->cancelFlag, job);
}

// ---------------------------------------------------------------------------
// B8-K2 §8: Background-task lifecycle
// ---------------------------------------------------------------------------

int ChuckRenderWriter::activeRenderCount() const noexcept
{
    std::lock_guard<std::mutex> lock(jobsMtx_);
    int count = 0;
    for (const auto& j : jobs_) {
        const RenderState s = j->state->load(std::memory_order_acquire);
        if (s != RenderState::Completed &&
            s != RenderState::Failed &&
            s != RenderState::Cancelled &&
            s != RenderState::Pending) {
            ++count;
        }
    }
    return count;
}

void ChuckRenderWriter::shutdown() noexcept
{
    // Signal all in-flight renders to cancel, then join their threads.
    std::vector<std::shared_ptr<detail::RenderJob>> jobsToJoin;
    {
        std::lock_guard<std::mutex> lock(jobsMtx_);
        for (auto& j : jobs_) {
            if (j->cancelFlag)
                j->cancelFlag->store(true, std::memory_order_release);
            jobsToJoin.push_back(j);
        }
    }

    for (auto& job : jobsToJoin) {
        if (job->thread.joinable())
            job->thread.join();
    }

    std::lock_guard<std::mutex> lock(jobsMtx_);
    jobs_.clear();
}

// ---------------------------------------------------------------------------
// B8-K2 main render loop — runs entirely on the background thread
// ---------------------------------------------------------------------------

void ChuckRenderWriter::runRender(std::shared_ptr<detail::RenderJob> job)
{
    const uint64_t jobId = job->jobId;
    (void)jobId;
    const uint8_t  tabId = job->tabId;
    const uint64_t numSamples = job->numSamples;
    const unsigned sampleRate = job->sampleRate;
    const unsigned channels = job->channels;
    const auto destPath = job->destPath;

    job->state->store(RenderState::Rendering, std::memory_order_release);

    RenderResult result;
    result.outputPath = destPath;
    result.state = RenderState::Rendering;

    // -----------------------------------------------------------------------
    // B8-K2 §1: Precondition — worker must be alive.
    // -----------------------------------------------------------------------
    if (!worker_ || !worker_->isWorkerAlive()) {
        result.success = false;
        result.state = RenderState::Failed;
        result.errorMessage = "audio worker is not running";
        job->state->store(RenderState::Failed, std::memory_order_release);
        if (job->onComplete)
            job->onComplete(result);
        return;
    }

    // -----------------------------------------------------------------------
    // B8-K2 §3: Activate the per-tab VM for this render.
    // -----------------------------------------------------------------------
    auto activateResult = worker_->activateTabVM(tabId, sampleRate, channels);
    if (!activateResult.ok) {
        result.success = false;
        result.state = RenderState::Failed;
        result.errorMessage = "failed to activate VM for tab " + std::to_string(tabId)
                            + ": " + activateResult.message;
        job->state->store(RenderState::Failed, std::memory_order_release);
        if (job->onComplete)
            job->onComplete(result);
        return;
    }

    // -----------------------------------------------------------------------
    // B8-K2 / B4-K7: Compile the ChucK source (serialized dispatcher).
    // -----------------------------------------------------------------------
    auto compileResult = worker_->evaluateCkTab(tabId, job->ckSource);
    if (!compileResult.ok) {
        result.success = false;
        result.state = RenderState::Failed;
        result.errorMessage = "ChucK compile failed: " + compileResult.message;
        job->state->store(RenderState::Failed, std::memory_order_release);
        worker_->destroyTabVM(tabId);
        if (job->onComplete)
            job->onComplete(result);
        return;
    }

    // -----------------------------------------------------------------------
    // B4-K4: Wait for the shred to actually START EXECUTING in the real VM.
    //
    // evaluateCkTab() returns as soon as the dispatcher has validated the
    // source — the shred is compiled+sporked asynchronously on the per-tab
    // VM thread (instance creation + compile + first run() step can take
    // ~50-200 ms).  Draining immediately would capture only pre-execution
    // silence.  We poll vm_query until the VM reports a real loaded shred id
    // (>= 0).  A program that compiles but never starts executing must NOT
    // be reported as a successful bake (B4-K7 / audit honesty requirement).
    //
    // Fresh-VM assumption: the bake flow activates the render's own tab VM
    // (or recreates one) and destroys it after every render, so the shred id
    // observed here belongs to THIS render's program (the VM's removeAllShreds
    // replace semantics also guarantee the previous shred is gone before the
    // new one sporks).  Reusing a tab that still hosts a live shred is not a
    // supported render flow.
    // -----------------------------------------------------------------------
    {
        const uint64_t genBeforeWait = worker_->generation();
        auto loadDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(10);
        bool loaded = false;
        bool cancelledDuringWait = false;
        bool workerDiedDuringWait = false;
        while (std::chrono::steady_clock::now() < loadDeadline) {
            if (job->cancelFlag->load(std::memory_order_acquire)) {
                cancelledDuringWait = true;
                break;
            }
            // Short-circuit if the worker dies or restarts mid-load: polling
            // a dead worker would otherwise burn the full 10 s budget.
            if (!worker_->isWorkerAlive() ||
                worker_->generation() != genBeforeWait) {
                workerDiedDuringWait = true;
                break;
            }
            std::string q = worker_->queryTabVM(tabId).message;
            auto pos = q.find("shred_id=");
            if (pos != std::string::npos) {
                try {
                    if (std::stol(q.substr(pos + 9)) >= 0) {
                        loaded = true;
                        break;
                    }
                } catch (...) { /* keep polling */ }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        if (!loaded && workerDiedDuringWait) {
            result.success = false;
            result.state = RenderState::Failed;
            result.errorMessage = "audio worker died or restarted while loading the "
                                  "ChucK program";
            job->state->store(RenderState::Failed, std::memory_order_release);
            worker_->destroyTabVM(tabId);
            if (job->onComplete)
                job->onComplete(result);
            return;
        }
        if (!loaded) {
            // B8-K2 §12: cancellation during the load wait must surface as
            // Cancelled (not Failed) and never publish a partial WAV.
            if (cancelledDuringWait) {
                job->state->store(RenderState::Cancelled, std::memory_order_release);
                worker_->destroyTabVM(tabId);
                if (job->onComplete) {
                    result.success = false;
                    result.state = RenderState::Cancelled;
                    result.errorMessage = "render cancelled by caller";
                    result.samplesWritten = 0;
                    job->onComplete(result);
                }
                return;
            }
            result.success = false;
            result.state = RenderState::Failed;
            result.errorMessage = "ChucK program compiled but did not start executing "
                                  "(shred never loaded)";
            job->state->store(RenderState::Failed, std::memory_order_release);
            worker_->destroyTabVM(tabId);
            if (job->onComplete)
                job->onComplete(result);
            return;
        }
    }

    // -----------------------------------------------------------------------
    // B8-K2 §1: Drain audio from the shared-memory ring.
    // -----------------------------------------------------------------------
    const uint64_t gen = worker_->generation();

    // -----------------------------------------------------------------------
    // B4-K4: discard stale PRE-EXECUTION silence.  Between evaluateCkTab()
    // and the shred's first real output, the VM thread produces silence
    // blocks (instance creation + compile can take ~50-200 ms).  Those
    // blocks sit in the ring and would otherwise lead the WAV (or, for a
    // short render, fill it entirely).  After the shred is confirmed loaded,
    // discard silent blocks until we reach the first non-silent block — which
    // is SAVED and becomes the head of the WAV — or a bounded discard budget
    // is exhausted (keeps the render honest and bounded even for instruments
    // with a genuine silent intro).
    // -----------------------------------------------------------------------
    std::vector<float> pcmBuffer;
    pcmBuffer.reserve(static_cast<std::size_t>(numSamples) * channels + kBlockSize);
    {
        constexpr float kSilenceThreshold = 1e-5f;
        // Upper bound on how much stale silence we are willing to skip:
        // generous (up to ~4 s of transport time) but finite.
        constexpr uint64_t kMaxDiscardSamples = 4u * 44100u;
        uint64_t discarded = 0;
        while (discarded < kMaxDiscardSamples &&
               !job->cancelFlag->load(std::memory_order_acquire)) {
            float blockBuf[kBlockSize];
            if (!worker_->tryReadAudioBlock(blockBuf, kBlockSize, gen))
                break; // ring currently empty — start collecting as-is
            float peak = 0.0f;
            for (unsigned i = 0; i < kBlockSize; ++i) {
                const float a = (blockBuf[i] < 0.0f) ? -blockBuf[i] : blockBuf[i];
                if (a > peak) peak = a;
            }
            if (peak >= kSilenceThreshold) {
                // First real audio — save it as the head of the WAV.
                pcmBuffer.insert(pcmBuffer.end(), blockBuf, blockBuf + kBlockSize);
                break;
            }
            discarded += kBlockSize;
        }
    }

    uint64_t samplesCollected = static_cast<uint64_t>(pcmBuffer.size());

    while (samplesCollected < numSamples && !job->cancelFlag->load(std::memory_order_acquire)) {
        // Check generation liveness — if the worker was restarted, stop.
        if (worker_->generation() != gen) {
            result.success = false;
            result.state = RenderState::Failed;
            result.errorMessage = "audio worker generation changed during render";
            job->state->store(RenderState::Failed, std::memory_order_release);
            worker_->destroyTabVM(tabId);
            if (job->onComplete)
                job->onComplete(result);
            return;
        }

        float blockBuf[kBlockSize];
        if (worker_->tryReadAudioBlock(blockBuf, kBlockSize, gen)) {
            const unsigned toCopy = (numSamples - samplesCollected < kBlockSize)
                                      ? static_cast<unsigned>(numSamples - samplesCollected)
                                      : kBlockSize;
            pcmBuffer.insert(pcmBuffer.end(), blockBuf, blockBuf + toCopy);
            samplesCollected += toCopy;
            job->samplesProduced->store(samplesCollected, std::memory_order_release);
        } else {
            // No data available yet — sleep briefly and retry.
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    // Check if we were cancelled while draining.
    if (job->cancelFlag->load(std::memory_order_acquire)) {
        // B8-K2 §12: cancellation must NOT publish a partial WAV.
        job->state->store(RenderState::Cancelled, std::memory_order_release);
        worker_->destroyTabVM(tabId);
        if (job->onComplete) {
            result.success = false;
            result.state = RenderState::Cancelled;
            result.errorMessage = "render cancelled by caller";
            result.samplesWritten = samplesCollected;
            result.durationSeconds = static_cast<double>(samplesCollected)
                                     / static_cast<double>(sampleRate);
            job->onComplete(result);
        }
        return;
    }

    // Pad to exact numSamples if we got slightly fewer (block alignment).
    // B8-K2 §4: produce exactly the requested duration.
    if (samplesCollected < numSamples) {
        const uint64_t padCount = numSamples - samplesCollected;
        pcmBuffer.insert(pcmBuffer.end(), padCount, 0.0f);
        samplesCollected = numSamples;
    }

    // -----------------------------------------------------------------------
    // B4-K7 / audit: a bake must not silently succeed with a silent WAV.
    // If the whole captured region is digital silence, the instrument
    // produced no audible output (e.g. crashed at runtime, or nothing was
    // routed to dac).  Surface it as a render failure instead of publishing
    // unrelated/silent audio.
    // -----------------------------------------------------------------------
    {
        float peak = 0.0f;
        for (float s : pcmBuffer) {
            const float a = (s < 0.0f) ? -s : s;
            if (a > peak) peak = a;
        }
        if (peak < 1e-5f) {
            result.success = false;
            result.state = RenderState::Failed;
            result.errorMessage = "ChucK program produced no audio output "
                                  "(render captured silence)";
            job->state->store(RenderState::Failed, std::memory_order_release);
            worker_->destroyTabVM(tabId);
            if (job->onComplete)
                job->onComplete(result);
            return;
        }
    }

    job->samplesProduced->store(samplesCollected, std::memory_order_release);

    // -----------------------------------------------------------------------
    // B8-K2 §7: Atomic file publication.
    // -----------------------------------------------------------------------
    job->state->store(RenderState::Writing, std::memory_order_release);
    result.state = RenderState::Writing;

    const std::filesystem::path tempFile =
        destPath.string() + ".tmp";

    // Remove any stale temp file.
    std::error_code ec;
    std::filesystem::remove(tempFile, ec);

    std::string writeError;
    const bool writeOk = writeWavFile(tempFile, pcmBuffer,
                                      numSamples, channels, sampleRate,
                                      writeError);

    if (!writeOk) {
        std::filesystem::remove(tempFile, ec);
        result.success = false;
        result.state = RenderState::Failed;
        result.errorMessage = "WAV write failed: " + writeError;
        job->state->store(RenderState::Failed, std::memory_order_release);
        worker_->destroyTabVM(tabId);
        if (job->onComplete)
            job->onComplete(result);
        return;
    }

    // B8-K2 §11: Verify the WAV is valid before publishing.
    std::string validateError;
    if (!validateWavHeader(tempFile, channels, sampleRate, numSamples, validateError)) {
        std::filesystem::remove(tempFile, ec);
        result.success = false;
        result.state = RenderState::Failed;
        result.errorMessage = "WAV validation failed: " + validateError;
        job->state->store(RenderState::Failed, std::memory_order_release);
        worker_->destroyTabVM(tabId);
        if (job->onComplete)
            job->onComplete(result);
        return;
    }

    // fsync the temp file so the data is durable before rename.
    {
        int fd = ::open(tempFile.c_str(), O_RDONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }

    // Atomic rename: temp -> final path.
    std::error_code renameEc;
    std::filesystem::rename(tempFile, destPath, renameEc);

    if (renameEc) {
        // rename across filesystems can fail — try copy+remove as fallback.
        std::error_code copyEc;
        std::filesystem::copy_file(tempFile, destPath,
                                   std::filesystem::copy_options::overwrite_existing,
                                   copyEc);
        if (copyEc) {
            std::filesystem::remove(tempFile, ec);
            result.success = false;
            result.state = RenderState::Failed;
            result.errorMessage = "failed to publish WAV: "
                + renameEc.message() + " / " + copyEc.message();
            job->state->store(RenderState::Failed, std::memory_order_release);
            worker_->destroyTabVM(tabId);
            if (job->onComplete)
                job->onComplete(result);
            return;
        }
        std::filesystem::remove(tempFile, ec);
    }

    // Final existence check (B8-K2 §11).
    if (!std::filesystem::exists(destPath, ec)) {
        result.success = false;
        result.state = RenderState::Failed;
        result.errorMessage = "WAV published but destination file does not exist after rename";
        job->state->store(RenderState::Failed, std::memory_order_release);
        worker_->destroyTabVM(tabId);
        if (job->onComplete)
            job->onComplete(result);
        return;
    }

    // -----------------------------------------------------------------------
    // B8-K2 §3: Shut down the VM after a successful render (B8-K3).
    // -----------------------------------------------------------------------
    worker_->destroyTabVM(tabId);

    // -----------------------------------------------------------------------
    // Success!
    // -----------------------------------------------------------------------
    result.success = true;
    result.state = RenderState::Completed;
    result.errorMessage = "";
    result.outputPath = destPath;
    result.samplesWritten = samplesCollected;
    result.durationSeconds = static_cast<double>(samplesCollected)
                             / static_cast<double>(sampleRate);

    job->state->store(RenderState::Completed, std::memory_order_release);
    job->samplesProduced->store(samplesCollected, std::memory_order_release);

    if (job->onComplete)
        job->onComplete(result);

    // Remove ourselves from the tracking list.
    return;
}

} // namespace hathor
