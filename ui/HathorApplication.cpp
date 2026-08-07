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
        double      initialBpm = 120.0;

        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i] == "--samples" && i + 1 < args.size())
                samplesPath = args[++i].toStdString();
            else if (args[i] == "--bpm" && i + 1 < args.size())
                initialBpm = args[++i].getDoubleValue();
        }

        if (samplesPath.empty())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Hathor",
                "--samples <path> is required.\n\nUsage: hathor-ui --samples <path> [--bpm <n>]",
                "OK",
                nullptr,
                juce::ModalCallbackFunction::create([this](int) { quit(); }));
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
                juce::ModalCallbackFunction::create([this](int) { quit(); }));
            return;
        }

        // Construct AudioEngine and open the audio device.
        audio_ = std::make_unique<AudioEngine>(*bank_);
        audio_->setBpm(initialBpm);

        const std::string initError = audio_->initialise();
        if (!initError.empty())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Hathor",
                juce::String("Audio device error: ") + initError,
                "OK",
                nullptr,
                juce::ModalCallbackFunction::create([this](int) { quit(); }));
            return;
        }

        // Construct ControlInterface (worker thread, worker stdin disabled in
        // GUI mode — ControlInterface::run() is not called here; dispatch() is
        // called directly from UI components on the worker thread pool).
        ci_ = std::make_unique<hathor::control::ControlInterface>(*audio_, *bank_);

        // Create and show the main window.
        mainWindow_ = std::make_unique<MainWindow>(*audio_, *ci_);
    }

    void shutdown() override
    {
        mainWindow_.reset();
        ci_.reset();
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
