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
    // Shut down the device before releasing resources.
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
    closeCapture();
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
}

void AudioEngine::slotStop(int slotIdx) noexcept
{
    auto state = loadSlot(slotIdx);
    if (!state)
        return;
    state->running.store(false, std::memory_order_release);
    // Silence any voices currently playing from this slot (A3).
    voicePool_.silenceSlot(static_cast<int8_t>(slotIdx));
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
// juce::AudioIODeviceCallback — device lifecycle
// ---------------------------------------------------------------------------

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (!device)
        return;

    const int rate = static_cast<int>(device->getCurrentSampleRate());
    sampleRate_.store(rate, std::memory_order_relaxed);

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
    // ------------------------------------------------------------------
    if (left && right) {
        voicePool_.mix(left, right, numSamples);
    }

    // ------------------------------------------------------------------
    // Step 4a: Apply master gain to the mixed output (Req 26.5, 26.6).
    // Relaxed load is sufficient — continuous fader, no sync dependency.
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
    // Step 4b: Capture mixed output to WAV file if capture is open.
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
