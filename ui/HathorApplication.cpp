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

// 0.2 (P6): Resolve the project/workspace root — prefer the explorer root
// persisted by the previous session; fall back to the process CWD. Used so
// `.hathor_assets` resolves under the opened project even when the binary is
// launched from elsewhere (e.g. `/`).
static std::filesystem::path resolveProjectRoot()
{
    juce::PropertiesFile::Options opts;
    opts.applicationName      = "Hathor";
    opts.filenameSuffix       = ".props";
    opts.folderName           = "Hathor";
    opts.storageFormat        = juce::PropertiesFile::storeAsXML;
    opts.commonToAllUsers     = false;
    opts.ignoreCaseOfKeyNames = false;

    juce::ApplicationProperties props;
    props.setStorageParameters(opts);
    if (auto* settings = props.getUserSettings())
    {
        const std::filesystem::path persisted(
            settings->getValue("explorerLastDirectory").toStdString());
        std::error_code ec;
        if (!persisted.empty() && std::filesystem::is_directory(persisted, ec))
            return persisted;
    }
    return std::filesystem::current_path();
}

class HathorApplication : public juce::JUCEApplication
{
public:
    HathorApplication() = default;

    const juce::String getApplicationName() override    { return "Hathor"; }
    const juce::String getApplicationVersion() override { return HATHOR_UI_VERSION; }
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
        // addTokens(..., true) preserves surrounding quote characters in the
        // token (e.g. "--samples \"/path with spaces\"" → token is
        // "\"/path with spaces\"").  Strip leading/trailing quotes so
        // paths with spaces resolve correctly.
        args.removeEmptyStrings();
        for (int i = 0; i < args.size(); ++i)
        {
            if (args[i].startsWith("\"") && args[i].endsWith("\"")
                && args[i].length() >= 2)
            {
                juce::String stripped = args[i].substring(1, args[i].length() - 1);
                args.set(i, stripped);
            }
        }

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

         // 0.2 (P6): Reload Studio-persisted baked WAV assets from the
         // resolved project root (persisted workspace, not the process CWD)
         // so previously-baked instruments are available for `s "name"`
         // without re-baking.
         {
             const std::filesystem::path studioDir =
                 resolveProjectRoot() / ".hathor_assets" / "chuck_instruments";
             if (std::filesystem::is_directory(studioDir)) {
                 bank_->reloadStudioAssets(studioDir, formatManager_, 44100.0);
             }
         }

        // Construct AudioEngine and open the audio device.
        audio_ = std::make_unique<AudioEngine>(*bank_);
        audio_->setBpm(initialBpm);

        // Initialize the AudioEngine's project directory from the resolved
        // project root so that currentProjectDir(), studioInstrumentsDir(),
        // and listChuckInstruments() all resolve against the correct project
        // root (0.2 / P6 — no longer the process CWD).
        audio_->setProjectDir(resolveProjectRoot());

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
         // Resolve path as a sibling of the executable — same layout the
         // macOS .app bundle uses (Contents/MacOS/).
         const juce::File workerFile =
             juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                 .getSiblingFile("hathor-audio-worker");
         const std::string workerPath = workerFile.getFullPathName().toStdString();

         // Phase 6.3: Explicit existence check — fail loudly if the worker
         // binary is missing from the application bundle.  Silent degradation
         // is unacceptable for the macOS beta: the audio backend must be
         // present for .ck tab evaluation.
         if (!workerFile.existsAsFile())
         {
             const std::string msg =
                 "Hathor audio worker is missing from the application bundle:\n"
                 "  " + workerPath + "\n\n"
                 "Audio tab evaluation (.ck files) will not function. "
                 "Please reinstall Hathor.";

             std::cerr << "[HathorApplication] ERROR: audio worker missing at "
                       << workerPath << std::endl;

             juce::AlertWindow::showMessageBoxAsync(
                 juce::AlertWindow::WarningIcon,
                 "Hathor — Audio Worker Missing",
                 msg,
                 "OK",
                 nullptr,
                 juce::ModalCallbackFunction::create([](int) {}));

             // Worker is not started.  hasWorker() will report false so that
             // .ck tab eval surfaces a clear error at eval time rather than
             // silently failing.
         }
         else
         {
             const std::string workerError = audio_->startWorker(workerPath);
             if (!workerError.empty())
             {
                 // Worker binary exists but failed to start — log for diagnosis.
                 // Non-fatal: mini-notation still works; .ck eval errors at
                 // eval time via hasWorker().
                 std::cerr << "[HathorApplication] Worker startup: "
                           << workerError << std::endl;
             }
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
         const juce::File mcpFile =
             juce::File::getSpecialLocation(juce::File::currentExecutableFile)
                 .getSiblingFile("hathor-mcp");
         std::string hathorMcpPath = mcpFile.getFullPathName().toStdString();

         // Phase 6.3: Explicit existence check — fail loudly if the MCP binary
         // is missing from the bundle.  Without it, agent tool calls would not
         // be forwarded, and the user would see "no response" rather than a
         // clear "unavailable" message.
         if (!mcpFile.existsAsFile())
         {
             const std::string missingPath = mcpFile.getFullPathName().toStdString();
             const std::string msg =
                 "Hathor MCP server is missing from the application bundle:\n"
                 "  " + missingPath + "\n\n"
                 "AI tool calls via the agent will not function. "
                 "Please reinstall Hathor.";

             std::cerr << "[HathorApplication] ERROR: hathor-mcp missing at "
                       << missingPath << std::endl;

             juce::AlertWindow::showMessageBoxAsync(
                 juce::AlertWindow::WarningIcon,
                 "Hathor — MCP Server Missing",
                 msg,
                 "OK",
                 nullptr,
                 juce::ModalCallbackFunction::create([](int) {}));

             // Pass empty path so MainWindow / ChatSidebar know MCP is
             // unavailable and will not attempt to spawn it.
             hathorMcpPath.clear();
         }

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

    // Single-instance app (moreThanOneInstanceAllowed() returns false).
    // JUCE still requires this override to exist; when a second instance is
    // launched, the first instance receives this callback instead. We
    // intentionally do nothing — the second instance simply exits via
    // JUCE's single-instance mechanism.
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
