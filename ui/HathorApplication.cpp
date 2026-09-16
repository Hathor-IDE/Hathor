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
#include <filesystem>
#include <iostream>
#include <vector>

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
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Resolve a default samples directory when the user launches hathor-ui without
// an explicit --samples <path> (e.g. by double-clicking the .app bundle, which
// passes no command-line arguments on macOS).
//
// Candidate locations, in priority order:
//   1. ./samples            (process CWD — the layout documented in README)
//   2. ../Resources/samples (bundled inside the .app package)
//   3. <source_root>/samples (dev-tree fallback when launched from build/)
//
// Returns the first existing directory, or an empty path if none are found.
// ---------------------------------------------------------------------------
static std::filesystem::path resolveDefaultSamplesPath()
{
    const std::vector<std::filesystem::path> candidates = {
        "samples",
        juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getSiblingFile("..")
            .getChildFile("Resources")
            .getChildFile("samples")
            .getFullPathName().toStdString(),
        juce::File::getSpecialLocation(juce::File::currentExecutableFile)
            .getSiblingFile("..")
            .getSiblingFile("..")
            .getChildFile("samples")
            .getFullPathName().toStdString(),
    };

    std::error_code ec;
    for (const auto& c : candidates)
    {
        if (!c.empty() && std::filesystem::is_directory(c, ec))
            return std::filesystem::canonical(c, ec);
    }
    return {};
}

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

// Wave 5.2 (C5, C6): centralised defaults + robust helper-binary lookup.
// Search order: exact sibling of exe → common CMake build-layout subdirs
// near exe → HATHOR_<NAME>_PATH env override. Returns empty if missing.
namespace {
constexpr double kDefaultBpm = 120.0;
constexpr double kDefaultSampleRate = 44100.0;

juce::File resolveHelperBinary(const char* name, const char* envVar)
{
    if (const char* env = std::getenv(envVar); env != nullptr && *env != '\0')
    {
        juce::File f(env);
        if (f.existsAsFile())
            return f;
    }
    const juce::File exe =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const juce::File sibling = exe.getSiblingFile(name);
    if (sibling.existsAsFile())
        return sibling;
    // Dev-build layouts: exe may live in build/app/Debug etc.
    const juce::File parent = exe.getParentDirectory();
    const char* subdirs[] = { ".", "..", "../..", "MacOS", "../MacOS" };
    for (auto* s : subdirs)
    {
        juce::File c = parent.getChildFile(s).getChildFile(name);
        if (c.existsAsFile())
            return c;
    }
    return sibling; // missing — caller reports with searched locations
}
} // namespace

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
        double      initialBpm   = kDefaultBpm;
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

        // If --samples was not supplied, try a default samples directory so
        // that double-clicking the .app bundle (which passes no argv on macOS)
        // still opens the IDE instead of immediately erroring out.
        if (samplesPath.empty())
        {
            const std::filesystem::path fallback = resolveDefaultSamplesPath();
            if (!fallback.empty())
            {
                samplesPath = fallback.string();
                std::cerr << "[HathorApplication] no --samples given; using default: "
                          << samplesPath << std::endl;
            }
        }

        if (samplesPath.empty())
        {
            const std::string usage =
                "--samples <path> is required.\n\n"
                "Usage: hathor-ui --samples <path> [--bpm <n>] [--agent <path>]\n\n"
                "No default samples directory was found. Launch from a terminal "
                "with --samples <path>, or open the app from within a project "
                "directory that contains a `samples/` folder.";
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Hathor",
                usage,
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
             bank_->load(samplesPath, formatManager_, kDefaultSampleRate);
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
                  bank_->reloadStudioAssets(studioDir, formatManager_, kDefaultSampleRate);
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

        // Wave 5.2 (C6): device-rate-aware sample loading. If the device
        // runs at e.g. 48000 Hz, reload at the real rate so playback pitch
        // is correct; otherwise warn loudly on stdout.
        {
            const double deviceRate = static_cast<double>(audio_->getSampleRate());
            if (deviceRate > 0.0 && std::abs(deviceRate - kDefaultSampleRate) > 1.0)
            {
                std::cerr << "[HathorApplication] device rate " << deviceRate
                          << " Hz differs from default " << kDefaultSampleRate
                          << " Hz — reloading samples at device rate." << std::endl;
                try
                {
                    bank_->load(samplesPath, formatManager_, deviceRate);
                }
                catch (const std::exception& ex)
                {
                    std::cerr << "[HathorApplication] device-rate reload failed: "
                              << ex.what() << std::endl;
                }
            }
        }

         // Start the audio worker process (B4-K7: needed for .ck tab eval).
         // Wave 5.2 (C5): resolve via sibling → build-layout dirs → env
         // override, with searched locations named on failure.
         const juce::File workerFile = resolveHelperBinary("hathor-audio-worker",
                                                           "HATHOR_WORKER_PATH");
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

         // Resolve hathor-mcp path (Wave 5.2 / C5: same search order).
         const juce::File mcpFile = resolveHelperBinary("hathor-mcp", "HATHOR_MCP_PATH");
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
