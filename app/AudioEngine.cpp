// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "AudioEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "hathor/Arc.hpp"
#include "hathor/Rational.hpp"

#include "AssetPathResolver.hpp"
#include "LiveJamSessionManager.hpp"

// Bring hathor-namespace asset types into scope for AudioEngine's
// implementation (the facade header already brings AssetTarget in).
using hathor::AssetPathResolver;

// ---------------------------------------------------------------------------
// WAV duration helper (read-only) — parses the 'fact' or 'data' chunk to
// determine duration without full decode.  Returns 0.0 on any error.
// Uses JUCE's AudioFormatManager for WAV parsing.
// ---------------------------------------------------------------------------

double AudioEngine::wavDurationSeconds(const std::filesystem::path& wavPath) noexcept
{
    if (wavPath.empty())
        return 0.0;

    std::error_code ec;
    if (!std::filesystem::exists(wavPath, ec))
        return 0.0;

    try {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        juce::File juceFile(juce::String(wavPath.string()));
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juceFile));
        if (!reader)
            return 0.0;

        const double rate = reader->sampleRate;
        if (rate <= 0.0)
            return 0.0;

        return static_cast<double>(reader->lengthInSamples) / rate;
    } catch (...) {
        return 0.0;
    }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

AudioEngine::AudioEngine(const SampleBank& bank)
    : bank_(bank)
{
    // Initialise all slot shared_ptrs to nullptr (default).
    for (int i = 0; i < kNumSlots; ++i) {
        std::atomic_store_explicit(&slots_[i],
                                   std::shared_ptr<SlotState>{},
                                   std::memory_order_relaxed);
        slotNames_[i].clear();
    }
}

AudioEngine::~AudioEngine()
{
    // Shut down the worker process before releasing resources.
    shutdownWorker();
    // Shut down the device before releasing resources.
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
    closeCapture();
}

// ---------------------------------------------------------------------------
// B4-K3: Worker process management
// ---------------------------------------------------------------------------

std::string AudioEngine::startWorker(const std::string& workerPath)
{
    if (workerPath.empty())
        return "worker path is empty";

    workerMgr_ = std::make_unique<hathor::AudioWorkerManager>();
    if (!workerMgr_->start(workerPath))
        return "failed to start worker: " + workerMgr_->getLastError();

    // Create the render writer bound to the worker manager.
    renderWriter_ = std::make_unique<hathor::ChuckRenderWriter>(workerMgr_.get());

    // Configure resource policy.
    hathor::AudioWorkerManager::ResourceLimits limits;
    limits.maxVms = hathor::audio_worker::kNumTabs;
    limits.maxThreads = 32;
    limits.maxVmMemoryMb = 256;
    workerMgr_->setResourceLimits(limits);

    // Record the worker's initial generation so the audio callback can reject
    // stale samples after a worker restart.
    workerGeneration_.store(workerMgr_->generation(), std::memory_order_release);

    return "";
}

void AudioEngine::shutdownWorker() noexcept
{
    if (renderWriter_) {
        renderWriter_->shutdown();
        renderWriter_.reset();
    }
    if (workerMgr_) {
        workerMgr_->shutdown();
        workerMgr_.reset();
    }
}

// ---------------------------------------------------------------------------
// Device management (Req 8.1–8.5)
// ---------------------------------------------------------------------------

std::string AudioEngine::initialise()
{
    // Register the callback BEFORE opening the device so the callback is set up
    // when audioDeviceAboutToStart fires.
    deviceManager_.addAudioCallback(this);

    // Open the default output device with stereo output, no input.
    // The empty AudioDeviceSetup requests system defaults.
    juce::String error = deviceManager_.initialise(
        /*numInputChannels=*/  0,
        /*numOutputChannels=*/ 2,
        /*savedState=*/        nullptr,
        /*selectDefaultDeviceOnFailure=*/ true
    );

    if (error.isNotEmpty()) {
        deviceManager_.removeAudioCallback(this);
        return error.toStdString();
    }

    // Report actual device settings to stderr (Req 8.5).
    if (auto* device = deviceManager_.getCurrentAudioDevice()) {
        const double actualRate   = device->getCurrentSampleRate();
        const int    actualBuffer = device->getCurrentBufferSizeSamples();
        std::fprintf(stderr,
                     "[AudioEngine] Device opened: %s | %.0f Hz | %d samples/buffer\n",
                     device->getName().toRawUTF8(),
                     actualRate,
                     actualBuffer);
        std::fflush(stderr);

        // Update our atomic sample rate so the audio callback uses the real value.
        sampleRate_.store(static_cast<int>(actualRate), std::memory_order_relaxed);
    }

    return {}; // success
}

// ---------------------------------------------------------------------------
// File capture (Req 20.1/20.2 automated regression path)
// ---------------------------------------------------------------------------

std::string AudioEngine::openCapture(const std::string& path)
{
    const int rate = sampleRate_.load(std::memory_order_relaxed);
    if (rate <= 0)
        return "openCapture called before sample rate is known (call after initialise())";

    juce::File outFile(path);
    outFile.getParentDirectory().createDirectory();

    auto stream = std::make_unique<juce::FileOutputStream>(outFile);
    if (stream->failedToOpen())
        return std::string("Could not open capture file: ") + path;

    // Overwrite any existing file.
    stream->setPosition(0);
    stream->truncate();

    // Create a stereo 16-bit PCM WAV writer at the actual device rate.
    // Use WavAudioFormat directly — no AudioFormatManager needed.
    juce::WavAudioFormat wavFormat;
    auto* writer = wavFormat.createWriterFor(
        stream.get(),
        static_cast<double>(rate),
        /*numChannels=*/    2,
        /*bitsPerSample=*/  16,
        /*metadataValues=*/ {},
        /*qualityOption=*/  0
    );
    if (!writer) {
        return std::string("WavAudioFormat::createWriterFor failed for: ") + path;
    }

    // writer takes ownership of the stream; release our unique_ptr.
    (void)stream.release();
    captureWriter_.reset(writer);
    captureOpen_.store(true, std::memory_order_release);

    std::fprintf(stderr, "[AudioEngine] Capturing audio to: %s\n", path.c_str());
    std::fflush(stderr);
    return {};
}

void AudioEngine::closeCapture()
{
    captureOpen_.store(false, std::memory_order_release);
    captureWriter_.reset();   // flushes and closes the WAV file
}

// ---------------------------------------------------------------------------
// Transport (Req 14.1–14.5)
// ---------------------------------------------------------------------------

void AudioEngine::play() noexcept
{
    running_.store(true, std::memory_order_relaxed);
}

void AudioEngine::stop() noexcept
{
    running_.store(false, std::memory_order_relaxed);
    // Immediately silence all playing voices (Req 14.2).
    voicePool_.silenceAll();
}

void AudioEngine::setBpm(double bpm) noexcept
{
    // Clamp to [20, 400] (Req 14.3, 14.4).
    bpm = std::clamp(bpm, 20.0, 400.0);
    bpm_.store(bpm, std::memory_order_relaxed);
}

double AudioEngine::getBpm() const noexcept
{
    return bpm_.load(std::memory_order_relaxed);
}

bool AudioEngine::isRunning() const noexcept
{
    return running_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Per-slot play/stop (A3)
// ---------------------------------------------------------------------------

void AudioEngine::slotPlay(int slotIdx) noexcept
{
    auto state = loadSlot(slotIdx);
    if (!state)
        return;
    state->running.store(true, std::memory_order_release);

    // B4-K3: Activate the per-tab ChucK VM in the worker process.
    if (workerMgr_) {
        workerMgr_->activateTabVM(static_cast<uint8_t>(slotIdx),
                                   sampleRate_.load(std::memory_order_acquire));
    }
}

void AudioEngine::slotStop(int slotIdx) noexcept
{
    auto state = loadSlot(slotIdx);
    if (!state)
        return;
    state->running.store(false, std::memory_order_release);
    // Silence any voices currently playing from this slot (A3).
    voicePool_.silenceSlot(static_cast<int8_t>(slotIdx));

    // B4-K3: Suspend the per-tab ChucK VM (keeps state for fast resume).
    if (workerMgr_) {
        workerMgr_->deactivateTabVM(static_cast<uint8_t>(slotIdx), /*suspend=*/true);
    }
}

bool AudioEngine::isSlotRunning(int slotIdx) const noexcept
{
    auto state = loadSlot(slotIdx);
    if (!state)
        return false;
    return state->running.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Master gain (Req 26.5, 26.6)
// ---------------------------------------------------------------------------

void AudioEngine::setMasterGain(float g) noexcept
{
    // Clamp to [0.0, 2.0]; relaxed ordering — continuous fader, no sync dep.
    masterGain_.store(std::clamp(g, 0.f, 2.f), std::memory_order_relaxed);
}

float AudioEngine::getMasterGain() const noexcept
{
    return masterGain_.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// B7-K2: Master-bus preset EQ (decision #13 signal chain)
// ---------------------------------------------------------------------------

void AudioEngine::setMasterEqPreset(hathor::EqPreset preset) noexcept
{
    // --- Worker/control thread path ---
    //
    // 1. Compute the complete preset coefficients from the preset spec.
    // 2. Construct the replacement EQ state (zeroed delay lines).
    // 3. Atomically publish the complete state.
    //
    // The audio thread will see the new state on its next atomic_load.
    // No partial state is ever observed.  No mutex, no allocation on the
    // audio thread (allocation happens here on the control thread).
    //
    // B7-K2 §4, §5, §6

    const int sr = sampleRate_.load(std::memory_order_acquire);
    if (sr <= 0)
        return; // device not yet open — preset will be applied on device start

    // Build the complete replacement state BEFORE publishing.
    auto newState = hathor::MasterEqState::create(preset, sr);
    if (!newState)
        return;

    // Atomic publication: release store so the audio thread's acquire load
    // sees the fully-constructed state.
    std::atomic_store_explicit(&activeEqState_, std::move(newState),
                               std::memory_order_release);
    eqPreset_.store(static_cast<int>(preset), std::memory_order_relaxed);
}

hathor::EqPreset AudioEngine::getMasterEqPreset() const noexcept
{
    return static_cast<hathor::EqPreset>(eqPreset_.load(std::memory_order_relaxed));
}

std::shared_ptr<hathor::MasterEqState> AudioEngine::loadEqState() const noexcept
{
    return std::atomic_load_explicit(&activeEqState_, std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// Hot-swap slot API
// ---------------------------------------------------------------------------

int AudioEngine::findOrAddSlot(const std::string& name)
{
    // First check if name is already registered.
    for (int i = 0; i < slotNameCount_; ++i) {
        if (slotNames_[i] == name)
            return i;
    }

    // Not found — register if there's room.
    if (slotNameCount_ >= kNumSlots)
        return -1;

    const int idx        = slotNameCount_++;
    slotNames_[idx]      = name;
    return idx;
}

void AudioEngine::storeSlot(int idx, std::shared_ptr<SlotState> state) noexcept
{
    if (idx < 0 || idx >= kNumSlots)
        return;
    // Worker thread: release ordering so the audio thread sees the full SlotState
    // before it reads through the loaded pointer (Req 11.1, 11.2).
    std::atomic_store_explicit(&slots_[idx], std::move(state), std::memory_order_release);
}

bool AudioEngine::clearSlot(int idx) noexcept
{
    if (idx < 0 || idx >= kNumSlots)
        return false;
    std::atomic_store_explicit(&slots_[idx],
                               std::shared_ptr<SlotState>{},
                               std::memory_order_release);
    return true;
}

int AudioEngine::slotCount() const noexcept
{
    return slotNameCount_;
}

std::string AudioEngine::slotName(int idx) const
{
    if (idx < 0 || idx >= slotNameCount_)
        return {};
    return slotNames_[idx];
}

std::shared_ptr<SlotState> AudioEngine::loadSlot(int idx) const noexcept
{
    if (idx < 0 || idx >= kNumSlots)
        return nullptr;
    return std::atomic_load_explicit(&slots_[idx], std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// B4-K7: Per-tab ChucK VM evaluation
// ---------------------------------------------------------------------------

bool AudioEngine::hasWorker() const noexcept
{
    return workerMgr_ != nullptr && workerMgr_->isWorkerAlive();
}

bool AudioEngine::ckEval(int slotIdx, const std::string& code) noexcept
{
    if (slotIdx < 0 || slotIdx >= kNumSlots)
        return false;

    if (!workerMgr_)
        return false;

    auto result = workerMgr_->evaluateCkTab(
        static_cast<uint8_t>(slotIdx), code);

    return result.ok;
}

bool AudioEngine::stopCkTab(int slotIdx) noexcept
{
    if (slotIdx < 0 || slotIdx >= kNumSlots)
        return false;

    if (!workerMgr_)
        return false;

    auto result = workerMgr_->stopCkTab(static_cast<uint8_t>(slotIdx));
    return result.ok;
}

std::string AudioEngine::queryCkTab(int slotIdx) const
{
    if (slotIdx < 0 || slotIdx >= kNumSlots)
        return "";

    if (!workerMgr_)
        return "";

    auto result = workerMgr_->queryTabVM(static_cast<uint8_t>(slotIdx));
    return result.message;
}

// ---------------------------------------------------------------------------
// juce::AudioIODeviceCallback — device lifecycle
// ---------------------------------------------------------------------------

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (!device)
        return;

    const int rate = static_cast<int>(device->getCurrentSampleRate());
    sampleRate_.store(rate, std::memory_order_relaxed);

    // B7-K2: Initialize the EQ state now that the real sample rate is known.
    // If a preset was already selected via setMasterEqPreset() before the
    // device opened (the sample rate was a placeholder), re-apply it with
    // the correct rate.  If no preset was selected yet, this publishes Flat.
    const hathor::EqPreset currentPreset =
        static_cast<hathor::EqPreset>(eqPreset_.load(std::memory_order_relaxed));
    setMasterEqPreset(currentPreset);

    // Reset the transport clock on device (re)start so it begins from zero.
    sampleClock_.store(0, std::memory_order_relaxed);
}

void AudioEngine::audioDeviceStopped()
{
    // Silence all voices when the device closes so no ghost state remains.
    voicePool_.silenceAll();
}

// ---------------------------------------------------------------------------
// juce::AudioIODeviceCallback — audio callback (Req 7.4, 9.1–9.5)
// ---------------------------------------------------------------------------
// This runs on the real-time audio thread. Rules:
//   - No mutex acquisition
//   - No heap allocation
//   - No file I/O
//   - Atomic loads/stores only for shared state
// ---------------------------------------------------------------------------

void AudioEngine::audioDeviceIOCallbackWithContext(
        const float* const* /*inputChannelData*/,
        int                 /*numInputChannels*/,
        float* const*          outputChannelData,
        int                    numOutputChannels,
        int                    numSamples,
        const juce::AudioIODeviceCallbackContext& /*context*/)
{
    // Obtain pointers to the left and right output channels.
    // Guard against devices that open with fewer than 2 channels.
    float* left  = (numOutputChannels > 0) ? outputChannelData[0] : nullptr;
    float* right = (numOutputChannels > 1) ? outputChannelData[1] : left;

    // Zero the output buffer first so we can safely accumulate into it.
    if (left)  std::memset(left,  0, static_cast<std::size_t>(numSamples) * sizeof(float));
    if (right && right != left)
               std::memset(right, 0, static_cast<std::size_t>(numSamples) * sizeof(float));

    // If not running, produce silence (Req 7.5) and advance the clock anyway
    // so the clock stays monotonic.
    const bool running = running_.load(std::memory_order_relaxed);

    if (!running) {
        // Clock still advances (keeps sampleClock_ monotonic for when we resume).
        sampleClock_.fetch_add(static_cast<uint64_t>(numSamples),
                               std::memory_order_relaxed);
        return;
    }

    // ------------------------------------------------------------------
    // Step 1: Load transport state exactly once per callback (Req 9.4, 9.5).
    // ------------------------------------------------------------------
    const uint64_t clockNow    = sampleClock_.load(std::memory_order_relaxed);
    const double   bpm         = bpm_.load(std::memory_order_relaxed);
    const int      sampleRate  = sampleRate_.load(std::memory_order_relaxed);

    if (sampleRate <= 0) {
        // Device not yet properly initialised — skip this callback.
        sampleClock_.fetch_add(static_cast<uint64_t>(numSamples),
                               std::memory_order_relaxed);
        return;
    }

    // ------------------------------------------------------------------
    // Step 2: Compute buffer Arc in cycle-time using exact Rational arithmetic.
    //
    //   cycle_position = (sampleClock / sampleRate) * (bpm / 60)
    //                  = sampleClock * bpm / (sampleRate * 60)
    //
    // We represent BPM as an integer multiple of 1/1000 to keep Rational exact.
    // However for phase 1 the BPM is always a whole or half-integer value, so
    // we convert to int64_t * 1000 and use denominator 60000 to keep the
    // arithmetic exact for all [20..400] BPM values with up to 3 decimal places.
    //
    // Req 9.1, 9.4: one conversion per callback, never accumulated.
    // ------------------------------------------------------------------

    // BPM represented as a rational: bpm/60.
    // To stay in int64_t territory, scale bpm by 1000 (3 decimal places).
    const int64_t bpmMillis      = static_cast<int64_t>(std::llround(bpm * 1000.0));
    const int64_t denominator    = static_cast<int64_t>(sampleRate) * 60000LL;

    // cycleStart = clockNow * bpmMillis / denominator
    hathor::Rational cycleStart{
        static_cast<int64_t>(clockNow) * bpmMillis,
        denominator
    };

    // cycleEnd = (clockNow + numSamples) * bpmMillis / denominator
    hathor::Rational cycleEnd{
        (static_cast<int64_t>(clockNow) + static_cast<int64_t>(numSamples)) * bpmMillis,
        denominator
    };

    hathor::Arc bufferArc{cycleStart, cycleEnd};

    // samplesPerCycle = sampleRate * 60 / bpm  (double — used only for the
    // final sample-offset conversion, never accumulated across callbacks).
    const double samplesPerCycle = static_cast<double>(sampleRate) * 60.0 / bpm;

    // ------------------------------------------------------------------
    // Step 3: For each slot, query the pattern and schedule voices.
    // Accumulate fired events for the visualizer ring buffer (Req 28.3, 28.8).
    // Use raw aligned storage so we avoid default-constructing Event<ParamMap>
    // (Rational has no default constructor). Only indices [0, firedEventCount)
    // are constructed; we explicitly destroy them after the write.
    // ------------------------------------------------------------------
    alignas(hathor::Event<hathor::ParamMap>)
        std::byte firedEventStorage[hathor::kMaxFrameEvents
                                    * sizeof(hathor::Event<hathor::ParamMap>)];
    auto* firedEvents = reinterpret_cast<hathor::Event<hathor::ParamMap>*>(
                            firedEventStorage);
    uint32_t firedEventCount = 0;

    for (int i = 0; i < kNumSlots; ++i) {
        // Acquire-load: ensures we see a fully-constructed SlotState (Req 11.2).
        std::shared_ptr<SlotState> state =
            std::atomic_load_explicit(&slots_[i], std::memory_order_acquire);

         if (!state || !state->pattern)
             continue;

         // Per-slot running check (A3): skip voice triggering for slots
         // that have been individually stopped.  This is a relaxed atomic
         // read — no allocation, no mutex, real-time safe.
         if (!state->running.load(std::memory_order_acquire))
             continue;

         // Query into the pre-sized event buffer (zero allocation — Req 7.1, 7.2).
         std::span<hathor::Event<hathor::ParamMap>> span{state->eventBuffer};
         const std::size_t count = state->pattern->query(bufferArc, span);

         // Schedule each event.
         for (std::size_t j = 0; j < count; ++j) {
             const hathor::Event<hathor::ParamMap>& ev = state->eventBuffer[j];

             // Convert event's cycle-domain start to a sample offset within this buffer.
             // Req 9.1: sampleOffset = (event.active.start - cycleStart) * samplesPerCycle
             const double offsetD =
                 (ev.active.start - cycleStart).toDouble() * samplesPerCycle;

             // Discard events that fall outside this buffer (Req 9.2).
             if (offsetD < 0.0 || offsetD >= static_cast<double>(numSamples))
                 continue;

             const int sampleOffset = static_cast<int>(offsetD);

             // Trigger the voice (Req 9.3).  Pass the slot index as the owner
             // so that slotStop() can silence voices from this slot.
              voicePool_.trigger(ev.value, bank_, sampleOffset, clockNow,
                                 sampleRate,
                                 static_cast<int8_t>(i));

             // Accumulate for the visualizer (no alloc, capped at kMaxFrameEvents).
             // B2: stamp the originating slot index onto the event before it
             // enters the visualizer ring buffer.  sourceOffset travels
             // passively via the Event copy (already set during lowering).
             if (firedEventCount < static_cast<uint32_t>(hathor::kMaxFrameEvents)) {
                 hathor::Event<hathor::ParamMap> fired = ev;
                 fired.slotId = static_cast<int8_t>(i);
                 new (&firedEvents[firedEventCount]) hathor::Event<hathor::ParamMap>(fired);
                 ++firedEventCount;
             }
        }
    }

    // ------------------------------------------------------------------
    // Step 4: Mix all active voices into the output buffer (Req 9.3).
    //   This includes per-voice B7-K1 filtering (cutoff/resonance).
    // ------------------------------------------------------------------
    if (left && right) {
        voicePool_.mix(left, right, numSamples, sampleRate);
    }

    // ------------------------------------------------------------------
    // Step 4a: Mix ChucK audio into the master signal (B4).
    //   ChucK instruments are rendered by the out-of-process worker.
    //   The audio plane is sampled via the validated shared-memory ring
    //   (B4-K1/B4-K2).  This happens AFTER per-voice filtering and BEFORE
    //   the master EQ — ChucK audio is part of the master mix.
    //
    //   B4-K2's transport is mono (kBlockSize = 64 samples).
    //   We deinterleave and mix into both left and right output channels.
    //
    //   Signal chain (decision #13, B7-K2):
    //     per-voice processing + B7-K1 filtering
    //         ↓
    //     voice mix  +  ChucK audio
    //         ↓
    //     Master EQ    (B7-K2)
    //         ↓
    //     Final Master Gain
    //         ↓
    //     Output
    // ------------------------------------------------------------------
    if (left && right && workerMgr_) {
        const uint64_t workerGen = workerGeneration_.load(std::memory_order_acquire);
        constexpr uint32_t kCkBlockSize = 64;
        float ckBuf[kCkBlockSize];

        // tryReadAudioBlock returns false if the worker is dead, stale, or
        // there's no data — in that case we simply skip (the voice mix
        // already in the output buffer is preserved).
        uint32_t blockSize = static_cast<uint32_t>(numSamples);
        if (blockSize > kCkBlockSize)
            blockSize = kCkBlockSize;

        if (workerMgr_->tryReadAudioBlock(ckBuf, blockSize, workerGen)) {
            for (uint32_t s = 0; s < blockSize; ++s) {
                left[s]  += ckBuf[s];
                right[s] += ckBuf[s];
            }
        }
    }

    // ------------------------------------------------------------------
    // Step 4b: Apply Master-bus EQ (B7-K2).
    //   The EQ is the LAST signal-shaping stage before master gain.
    //   We load the current immutable state via atomic_load (acquire).
    //   Processing is allocation-free: we iterate the fixed number of
    //   bands and apply the direct-form-I difference equation.
    //
    //   B7-K2 §3, §4, §5, §6 — no mutex, no allocation, no mutation of
    //   coefficients the audio thread is reading.
    // ------------------------------------------------------------------
    if (left && right) {
        // Acquire-load the complete, immutable EQ state.  The pointer
        // itself is atomically swapped; the pointed-to MasterEqState's
        // delay fields are mutated in-place (they are runtime state, not
        // coefficients — see B7-K2 §6).
        std::shared_ptr<hathor::MasterEqState> eqState =
            std::atomic_load_explicit(&activeEqState_, std::memory_order_acquire);

        if (eqState && eqState->bandCount > 0) {
            for (int s = 0; s < numSamples; ++s) {
                float outL, outR;
                eqState->processSample(left[s], right[s], outL, outR);
                left[s]  = outL;
                right[s] = outR;
            }
        }
        // For Flat (bandCount==0) the filter is identity — we skip processing
        // entirely, which is exactly the neutral behavior required (B7-K2 §8).
    }

    // ------------------------------------------------------------------
    // Step 4c: Apply master gain to the mixed + EQ'd output (Req 26.5, 26.6).
    //   Master gain is the FINAL gain stage — after the EQ (decision #13).
    //   Relaxed load is sufficient — continuous fader, no sync dependency.
    // ------------------------------------------------------------------
    const float gain = masterGain_.load(std::memory_order_relaxed);
    if (gain != 1.0f) {
        for (int ch = 0; ch < numOutputChannels; ++ch) {
            if (outputChannelData[ch])
                juce::FloatVectorOperations::multiply(
                    outputChannelData[ch], gain, numSamples);
        }
    }

    // ------------------------------------------------------------------
    // Step 4d: Capture mixed output to WAV file if capture is open.
    // ------------------------------------------------------------------
    if (captureOpen_.load(std::memory_order_acquire) && captureWriter_ && left && right) {
        // AudioFormatWriter::writeFromFloatArrays expects a non-owning array of
        // const float* channel pointers.
        const float* channels[2] = { left, right };
        captureWriter_->writeFromFloatArrays(channels, 2, numSamples);
    }

    // ------------------------------------------------------------------
    // Step 4c: Publish visualizer frame to SPSC ring buffer (Req 28.3, 28.8).
    //
    // currentCyclePos uses the Phase 1 Req 9.4 double-conversion formula:
    //   cyclePos = (sampleClock * bpm) / (sampleRate * 60)
    // ------------------------------------------------------------------
    {
        const double currentCyclePos =
            (static_cast<double>(clockNow) * bpm) /
            (static_cast<double>(sampleRate) * 60.0);
        vizRingBuffer_.write(currentCyclePos, firedEventCount, firedEvents);
    }

    // Explicitly destroy the placement-new'd elements (Req 7.1 — no heap).
    for (uint32_t i = 0; i < firedEventCount; ++i)
        firedEvents[i].~Event();

    // ------------------------------------------------------------------
    // Step 5: Advance the sample clock by exactly bufferSize (Req 9.4).
    // ------------------------------------------------------------------
    sampleClock_.fetch_add(static_cast<uint64_t>(numSamples),
                           std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// B8-K1: Asset target plumbing (Studio vs Live Jam)
// ---------------------------------------------------------------------------

std::filesystem::path AudioEngine::resolveRenderPath(hathor::AssetTarget target,
                                                     std::string_view name,
                                                     const std::filesystem::path& projectDir)
{
    // B8-K1 §5: centralised path resolution — never duplicated in the renderer.
    // B8-K2 receives the resolved path and writes PCM data into it.
    resolver_.setProjectDir(projectDir);

    // Determine the LiveJam session directory: either the one managed by
    // LiveJamSessionManager (initialised at startup), or a caller-provided
    // directory stored in liveJamSessionDirStorage_.
    std::filesystem::path liveJamDir;
    if (liveJamSession_.isInitialised())
        liveJamDir = liveJamSession_.sessionDir();
    else if (!liveJamSessionDirStorage_.empty())
        liveJamDir = liveJamSessionDirStorage_;

    AssetPathResolver::ResolveResult result =
        resolver_.resolve(target, name, liveJamDir);

    if (!result.ok)
    {
        std::cerr << "[AudioEngine] resolveRenderPath failed for target="
                  << hathor::toString(target) << " name=" << std::string(name)
                  << ": " << result.error << '\n';
        return {};
    }

    return result.path;
}

void AudioEngine::setLiveJamSessionDir(std::filesystem::path dir)
{
    // Register the session temp directory with the LiveJamSessionManager.
    // If @p dir is empty, initialise() creates a fresh session-unique temp dir.
    if (dir.empty())
    {
        if (!liveJamSession_.initialise())
            std::cerr << "[AudioEngine] LiveJam session dir init failed: "
                      << liveJamSession_.lastError() << '\n';
    }
    else
    {
        // Caller-supplied session directory — store it so resolveRenderPath
        // can pass it to the resolver.  We don't call initialise() since the
        // directory is already created.
        liveJamSessionDirStorage_ = std::move(dir);
    }
}

void AudioEngine::cleanupLiveJamAssets()
{
    // B8-K1 §8, §9: session-end cleanup.
    // Removes only LiveJam temp files — NEVER Studio assets.
    if (!liveJamSession_.cleanup())
        std::cerr << "[AudioEngine] LiveJam cleanup warning: "
                  << liveJamSession_.lastError() << '\n';
}

bool AudioEngine::isStudioAssetPath(const std::filesystem::path& path) const
{
    return resolver_.isStudioPath(path);
}

// ---------------------------------------------------------------------------
// B8-K2: Background render writer
// ---------------------------------------------------------------------------

hathor::RenderHandle AudioEngine::startBakeRender(
    uint8_t                            tabId,
    std::string                        ckSource,
    uint64_t                           numSamples,
    unsigned                           sampleRate,
    const std::filesystem::path&       destPath,
    hathor::ChuckRenderWriter::CompletionCallback onComplete)
{
    if (!renderWriter_) {
        onComplete(hathor::RenderResult{
            .success = false,
            .state = hathor::RenderState::Failed,
            .errorMessage = "Render writer not initialised (worker not started)",
        });
        return hathor::RenderHandle{};
    }

    // B8-K4: Wrap the caller's completion callback so that, on a successful
    // bake, the rendered WAV is automatically registered in the SampleBank.
    // This lets `s "instrument_name"` resolve through normal sample playback
    // without any ChucK VM dependency after baking.
    //
    // The wrapped callback:
    //   1. Calls registerBakedAsset() on the SampleBank (B8-K4 §3).
    //   2. Forwards the original RenderResult to the caller's callback.
    auto wrappedCallback = [this, destPath, userCallback = std::move(onComplete)]
                           (const hathor::RenderResult& result) {
        if (result.success && !result.outputPath.empty()) {
            // Derive the sample name from the output path's filename stem.
            // This matches the convention: <name>.wav → sample name "name".
            const std::string sampleName =
                result.outputPath.stem().string();

            // Register the baked asset in the SampleBank.
            // Failures are non-fatal — the WAV file still exists on disk.
            if (!this->registerBakedAsset(sampleName, result.outputPath)) {
                std::cerr << "[AudioEngine] B8-K4 warning: could not register "
                          << sampleName << " in SampleBank after successful bake\n";
            }
        }

        // Forward the result to the caller.
        if (userCallback)
            userCallback(result);
    };

    return renderWriter_->startRender(tabId, std::move(ckSource), numSamples,
                                      sampleRate, destPath,
                                      std::move(wrappedCallback));
}

int AudioEngine::activeRenderCount() const noexcept
{
    return renderWriter_ ? renderWriter_->activeRenderCount() : 0;
}

void AudioEngine::shutdownRender() noexcept
{
    if (renderWriter_)
        renderWriter_->shutdown();
}

// ---------------------------------------------------------------------------
// B8-K4: SampleBank registration after bake
// ---------------------------------------------------------------------------
// These methods are called from the B8-K2 completion callback (on the render
// thread) after a WAV has been successfully published and validated.  They
// decode the WAV, resample to the device rate, and register it in the
// SampleBank via addEntry().
//
// The SampleBank is stored as `const SampleBank& bank_` in AudioEngine, but
// addEntry() is a non-const method.  We cast away constness here because the
// const-ness was inherited from the "load once at startup" design.  B8-K4
// extends this to allow post-bake registration, and the registration mutex
// ensures safe concurrent access.  The audio thread only reads via find()
// (lock-free).
// ---------------------------------------------------------------------------

bool AudioEngine::registerBakedAsset(std::string name,
                                      const std::filesystem::path& wavPath)
{
    if (wavPath.empty())
        return false;

    // Decode the WAV file.
    juce::File juceFile(juce::String(wavPath.string()));
    if (!juceFile.existsAsFile()) {
        std::cerr << "[AudioEngine] registerBakedAsset: file not found: "
                  << wavPath << '\n';
        return false;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juceFile));
    if (!reader) {
        std::cerr << "[AudioEngine] registerBakedAsset: failed to create reader for: "
                  << wavPath << '\n';
        return false;
    }

    const int    numChannels = static_cast<int>(std::min<int>(reader->numChannels, 2));
    const double nativeRate  = reader->sampleRate;
    const int64_t numSamples = static_cast<int64_t>(reader->lengthInSamples);

    if (numSamples <= 0 || nativeRate <= 0.0) {
        std::cerr << "[AudioEngine] registerBakedAsset: invalid metadata in: "
                  << wavPath << '\n';
        return false;
    }

    auto buf = std::make_unique<juce::AudioBuffer<float>>(numChannels,
                                                           static_cast<int>(numSamples));
    if (!reader->read(buf.get(), 0, static_cast<int>(numSamples), 0,
                      true, numChannels > 1)) {
        std::cerr << "[AudioEngine] registerBakedAsset: read failed for: "
                  << wavPath << '\n';
        return false;
    }

    // Resample to device rate if needed.
    std::vector<float> interleavedData;
    const double deviceRate = static_cast<double>(sampleRate_.load(std::memory_order_acquire));

    if (std::abs(nativeRate - deviceRate) > 0.5) {
        // Use the same resampling path as SampleBank::load().
        // We inline it here since resampleBuffer is in an anonymous namespace.
        const double ratio       = deviceRate / nativeRate;
        const int    outSamples  = static_cast<int>(
            std::ceil(static_cast<double>(buf->getNumSamples()) * ratio));

        juce::AudioBuffer<float> outBuf(numChannels, outSamples);
        juce::MemoryAudioSource   memSrc(*buf, /*keepInternalCopy=*/false);
        juce::ResamplingAudioSource resampler(&memSrc, /*deleteSourceWhenDeleted=*/false,
                                               numChannels);
        resampler.setResamplingRatio(nativeRate / deviceRate);
        resampler.prepareToPlay(outSamples, deviceRate);

        juce::AudioSourceChannelInfo info(&outBuf, 0, outSamples);
        resampler.getNextAudioBlock(info);

        // Interleave.
        interleavedData.resize(static_cast<std::size_t>(outSamples) * numChannels);
        if (numChannels == 1) {
            const float* src = outBuf.getReadPointer(0);
            std::copy(src, src + outSamples, interleavedData.begin());
        } else {
            const float* left  = outBuf.getReadPointer(0);
            const float* right = outBuf.getReadPointer(1);
            for (int i = 0; i < outSamples; ++i) {
                interleavedData[static_cast<std::size_t>(i) * 2]     = left[i];
                interleavedData[static_cast<std::size_t>(i) * 2 + 1] = right[i];
            }
        }
    } else {
        // No resampling needed — just interleave.
        interleavedData.resize(static_cast<std::size_t>(numSamples) * numChannels);
        if (numChannels == 1) {
            const float* src = buf->getReadPointer(0);
            std::copy(src, src + static_cast<int>(numSamples), interleavedData.begin());
        } else {
            const float* left  = buf->getReadPointer(0);
            const float* right = buf->getReadPointer(1);
            for (int i = 0; i < static_cast<int>(numSamples); ++i) {
                interleavedData[static_cast<std::size_t>(i) * 2]     = left[i];
                interleavedData[static_cast<std::size_t>(i) * 2 + 1] = right[i];
            }
        }
    }

    // Register in the SampleBank.  bank_ is const by design (load-once), but
    // addEntry() is a legitimate post-bake mutation.  The registration mutex
    // ensures the audio thread's find() (lock-free read) is never in the
    // middle of construction.
    const_cast<SampleBank&>(bank_).addEntry(
        std::move(name),
        0,
        std::move(interleavedData),
        numChannels,
        deviceRate,
        wavPath.string());

    return true;
}

std::vector<std::string> AudioEngine::listSamples() const
{
    return bank_.listNames();
}

// ---------------------------------------------------------------------------
// AI-2: Read-only introspection (Phase 2.5 H0)
// ---------------------------------------------------------------------------

std::vector<AudioEngineFacade::SlotInfo> AudioEngine::listSlots() const noexcept
{
    std::vector<SlotInfo> result;
    result.reserve(kNumSlots);

    for (int i = 0; i < kNumSlots; ++i) {
        AudioEngineFacade::SlotInfo info;
        info.slotIndex = i;
        info.slotName  = slotNames_[i];
        auto state = std::atomic_load_explicit(&slots_[i], std::memory_order_acquire);
        info.active   = (state != nullptr);
        info.running  = false;
        info.eventCount = 0;
        if (state) {
            info.running     = state->running.load(std::memory_order_acquire);
            info.notation    = state->notation;
            info.eventCount  = static_cast<int>(state->eventBuffer.size());
        }
        result.push_back(std::move(info));
    }
    return result;
}

AudioEngineFacade::SlotInfo AudioEngine::getSlotInfo(int slotIndex) const noexcept
{
    SlotInfo info;
    if (slotIndex < 0 || slotIndex >= kNumSlots)
        return info;  // active defaults to false

    info.slotIndex = slotIndex;
    info.slotName  = slotNames_[slotIndex];
    auto state = std::atomic_load_explicit(&slots_[slotIndex], std::memory_order_acquire);
    info.active = (state != nullptr);
    if (state) {
        info.running    = state->running.load(std::memory_order_acquire);
        info.notation   = state->notation;
        info.eventCount = static_cast<int>(state->eventBuffer.size());
    }
    return info;
}

AudioEngineFacade::VmStatus AudioEngine::getVmStatus(int slotIndex) const noexcept
{
    VmStatus status;
    status.hasWorker = (workerMgr_ != nullptr) && workerMgr_->isWorkerAlive();

    if (slotIndex < 0 || slotIndex >= kNumSlots) {
        if (!status.hasWorker)
            status.state = "not_started";
        else
            status.state = "inactive";
        return status;
    }

    if (workerMgr_ && status.hasWorker) {
        const auto vmResult = workerMgr_->queryTabVM(static_cast<uint8_t>(slotIndex));
        status.state = vmResult.message;  // queryTabVM returns a human-readable status string
        if (vmResult.ok)
            status.shredInfo = status.state;  // combined status
        else
            status.lastError = vmResult.message;  // error text in message field
        status.generation = workerMgr_->generation();
    } else if (!status.hasWorker) {
        status.state = "not_started";
    }

    return status;
}

AudioEngineFacade::AudioStatus AudioEngine::getAudioStatus() const noexcept
{
    AudioStatus s;
    s.running    = running_.load(std::memory_order_relaxed);
    s.bpm        = bpm_.load(std::memory_order_relaxed);
    s.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    s.masterGain = masterGain_.load(std::memory_order_relaxed);
    s.eqPreset   = hathor::toString(getMasterEqPreset());
    s.sampleClock = sampleClock_.load(std::memory_order_relaxed);
    s.deviceOpen = (s.sampleRate > 0);  // sample rate set when device opens
    s.activeRenders = activeRenderCount();
    return s;
}

std::vector<AudioEngineFacade::SlotPlayback> AudioEngine::listSlotPlayback() const noexcept
{
    std::vector<SlotPlayback> result;
    result.reserve(kNumSlots);

    for (int i = 0; i < kNumSlots; ++i) {
        SlotPlayback sp;
        sp.slotIndex = i;
        sp.slotName  = slotNames_[i];
        auto state = std::atomic_load_explicit(&slots_[i], std::memory_order_acquire);
        sp.hasPattern = (state != nullptr);
        sp.running    = false;
        if (state) {
            sp.running   = state->running.load(std::memory_order_acquire);
            sp.notation  = state->notation;
        }
        result.push_back(std::move(sp));
    }
    return result;
}

std::vector<AudioEngineFacade::InstrumentInfo> AudioEngine::listChuckInstruments(
    const std::filesystem::path& projectDir) const noexcept
{
    std::vector<InstrumentInfo> result;

    const auto instrDir = resolver_.studioInstrumentsDir();
    std::error_code ec;
    if (!std::filesystem::exists(instrDir, ec))
        return result;

    // Scan the Studio instruments directory for .wav and .ck files.
    // The .wav files are the rendered assets; matching .ck sources may exist.
    std::vector<std::string> wavStems;

    for (const auto& entry : std::filesystem::directory_iterator(instrDir, ec)) {
        if (!entry.is_regular_file())
            continue;
        const auto ext = entry.path().extension();
        if (ext == ".wav")
            wavStems.push_back(entry.path().stem().string());
    }
    std::sort(wavStems.begin(), wavStems.end());

    for (const auto& stem : wavStems) {
        InstrumentInfo info;
        info.name = stem;

        // Check for .ck source — look in the instruments dir and the project root.
        std::filesystem::path ckPath = instrDir / (stem + ".ck");
        if (!std::filesystem::exists(ckPath, ec))
            ckPath = projectDir / (stem + ".ck");
        info.sourceCkExists = std::filesystem::exists(ckPath, ec);
        info.sourcePath = info.sourceCkExists ? ckPath.string() : "";

        // The rendered .wav is in the Studio instruments dir.
        std::filesystem::path wavPath = instrDir / (stem + ".wav");
        info.renderedWavExists = std::filesystem::exists(wavPath, ec);
        info.renderedPath = info.renderedWavExists ? wavPath.string() : "";

        // Check if bound to SampleBank via name lookup (addEntry registers
        // with name = stem, index = 0).
        info.boundToSampleBank = (bank_.find(stem, 0) != nullptr);

        // Duration — read from the WAV file header if available.
        if (info.renderedWavExists)
            info.durationSeconds = AudioEngine::wavDurationSeconds(wavPath);

        result.push_back(std::move(info));
    }

    return result;
}

std::filesystem::path AudioEngine::studioInstrumentsDir(
    const std::filesystem::path& projectDir) const noexcept
{
    return AssetPathResolver(projectDir).studioInstrumentsDir();
}

std::filesystem::path AudioEngine::currentProjectDir() const noexcept
{
    return resolver_.projectDir();
}

