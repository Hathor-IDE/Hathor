// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "SampleBank.hpp"

// Pull in the JUCE modules we need.  The JuceHeader.h approach is used by
// juce_add_console_app; for console targets that don't generate one we
// include the module headers directly.
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <iostream>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers
// ---------------------------------------------------------------------------
namespace {

/// Returns true when `ext` (already lower-cased) is one of the three
/// accepted audio extensions.
bool isAcceptedExtension(const std::string& ext)
{
    return ext == ".wav" || ext == ".aiff" || ext == ".flac";
}

/// Lower-case an ASCII string in-place.
std::string toLower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Try to parse `stem` as a non-negative decimal integer.
/// Returns true and sets `out` on success; returns false otherwise.
bool parseNonNegativeIndex(const std::string& stem, int64_t& out)
{
    if (stem.empty())
        return false;

    // std::from_chars is locale-independent and allocation-free.
    const char* first = stem.data();
    const char* last  = first + stem.size();
    int64_t value{};
    auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || value < 0)
        return false;

    out = value;
    return true;
}

/// Decode an audio file into an AudioBuffer<float> at its native sample rate.
/// Returns nullptr (and logs) on failure.
std::unique_ptr<juce::AudioBuffer<float>>
decodeFile(const juce::File&            file,
           juce::AudioFormatManager&    formats,
           int&                         outNumChannels,
           double&                      outNativeSampleRate,
           int64_t&                     outNumSamples)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader)
    {
        std::cerr << "[SampleBank] failed to create reader for: "
                  << file.getFullPathName().toStdString() << '\n';
        return nullptr;
    }

    // Clamp channels to 2 (mono or stereo only).
    outNumChannels      = static_cast<int>(std::min<int>(reader->numChannels, 2));
    outNativeSampleRate = reader->sampleRate;
    outNumSamples       = static_cast<int64_t>(reader->lengthInSamples);

    if (outNumSamples <= 0 || outNativeSampleRate <= 0.0)
    {
        std::cerr << "[SampleBank] invalid metadata in: "
                  << file.getFullPathName().toStdString() << '\n';
        return nullptr;
    }

    auto buf = std::make_unique<juce::AudioBuffer<float>>(
        outNumChannels,
        static_cast<int>(outNumSamples));

    // read() with a float AudioBuffer writes normalised floats directly.
    bool ok = reader->read(buf.get(),
                           /*destStartSample=*/0,
                           static_cast<int>(outNumSamples),
                           /*readerStartSample=*/0,
                           /*useReaderLeftChan=*/true,
                           /*useReaderRightChan=*/(outNumChannels > 1));
    if (!ok)
    {
        std::cerr << "[SampleBank] read failed for: "
                  << file.getFullPathName().toStdString() << '\n';
        return nullptr;
    }

    return buf;
}

/// Resample `sourceBuf` (recorded at `sourceRate`) to `deviceRate`.
/// Returns a new AudioBuffer<float> at deviceRate, or nullptr on failure.
std::unique_ptr<juce::AudioBuffer<float>>
resampleBuffer(juce::AudioBuffer<float>& sourceBuf,
               int                       numChannels,
               double                    sourceRate,
               double                    deviceRate)
{
    const double ratio      = deviceRate / sourceRate;
    const int    outSamples = static_cast<int>(
        std::ceil(static_cast<double>(sourceBuf.getNumSamples()) * ratio));

    if (outSamples <= 0)
        return nullptr;

    auto outBuf = std::make_unique<juce::AudioBuffer<float>>(numChannels, outSamples);

    // MemoryAudioSource wraps the decoded PCM without copying.
    juce::MemoryAudioSource   memSrc(sourceBuf, /*keepInternalCopy=*/false);
    juce::ResamplingAudioSource resampler(&memSrc,
                                          /*deleteSourceWhenDeleted=*/false,
                                          numChannels);

    // JUCE's ResamplingAudioSource ratio is source/output (i.e. < 1 means upsampling).
    resampler.setResamplingRatio(sourceRate / deviceRate);
    resampler.prepareToPlay(outSamples, deviceRate);

    juce::AudioSourceChannelInfo info(outBuf.get(), 0, outSamples);
    resampler.getNextAudioBlock(info);

    return outBuf;
}

/// Copy an AudioBuffer<float> into an interleaved std::vector<float>.
/// Layout: for stereo [L0,R0,L1,R1,...], for mono [S0,S1,...].
std::vector<float> interleave(const juce::AudioBuffer<float>& buf, int numChannels)
{
    const int numSamples = buf.getNumSamples();
    std::vector<float> out;
    out.resize(static_cast<std::size_t>(numChannels) *
               static_cast<std::size_t>(numSamples));

    if (numChannels == 1)
    {
        const float* src = buf.getReadPointer(0);
        std::copy(src, src + numSamples, out.begin());
    }
    else // stereo
    {
        const float* left  = buf.getReadPointer(0);
        const float* right = buf.getReadPointer(1);
        for (int i = 0; i < numSamples; ++i)
        {
            out[static_cast<std::size_t>(i) * 2]     = left[i];
            out[static_cast<std::size_t>(i) * 2 + 1] = right[i];
        }
    }

    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// SampleBank::load
// ---------------------------------------------------------------------------
void SampleBank::load(const std::filesystem::path& root,
                      juce::AudioFormatManager&    formats,
                      double                       deviceSampleRate)
{
    // Register WAV, AIFF, FLAC (and a few others) in one call.
    formats.registerBasicFormats();

    if (!std::filesystem::is_directory(root))
    {
        std::cerr << "[SampleBank] root is not a directory: " << root << '\n';
        return;
    }

    // Iterate top-level entries – each subdirectory is a sample "name".
    for (const auto& nameEntry : std::filesystem::directory_iterator(root))
    {
        if (!nameEntry.is_directory())
            continue;

        const std::string name = nameEntry.path().filename().string();

        // Iterate files inside this subdirectory.
        for (const auto& fileEntry : std::filesystem::directory_iterator(nameEntry.path()))
        {
            if (!fileEntry.is_regular_file())
                continue;

            const std::string ext  = toLower(fileEntry.path().extension().string());
            if (!isAcceptedExtension(ext))
                continue;

            const std::string stem = fileEntry.path().stem().string();
            int64_t index{};
            if (!parseNonNegativeIndex(stem, index))
            {
                // Non-numeric stem – skip silently (not counted as a decode error).
                ++skipped_;
                continue;
            }

            // Decode ---------------------------------------------------------
            juce::File juceFile(juce::String(fileEntry.path().string()));
            int    numChannels{};
            double nativeRate{};
            int64_t numSamples{};

            auto sourceBuf = decodeFile(juceFile, formats,
                                        numChannels, nativeRate, numSamples);
            if (!sourceBuf)
            {
                ++skipped_;
                continue;
            }

            // Resample -------------------------------------------------------
            std::vector<float> interleavedData;

            if (std::abs(nativeRate - deviceSampleRate) > 0.5)
            {
                auto resampled = resampleBuffer(*sourceBuf, numChannels,
                                               nativeRate, deviceSampleRate);
                if (!resampled)
                {
                    std::cerr << "[SampleBank] resampling failed for: "
                              << fileEntry.path() << '\n';
                    ++skipped_;
                    continue;
                }
                interleavedData = interleave(*resampled, numChannels);
            }
            else
            {
                interleavedData = interleave(*sourceBuf, numChannels);
            }

            // Store ----------------------------------------------------------
            SampleEntry entry;
            entry.name        = name;
            entry.index       = index;
            entry.numChannels = numChannels;
            entry.sampleRate  = deviceSampleRate;
            entry.data        = std::move(interleavedData);

            entries_.push_back(std::move(entry));
            ++loaded_;
        }
    }
}

// ---------------------------------------------------------------------------
// B8-K4: Dynamic asset registration
// ---------------------------------------------------------------------------

void SampleBank::addEntry(std::string             name,
                           int64_t                 index,
                           std::vector<float>      data,
                           int                     numChannels,
                           double                  sampleRate,
                           std::string             sourcePath)
{
    std::lock_guard<std::mutex> lock(registrationMutex_);
    SampleEntry entry;
    entry.name        = std::move(name);
    entry.index       = index;
    entry.data        = std::move(data);
    entry.numChannels = numChannels;
    entry.sampleRate  = sampleRate;
    entry.sourcePath  = std::move(sourcePath);
    entries_.push_back(std::move(entry));
    ++loaded_;
}

void SampleBank::reloadStudioAssets(const std::filesystem::path&     dir,
                                     juce::AudioFormatManager&        formats,
                                     double                         sampleRate,
                                     bool                           skipRegistered)
{
    if (!std::filesystem::is_directory(dir))
        return;

    for (const auto& entry : std::filesystem::directory_iterator(dir))
    {
        if (!entry.is_regular_file())
            continue;

        const auto ext = toLower(entry.path().extension().string());
        if (!isAcceptedExtension(ext))
            continue;

        const std::string stem = entry.path().stem().string();

        // Skip if already registered (unless skipRegistered is false).
        if (skipRegistered)
        {
            std::lock_guard<std::mutex> lock(registrationMutex_);
            bool already = false;
            for (const auto& e : entries_)
            {
                if (e.name == stem && e.index == 0)
                {
                    already = true;
                    break;
                }
            }
            if (already)
                continue;
        }

        // Decode and resample using the existing helpers.
        juce::File juceFile(juce::String(entry.path().string()));
        int    numChannels{};
        double nativeRate{};
        int64_t numSamples{};

        auto sourceBuf = decodeFile(juceFile, formats,
                                    numChannels, nativeRate, numSamples);
        if (!sourceBuf)
            continue;

        std::vector<float> interleavedData;
        if (std::abs(nativeRate - sampleRate) > 0.5)
        {
            auto resampled = resampleBuffer(*sourceBuf, numChannels,
                                           nativeRate, sampleRate);
            if (!resampled)
                continue;
            interleavedData = interleave(*resampled, numChannels);
        }
        else
        {
            interleavedData = interleave(*sourceBuf, numChannels);
        }

        addEntry(stem, 0, std::move(interleavedData),
                 numChannels, sampleRate,
                 entry.path().string());
    }
}

std::vector<std::string> SampleBank::listNames() const noexcept
{
    std::vector<std::string> names;
    for (const auto& e : entries_)
    {
        bool seen = false;
        for (const auto& n : names)
        {
            if (n == e.name)
            {
                seen = true;
                break;
            }
        }
        if (!seen)
            names.push_back(e.name);
    }
    std::sort(names.begin(), names.end());
    return names;
}
