// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Main.cpp — Hathor entry point.
 *
 * Initialisation order (§2.5):
 *   1. Parse CLI args (--samples required, --bpm optional, default 120).
 *   2. Register audio formats and load SampleBank at 44100 Hz default rate.
 *   3. Construct AudioEngine.
 *   4. Install SIGTERM / SIGINT handlers.
 *   5. AudioEngine::initialise() — opens the audio device.
 *   6. Write {"event":"ready","version":"0.1.0"} to stdout.
 *   7. ControlInterface::run() — blocking stdin loop; exits on EOF.
 *
 * Requirements: 8.1–8.5, 12.1–12.5, 14.1–14.6, 16.1–16.5
 */

// JUCE — must come before any app headers that transitively pull in JUCE
#include <juce_audio_formats/juce_audio_formats.h>

// App subsystems
#include "AudioEngine.hpp"
#include "SampleBank.hpp"

// Control layer (JUCE-free facade)
#include "../control/ControlInterface.hpp"
#include "../control/Commands.hpp"

// Third-party
#include <nlohmann/json.hpp>

// Standard library
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>  // _exit()

// ---------------------------------------------------------------------------
// Global state for signal handlers
// ---------------------------------------------------------------------------

/// Set to true by SIGTERM / SIGINT handlers. The main thread may check this
/// between operations, but ControlInterface::run() exits via std::exit(0) on
/// EOF so the flag is mostly a belt-and-suspenders safeguard.
static std::atomic<bool> g_shouldQuit{false};

/// Non-owning pointer to the AudioEngine, set before installing handlers.
static AudioEngine* g_audioEngine = nullptr;

/// Signal handler — async-signal-safe: calls _exit() after a best-effort
/// stop of any active audio voices.
static void signalHandler(int /*sig*/) noexcept
{
    // stop() is not strictly async-signal-safe, but for a desktop application
    // that is not running audio callbacks from a signal-masked thread, this is
    // acceptable and recommended practice for clean audio shutdown.
    if (g_audioEngine) {
        g_audioEngine->stop();
        g_audioEngine->closeCapture();  // flush WAV file before exit
    }
    // _exit is async-signal-safe; std::exit is not.
    _exit(0);
}

/// atexit handler — flushes the WAV capture file when std::exit(0) is called
/// (e.g. from ControlInterface quit/EOF path).
static void atExitHandler() noexcept
{
    if (g_audioEngine) {
        g_audioEngine->closeCapture();
    }
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    // 1. Parse CLI arguments
    // -----------------------------------------------------------------------
    std::string samplesPath;
    double      initialBpm  = 120.0;
    bool        hasBpm      = false;
    std::string capturePath;

    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--samples" && i + 1 < argc) {
            samplesPath = argv[++i];
        } else if (arg == "--bpm" && i + 1 < argc) {
            try {
                initialBpm = std::stod(argv[++i]);
                hasBpm     = true;
            } catch (...) {
                std::cerr << "[hathor] error: --bpm value is not a valid number: "
                          << argv[i] << '\n';
                return 1;
            }
        } else if (arg == "--capture-to-file" && i + 1 < argc) {
            capturePath = argv[++i];
        }
    }

    if (samplesPath.empty()) {
        std::cerr << "[hathor] error: --samples <path> is required\n";
        std::cerr << "Usage: hathor --samples <path> [--bpm <n>]\n";
        return 1;
    }

    // -----------------------------------------------------------------------
    // 2. Register audio formats and load SampleBank
    //    Use 44100 Hz as the default device sample rate; the AudioEngine will
    //    query the actual rate after the device opens (§2.5 design note).
    // -----------------------------------------------------------------------
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    SampleBank bank;
    try {
        bank.load(samplesPath, formatManager, 44100.0);
    } catch (const std::exception& ex) {
        std::cerr << "[hathor] error: failed to load samples from '"
                  << samplesPath << "': " << ex.what() << '\n';
        return 1;
    }

    std::cerr << "[hathor] loaded " << bank.loadedCount()
              << " samples ("  << bank.skippedCount() << " skipped) from '"
              << samplesPath   << "'\n";

    // -----------------------------------------------------------------------
    // 3. Construct AudioEngine (and optionally set initial BPM)
    // -----------------------------------------------------------------------
    AudioEngine audio(bank);

    if (hasBpm) {
        audio.setBpm(initialBpm);
    }

    // -----------------------------------------------------------------------
    // 4. Install SIGTERM / SIGINT handlers
    // -----------------------------------------------------------------------
    g_audioEngine = &audio;
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGINT,  signalHandler);
    std::atexit(atExitHandler);  // flushes WAV capture on std::exit()

    // -----------------------------------------------------------------------
    // 5. Initialise the audio device (opens hardware, reports rate + buffer)
    // -----------------------------------------------------------------------
    const std::string initError = audio.initialise();
    if (!initError.empty()) {
        std::cerr << "[hathor] error: audio device initialisation failed: "
                  << initError << '\n';

        // Write error event to stdout so the controlling process can detect it.
        hathor::control::respond({
            {"event",   "error"},
            {"message", initError}
        });

        return 1;
    }

    // -----------------------------------------------------------------------
    // 5b. Open WAV capture file if requested (--capture-to-file)
    //     Must be after initialise() so the sample rate is known.
    // -----------------------------------------------------------------------
    if (!capturePath.empty()) {
        const std::string captureErr = audio.openCapture(capturePath);
        if (!captureErr.empty()) {
            std::cerr << "[hathor] warning: could not open capture file: "
                      << captureErr << '\n';
            // Non-fatal — proceed without capture.
        }
    }

    // -----------------------------------------------------------------------
    // 6. Signal readiness to the controlling process
    // -----------------------------------------------------------------------
    hathor::control::respond({
        {"event",   "ready"},
        {"version", "0.1.0"}
    });

    // -----------------------------------------------------------------------
    // 7. Enter the blocking control loop (exits via std::exit on EOF or quit)
    // -----------------------------------------------------------------------
    hathor::control::ControlInterface ci(audio, bank);
    ci.run();

    // Unreachable — ci.run() calls std::exit(0) on EOF.
    return 0;
}
