// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * Main.cpp — Hathor entry point.
 *
 * Initialisation order (§2.5):
 *   1. Parse CLI args (--samples required, --bpm optional, --agent optional).
 *   2. Register audio formats and load SampleBank at 44100 Hz default rate.
 *   3. Construct AudioEngine.
 *   4. Install SIGTERM / SIGINT handlers.
 *   5. AudioEngine::initialise() — opens the audio device.
 *   6. Write {"event":"ready","version":"0.1.0"} to stdout.
 *   7. ControlInterface::run() — blocking stdin loop; exits on EOF.
 *
 * Agent path resolution order (Req 32.1):
 *   --agent <path>  →  HATHOR_AGENT env var  →  persisted PropertiesFile value  →  ""
 *
 * Requirements: 8.1–8.5, 12.1–12.5, 14.1–14.6, 16.1–16.5, 32.1
 */

// JUCE — must come before any app headers that transitively pull in JUCE
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

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

/// Resolved agent executable path, set once during startup (Req 32.1).
/// Accessed by getAgentExePath() below.
static std::string g_agentExePath;

/// Returns the resolved agent executable path set at startup (Req 32.1).
/// Empty string means no agent is configured.
const std::string& getAgentExePath() noexcept
{
    return g_agentExePath;
}

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
    std::string agentPathArg; ///< value of --agent flag (empty if not supplied)

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
        } else if (arg == "--agent" && i + 1 < argc) {
            agentPathArg = argv[++i];
        }
    }

    // -----------------------------------------------------------------------
    // 1b. Resolve agent executable path (Req 32.1)
    //
    // Priority: --agent flag > HATHOR_AGENT env var > persisted XML properties
    //           file > empty string (defer agent startup).
    //
    // Persistence uses juce::File + juce::XmlDocument (both in juce_core,
    // already linked) to avoid adding juce_data_structures as a dependency.
    // -----------------------------------------------------------------------
    const juce::File propsDir  = juce::File::getSpecialLocation(
                                     juce::File::userApplicationDataDirectory)
                                 .getChildFile("Hathor");
    const juce::File propsFile = propsDir.getChildFile("hathor.xml");

    // Load persisted XML properties.
    std::unique_ptr<juce::XmlElement> propsXml;
    if (propsFile.existsAsFile()) {
        propsXml = juce::XmlDocument::parse(propsFile);
    }
    if (!propsXml || propsXml->getTagName() != "HathorProperties") {
        propsXml = std::make_unique<juce::XmlElement>("HathorProperties");
    }

    std::string resolvedAgentPath;

    if (!agentPathArg.empty()) {
        // --agent flag takes highest priority.
        resolvedAgentPath = agentPathArg;
    } else {
        // Fall back to HATHOR_AGENT environment variable.
        const char* envAgent = std::getenv("HATHOR_AGENT");
        if (envAgent && envAgent[0] != '\0') {
            resolvedAgentPath = envAgent;
        } else {
            // Fall back to persisted XML value.
            resolvedAgentPath =
                propsXml->getStringAttribute("agentExePath").toStdString();
        }
    }

    // Persist the resolved path for future launches.
    propsXml->setAttribute("agentExePath", juce::String(resolvedAgentPath));
    propsDir.createDirectory();
    propsXml->writeTo(propsFile, {});

    if (resolvedAgentPath.empty()) {
        std::cerr << "[hathor] info: no agent configured — "
                     "ChatSidebar will show 'No agent configured'.\n";
        std::cerr << "[hathor] info: set --agent <path> or "
                     "HATHOR_AGENT env var to enable AI chat.\n";
    } else {
        std::cerr << "[hathor] info: agent executable: "
                  << resolvedAgentPath << '\n';
    }

    // Store in the global accessor so AcpAgentSession::start() can retrieve it.
    g_agentExePath = resolvedAgentPath;

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

    // B8-K4 §4: Reload Studio-persisted baked WAV assets from the current
    // project directory (<cwd>/.hathor_assets/chuck_instruments/).  This makes
    // previously-baked instruments available for `s "name"` without re-baking.
    {
        const std::filesystem::path studioDir =
            std::filesystem::current_path() / ".hathor_assets" / "chuck_instruments";
        if (std::filesystem::is_directory(studioDir)) {
            bank.reloadStudioAssets(studioDir, formatManager, 44100.0);
            std::cerr << "[hathor] reloaded " << bank.loadedCount()
                      << " samples (including " << (bank.loadedCount())
                      << " total after studio assets)\n";
        }
    }

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
