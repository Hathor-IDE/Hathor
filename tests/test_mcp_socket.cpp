// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * test_mcp_socket.cpp — Phase 2.5 H0 socket accept-loop tests.
 *
 * Exercises the full MCP/control command path without JUCE or an audio device:
 * a Unix listener + runSocketAcceptLoop (worker thread) + ControlInterface
 * (via dispatchWithCallback) + a fake AudioEngineFacade.
 *
 * Covers (H0 acceptance):
 *   1. A client can connect to the Hathor Unix socket.
 *   2. A command (bpm) reaches the dispatch layer and a JSON response returns.
 *   3. An async command (set-pattern, WorkerThread) also returns a response.
 *   4. Multiple commands / connections do not corrupt command processing.
 *   5. Shutdown sets the stop flag, the accept loop exits promptly (no hang),
 *      and the socket file is removed.
 *
 * Requirement: Phase 2.5 H0
 */

#include "ControlInterface.hpp"
#include "SocketServer.hpp"

#include "AudioEngineFacade.hpp"
#include "SampleBank.hpp"
#include "SlotState.hpp"

#include <nlohmann/json.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <vector>

// ---------------------------------------------------------------------------
// Fake AudioEngineFacade (JUCE-free stand-in)
// ---------------------------------------------------------------------------

class FakeFacade final : public AudioEngineFacade {
public:
    void play() noexcept override { running_ = true; }
    void stop() noexcept override { running_ = false; }
    void setBpm(double b) noexcept override { bpm_ = b; }
    double getBpm() const noexcept override { return bpm_; }
    bool isRunning() const noexcept override { return running_; }

    void slotPlay(int slotIdx) noexcept override {
        if (slotIdx >= 0 && static_cast<std::size_t>(slotIdx) < states_.size())
            slotRunning_[static_cast<std::size_t>(slotIdx)] = true;
    }
    void slotStop(int slotIdx) noexcept override {
        if (slotIdx >= 0 && static_cast<std::size_t>(slotIdx) < states_.size())
            slotRunning_[static_cast<std::size_t>(slotIdx)] = false;
    }
    bool isSlotRunning(int slotIdx) const noexcept override {
        if (slotIdx >= 0 && static_cast<std::size_t>(slotIdx) < states_.size())
            return slotRunning_[static_cast<std::size_t>(slotIdx)];
        return false;
    }

    void setMasterGain(float g) noexcept override { gain_ = g; }
    float getMasterGain() const noexcept override { return gain_; }

    int findOrAddSlot(const std::string& name) override
    {
        for (std::size_t i = 0; i < names_.size(); ++i)
            if (names_[i] == name)
                return static_cast<int>(i);
        if (names_.size() < 16)
        {
            names_.push_back(name);
            states_.push_back(nullptr);
            slotRunning_.push_back(false);
            return static_cast<int>(names_.size() - 1);
        }
        return -1;
    }
    void storeSlot(int idx, std::shared_ptr<SlotState> state) noexcept override
    {
        if (idx >= 0 && static_cast<std::size_t>(idx) < states_.size())
            states_[static_cast<std::size_t>(idx)] = std::move(state);
    }
    bool clearSlot(int idx) noexcept override
    {
        if (idx >= 0 && static_cast<std::size_t>(idx) < states_.size())
        {
            states_[static_cast<std::size_t>(idx)].reset();
            return true;
        }
        return false;
    }
    int slotCount() const noexcept override { return static_cast<int>(states_.size()); }
    std::string slotName(int idx) const override
    {
        if (idx >= 0 && static_cast<std::size_t>(idx) < names_.size())
            return names_[static_cast<std::size_t>(idx)];
        return {};
    }
    std::shared_ptr<SlotState> loadSlot(int idx) const noexcept override
    {
        if (idx >= 0 && static_cast<std::size_t>(idx) < states_.size())
            return states_[static_cast<std::size_t>(idx)];
        return nullptr;
    }

    bool hasWorker() const noexcept override { return false; }
    bool ckEval(int slotIdx, const std::string& code) noexcept override
    {
        (void)slotIdx; (void)code;
        return false;
    }
    bool stopCkTab(int slotIdx) noexcept override
    {
        (void)slotIdx;
        return false;
    }
    std::string queryCkTab(int slotIdx) const override
    {
        (void)slotIdx;
        return {};
    }

    // AI-5 stubs
    uint64_t startAsyncCkCompile(int slotIdx, const std::string& code,
                                  std::function<void(bool, const std::string&)> onComplete) override
    {
        (void)slotIdx; (void)code;
        if (onComplete) onComplete(false, "worker not running");
        return 0;
    }
    nlohmann::json queryCkJob(uint64_t jobId) const override
    {
        return nlohmann::json{{"ok", false}, {"job_id", jobId}, {"status", "failed"}};
    }
    bool cancelCkJob(uint64_t jobId) override
    {
        (void)jobId;
        return false;
    }

    void setMasterEqPreset(hathor::EqPreset preset) noexcept override
    {
        (void)preset;
    }
     hathor::EqPreset getMasterEqPreset() const noexcept override
    {
        return hathor::EqPreset::Flat;
    }

    // B8-K1 stubs — not exercised by the MCP socket tests.
    std::filesystem::path resolveRenderPath(hathor::AssetTarget /*target*/,
                                            std::string_view /*name*/,
                                            const std::filesystem::path& /*projectDir*/) override
    {
        return {};
    }
    void setLiveJamSessionDir(std::filesystem::path /*dir*/) override {}
    void cleanupLiveJamAssets() override {}
    bool isStudioAssetPath(const std::filesystem::path& /*path*/) const override
    {
        return false;
    }

    // B8-K2 stubs — not exercised by the MCP socket tests.
    hathor::RenderHandle startBakeRender(uint8_t,
                                         std::string,
                                         uint64_t,
                                         unsigned,
                                         const std::filesystem::path&,
                                         hathor::ChuckRenderWriter::CompletionCallback) override
    {
        return hathor::RenderHandle{};
    }
    int  activeRenderCount() const noexcept override { return 0; }
    void shutdownRender() noexcept override {}

     // B8-K4 stubs
     bool registerBakedAsset(std::string, const std::filesystem::path&) override
     {
         return false;
     }
     std::vector<std::string> listSamples() const override
     {
         return {};
     }

     // --- AI-2 read-only introspection stubs (not exercised by MCP socket tests) ---
     std::vector<SlotInfo> listSlots() const noexcept override { return {}; }
     SlotInfo getSlotInfo(int idx) const noexcept override
     {
         SlotInfo info;
         info.slotIndex = idx;
         return info;
     }
     VmStatus getVmStatus(int) const noexcept override { return VmStatus{}; }
     AudioStatus getAudioStatus() const noexcept override
     {
         return AudioStatus{false, 120.0, 0, 1.0f, "flat", 0, false, 0};
     }
     std::vector<SlotPlayback> listSlotPlayback() const noexcept override { return {}; }
     std::vector<InstrumentInfo> listChuckInstruments(
         const std::filesystem::path&) const noexcept override { return {}; }
     std::filesystem::path studioInstrumentsDir(
         const std::filesystem::path&) const noexcept override { return {}; }
     std::filesystem::path currentProjectDir() const noexcept override { return projectDir_; }
     void setProjectDir(std::filesystem::path dir) override { projectDir_ = std::move(dir); }

private:
    double bpm_ = 120.0;
    bool running_ = false;
    float gain_ = 1.0f;
    std::vector<std::string> names_;
    std::vector<std::shared_ptr<SlotState>> states_;
    std::vector<bool> slotRunning_;
    std::filesystem::path projectDir_;
};

// ---------------------------------------------------------------------------
// UNIX socket helpers
// ---------------------------------------------------------------------------

static int socketCounter = 0;

static std::string makeSocketPath()
{
    const char* tmpdir = std::getenv("TMPDIR");
    if (!tmpdir || tmpdir[0] == '\0')
        tmpdir = "/tmp";
    return std::string(tmpdir) + "/hathor-test-" + std::to_string(::getpid())
         + "-" + std::to_string(++socketCounter) + ".sock";
}


/// Create a listening UNIX socket at @p path.  Returns the listener fd.
static int createListener(const std::string& path)
{
    ::unlink(path.c_str());
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0
        || ::listen(fd, 4) != 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Connect a client to @p path; returns the client fd.
static int connectClient(const std::string& path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Send @p line + '\n' to @p fd, then read one response line.
static std::string getResponse(int fd, const std::string& command)
{
    const std::string out = command + "\n";
    ::write(fd, out.c_str(), out.size());

    std::string response;
    char buf[4096];
    while (true)
    {
        const ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n <= 0)
            break;
        buf[n] = '\0';
        response += buf;
        if (response.find('\n') != std::string::npos)
            break;
    }
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
        response.pop_back();
    return response;
}

/// Spawn the accept loop in a thread; returns (thread, stop flag reference).
struct AcceptServer {
    std::atomic<bool> stop{false};
    std::thread thread;
};

// ---------------------------------------------------------------------------
// Test cases
// ---------------------------------------------------------------------------

TEST_CASE("H0: sync command reaches dispatch layer and returns a response",
          "[h0][socket][control]")
{
    FakeFacade audio;
    SampleBank bank;
    hathor::control::ControlInterface ci(audio, bank);

    const std::string path = makeSocketPath();
    const int listener = createListener(path);
    REQUIRE(listener >= 0);

    AcceptServer srv;
    srv.thread = std::thread([&] {
        hathor::control::runSocketAcceptLoop(
            listener, srv.stop,
            [&ci](std::string cmd, std::function<void(std::string)> respond) {
                ci.dispatchWithCallback(
                    cmd, [respond = std::move(respond)](nlohmann::json j) {
                        respond(j.dump());
                    });
            });
    });

    const int client = connectClient(path);
    REQUIRE(client >= 0);

    const std::string resp = getResponse(client, "bpm 130");
    REQUIRE_FALSE(resp.empty());
    const nlohmann::json j = nlohmann::json::parse(resp);
    REQUIRE(j.value("ok", false) == true);
    REQUIRE(j.value("bpm", 0.0) == 130.0);
    REQUIRE(audio.getBpm() == 130.0);

    ::close(client);
    srv.stop = true;
    if (srv.thread.joinable()) srv.thread.join();
    ::close(listener);
    ::unlink(path.c_str());
}

TEST_CASE("h0: async set-pattern returns a response via the worker thread", "[h0][socket][control]")
{
    FakeFacade facade;
    SampleBank bank;
    hathor::control::ControlInterface ci(facade, bank);

    const std::string path = makeSocketPath();
    const int listener = createListener(path);
    REQUIRE(listener >= 0);

    AcceptServer srv;
    srv.thread = std::thread([&] {
        hathor::control::runSocketAcceptLoop(
            listener, srv.stop,
            [&ci](std::string cmd, std::function<void(std::string)> respond) {
                ci.dispatchWithCallback(
                    cmd, [respond = std::move(respond)](nlohmann::json j) {
                        respond(j.dump());
                    });
            });
    });

    const int client = connectClient(path);
    REQUIRE(client >= 0);

    const std::string resp = getResponse(client, "set-pattern d0 bd sn");
    REQUIRE_FALSE(resp.empty());
    const nlohmann::json j = nlohmann::json::parse(resp);
    REQUIRE(j.value("ok", false) == true);

    ::close(client);
    srv.stop = true;
    if (srv.thread.joinable()) srv.thread.join();
    ::close(listener);
    ::unlink(path.c_str());
}

TEST_CASE("[h0] multiple commands/connections do not corrupt processing", "[h0][socket][control]")
{
    FakeFacade facade;
    SampleBank bank;
    hathor::control::ControlInterface ci(facade, bank);

    const std::string path = makeSocketPath();
    const int listener = createListener(path);
    REQUIRE(listener >= 0);

    AcceptServer srv;
    srv.thread = std::thread([&] {
        hathor::control::runSocketAcceptLoop(
            listener, srv.stop,
            [&ci](std::string cmd, std::function<void(std::string)> respond) {
                ci.dispatchWithCallback(
                    cmd, [respond = std::move(respond)](nlohmann::json j) {
                        respond(j.dump());
                    });
            });
    });

    // Two clients, several serialised commands each.
    const int c1 = connectClient(path);
    const int c2 = connectClient(path);
    REQUIRE(c1 >= 0);
    REQUIRE(c2 >= 0);

    const nlohmann::json a = nlohmann::json::parse(getResponse(c1, "bpm 100"));
    REQUIRE(a.value("bpm", -1.0) == 100.0);
    const nlohmann::json b = nlohmann::json::parse(getResponse(c2, "bpm 140"));
    REQUIRE(b.value("bpm", -1.0) == 140.0);
    const nlohmann::json c = nlohmann::json::parse(getResponse(c1, "play"));
    REQUIRE(c.value("ok", false) == true);
    const nlohmann::json d = nlohmann::json::parse(getResponse(c2, "stop"));
    REQUIRE(d.value("ok", false) == true);

    // Confirming last-writer bpm wins (commands processed, not dropped).
    REQUIRE(facade.getBpm() == 140.0);

    ::close(c1);
    ::close(c2);
    srv.stop = true;
    if (srv.thread.joinable()) srv.thread.join();
    ::close(listener);
    ::unlink(path.c_str());
}

TEST_CASE("[h0] shutdown stops the loop promptly and cleans up the socket", "[h0][socket][control]")
{
    FakeFacade facade;
    SampleBank bank;
    hathor::control::ControlInterface ci(facade, bank);

    const std::string path = makeSocketPath();
    const int listener = createListener(path);
    REQUIRE(listener >= 0);

    AcceptServer srv;
    srv.thread = std::thread([&] {
        hathor::control::runSocketAcceptLoop(
            listener, srv.stop,
            [&ci](std::string cmd, std::function<void(std::string)> resp) {
                ci.dispatchWithCallback(
                    cmd, [&](nlohmann::json j) { resp(j.dump()); });
            });
    });

    // Give the accept loop a moment to start, then request shutdown.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    srv.stop = true;

    // Must exit promptly (no hang) — bounded join.
    const auto begin = std::chrono::steady_clock::now();
    if (srv.thread.joinable())
        srv.thread.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);
    REQUIRE(elapsed.count() < 2000);

    ::close(listener);

    // The accept loop is done and no longer holds the socket; the app-level
    // teardown (AcpAgentSession::removeUnixSocket) can now unlink it safely.
    ::unlink(path.c_str());

    struct stat st{};
    REQUIRE(::stat(path.c_str(), &st) != 0); // socket file removed
}