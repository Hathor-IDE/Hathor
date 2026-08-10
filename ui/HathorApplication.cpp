// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * HathorApplication.cpp — JUCE application entry point for hathor-ui.
 *
 * Defines the JUCEApplication subclass that owns MainWindow and wires together
 * the AudioEngine, SampleBank, and ControlInterface at startup.
 *
 * CLI argument handling follows the same priority as the Phase 1 Main.cpp:
 *   --samples <path>  (required)
 *   --bpm     <n>     (optional, default 120)
 *   --agent   <path>  (optional; also reads HATHOR_AGENT env var)
 *
 * Requirements: 20.4, 20.5, 31.1, 32.1
 */

#include <cstdlib>
#include <iostream>

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "MainWindow.hpp"

#include "../app/AudioEngine.hpp"
#include "../app/SampleBank.hpp"
#include "../control/ControlInterface.hpp"

// ---------------------------------------------------------------------------
// HathorApplication
// ---------------------------------------------------------------------------

class HathorApplication : public juce::JUCEApplication
{
public:
    HathorApplication() = default;

    const juce::String getApplicationName() override    { return "Hathor"; }
    const juce::String getApplicationVersion() override { return "0.2.0"; }
    bool moreThanOneInstanceAllowed() override           { return false; }

    // -----------------------------------------------------------------------
    // Initialise — called by JUCE on the message thread after the event loop
    // starts.
    // -----------------------------------------------------------------------
    void initialise(const juce::String& commandLine) override
    {
        // Parse arguments from the command line string provided by JUCE.
        juce::StringArray args;
        args.addTokens(commandLine, true);

        std::string samplesPath;
        double      initialBpm   = 120.0;
        std::string agentExePath;

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--samples" && i + 1 < args.size())
                samplesPath = args[++i].toStdString();
            else if (args[i] == "--bpm" && i + 1 < args.size())
                initialBpm = args[++i].getDoubleValue();
            else if (args[i] == "--agent" && i + 1 < args.size())
                agentExePath = args[++i].toStdString();
        }

        // Fall back to HATHOR_AGENT env var if --agent was not provided (Req 32.1).
        if (agentExePath.empty())
        {
            const char* envAgent = std::getenv("HATHOR_AGENT");
            if (envAgent != nullptr)
                agentExePath = envAgent;
        }

        if (samplesPath.empty())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Hathor",
                "--samples <path> is required.\n\nUsage: hathor-ui --samples <path> [--bpm <n>] [--agent <path>]",
                "OK",
                nullptr,
                juce::ModalCallbackFunction::create([](int) { juce::JUCEApplication::getInstance()->quit(); }));
            return;
        }

        // Register audio formats and load SampleBank.
        formatManager_.registerBasicFormats();
         bank_ = std::make_unique<SampleBank>();
         try
         {
             bank_->load(samplesPath, formatManager_, 44100.0);
         }
          catch (const std::exception& ex)
          {
             juce::AlertWindow::showMessageBoxAsync(
                 juce::AlertWindow::WarningIcon,
                 "Hathor",
                 juce::String("Failed to load samples: ") + ex.what(),
                 "OK",
                 nullptr,
                 juce::ModalCallbackFunction::create([](int) { juce::JUCEApplication::getInstance()->quit(); }));
             return;
         }

         // B8-K4 §4: Reload Studio-persisted baked WAV assets from the
         // current project directory so previously-baked instruments are
         // available for `s "name"` without re-baking.
         {
             const std::filesystem::path cwd =
                 std::filesystem::current_path();
             const std::filesystem::path studioDir =
                 cwd / ".hathor_assets" / "chuck_instruments";
             if (std::filesystem::is_directory(studioDir)) {
                 bank_->reloadStudioAssets(studioDir, formatManager_, 44100.0);
             }
         }

        // Construct AudioEngine and open the audio device.
        audio_ = std::make_unique<AudioEngine>(*bank_);
        audio_->setBpm(initialBpm);

        // Initialize the AudioEngine's project directory from the application's
        // working directory so that currentProjectDir(), studioInstrumentsDir(),
        // and listChuckInstruments() all resolve against the correct project root.
        audio_->setProjectDir(std::filesystem::current_path());

        const std::string initError = audio_->initialise();
        if (!initError.empty())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Hathor",
                juce::String("Audio device error: ") + initError,
                "OK",
                nullptr,
                juce::ModalCallbackFunction::create([](int) { juce::JUCEApplication::getInstance()->quit(); }));
            return;
        }

         // Start the audio worker process (B4-K7: needed for .ck tab eval).
        // Resolve path as a sibling of the executable, same as hathor-mcp.
        const std::string workerPath =
            juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                .getSiblingFile("hathor-audio-worker")
                .getFullPathName()
                .toStdString();
        const std::string workerError = audio_->startWorker(workerPath);
        if (!workerError.empty())
        {
            // Worker failure is non-fatal — mini-notation still works.
            // .ck tab eval will show error at eval time via hasWorker() check.
            // Log to stderr for diagnosis but don't block startup.
            std::cerr << "[HathorApplication] Worker startup: " << workerError << std::endl;
        }

        // B8-K1 §9: Initialise the LiveJam session temp directory at startup.
        // This creates a session-unique temp dir under the platform temp area
        // for Live Jam assets (disposable renders).  Studio assets are unaffected.
        audio_->setLiveJamSessionDir({});

        // Construct ControlInterface (worker thread, worker stdin disabled in
        // GUI mode — ControlInterface::run() is not called here; dispatch() is
        // called directly from UI components on the worker thread pool).
        ci_ = std::make_unique<hathor::control::ControlInterface>(*audio_, *bank_);

        // Resolve hathor-mcp path: look for it as a sibling of the executable.
        // If not present, pass empty string — ChatSidebar will still work but
        // tool calls won't be forwarded (Req 32.1).
        const std::string hathorMcpPath =
            juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                .getSiblingFile("hathor-mcp")
                .getFullPathName()
                .toStdString();

        // Create and show the main window.
         mainWindow_ = std::make_unique<MainWindow>(*audio_, *ci_, agentExePath, hathorMcpPath);
    }

    void shutdown() override
    {
        // B8-K1 §9: Clean up LiveJam session assets before tearing down.
        // This removes only temporary LiveJam files — NEVER Studio assets.
        // Done before window destruction so the AudioEngine (and its
        // LiveJamSessionManager) is still alive.
        if (audio_)
            audio_->cleanupLiveJamAssets();

        mainWindow_.reset();
        ci_.reset();
        if (audio_)
            audio_->shutdownWorker();
        audio_.reset();
        bank_.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted(const juce::String& /*commandLine*/) override {}

private:
    juce::AudioFormatManager                          formatManager_;
    std::unique_ptr<SampleBank>                       bank_;
    std::unique_ptr<AudioEngine>                      audio_;
    std::unique_ptr<hathor::control::ControlInterface> ci_;
    std::unique_ptr<MainWindow>                        mainWindow_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HathorApplication)
};

// ---------------------------------------------------------------------------
// JUCE application entry point macro — replaces main()
// ---------------------------------------------------------------------------
START_JUCE_APPLICATION(HathorApplication)
