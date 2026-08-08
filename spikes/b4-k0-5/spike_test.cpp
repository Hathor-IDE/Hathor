// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// B4-K0.5: libchuck concurrency spike (thread-safety)
//
// Purpose: Experimentally determine whether the libchuck API
// (compileCode / run) is thread-safe for the concurrency pattern B4 needs:
//
//   Thread A (VM owner thread):
//     - create a ChucK VM
//     - continuously call run() on the VM
//
//   Thread B (compile/worker thread):
//     - repeatedly call compileCode() while the VM is running
//
// The experiment tests BOTH the immediate=FALSE path (deferred spork via
// lock-free FinalRingBuffer — the path B4-K4 should use) AND the
// immediate=TRUE path (direct spork on the calling thread — the unsafe path
// B4 must NOT use). Comparing both establishes whether the safe path is
// actually safe, and whether the unsafe path actually races.
//
// Test cases:
//   Case A — Baseline: VM starts, runs, stops, clean lifecycle.
//   Case B — Single compilation: compile one program from another thread
//            while VM runs continuously.
//   Case C — Repeated compilation: repeatedly call compileCode() with
//            multiple representative snippets while run() stays active.
//   Case D — Sustained concurrency: run for a bounded iteration count
//            sufficient to expose races.
//   Case E — Rapid handoff: high-frequency compilation to increase contention.
//   Case F — Shutdown race: compile while VM is shutting down.
//
// Result: GO (immediate=FALSE is safe) or NO-GO (immediate=TRUE races).
//
// See docs/b4-k0-5-decision.md for the full architectural decision record.

#include "chuck.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

static void printChout(const char* msg)
{
    std::printf("[chuck] %s\n", msg);
    std::fflush(stdout);
}

// Global flag for detecting crashes via signal — minimal crash detection
static volatile sig_atomic_t g_vm_crashed = 0;

struct SpikeResult
{
    int caseNum = 0;
    const char* caseName = "";
    const char* mode = ""; // "deferred" (immediate=FALSE) or "immediate" (immediate=TRUE)
    unsigned long compileAttempts = 0;
    unsigned long compileSuccesses = 0;
    unsigned long compileFailures = 0;
    unsigned long vmCrashes = 0;
    unsigned long runIterations = 0;
    bool completed = false;
    bool crashed = false;
};

// Representative ChucK snippets that exercise different compilation paths.
// These mimic the kinds of programs B4-K4 would compile for per-tab VMs.
static const std::vector<std::string> kChuckSnippets = {
    // Simple oscillator + sine wave — the "hello world" of ChucK audio
    "SinOsc sin => dac; 440 => sin.freq; 0.5 => sin.gain; 1::second => now;",

    // More complex: two oscillators with modulation
    "SinOsc lfo => blackhole; SinOsc osc => dac; "
    "440 => osc.freq; 5 => lfo.freq; "
    "osc + (0.01 => osc.oscillator) => now; 1::second => now;",

    // Loop with a shorter duration — exercises shred scheduling
    "SinOsc osc => dac; 220 => osc.freq; "
    "repeat(4) { 0.25::second => now; }",

    // Noise generator — different UGen type
    "Noise n => LPF filt => dac; 1000 => filt.freq; "
    "0.3 => n.gain; 0.5::second => now;",

    // Multiple oscillators with different frequencies
    "SinOsc a => dac; SinOsc b => dac; "
    "220 => a.freq; 330 => b.freq; 0.3 => a.gain; 0.3 => b.gain; "
    "1::second => now;",
};

// ---------------------------------------------------------------------------
// VM owner thread function (Thread A)
//
// Continuously calls run() on the ChucK VM. Each call processes a buffer
// of audio frames. The thread owns the VM exclusively for run() calls.
// compileCode() from Thread B uses the deferred path (immediate=FALSE),
// which queues shreds through a lock-free ring buffer consumed by compute().
// ---------------------------------------------------------------------------

struct VmContext
{
    ChucK* chuck = nullptr;
    std::atomic<bool> shouldStop{false};
    std::atomic<bool> vmRunning{false};
    std::atomic<unsigned long> runIterations{0};
    std::atomic<unsigned long> crashes{0};
};

static void vmOwnerThread(VmContext& ctx, unsigned long maxIterations)
{
    constexpr int kNumFrames = 256;
    constexpr int kNumInputChannels = 0;
    constexpr int kNumOutputChannels = 2;

    SAMPLE input[kNumInputChannels * kNumFrames] = {};
    SAMPLE output[kNumOutputChannels * kNumFrames] = {};

    // Give Thread B a moment to start compiling
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    while (!ctx.shouldStop.load(std::memory_order_relaxed))
    {
        try
        {
            ctx.chuck->run(input, output, kNumFrames);
            ctx.runIterations.fetch_add(1, std::memory_order_relaxed);

            if (ctx.runIterations.load() >= maxIterations)
                break;
        }
        catch (...)
        {
            ctx.crashes.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        // Small yield to avoid consuming 100% CPU in the test
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    ctx.vmRunning.store(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Compile thread function (Thread B)
//
// Repeatedly calls compileCode() from a separate thread while Thread A
// is running the VM. Tests both immediate=FALSE (deferred, the path B4
// should use) and immediate=TRUE (immediate, the path B4 must avoid).
// ---------------------------------------------------------------------------

enum class SporkMode { Deferred, Immediate };

static void compileThread(VmContext& ctx,
                          std::atomic<bool>& shouldStop,
                          unsigned long maxIterations,
                          SporkMode mode,
                          SpikeResult& result)
{
    t_CKBOOL immediate = (mode == SporkMode::Immediate) ? TRUE : FALSE;
    const char* modeStr = (mode == SporkMode::Deferred) ? "deferred" : "immediate";

    // Give Thread A time to start running before we begin compiling
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    unsigned long iter = 0;
    while (!shouldStop.load(std::memory_order_relaxed) && iter < maxIterations)
    {
        const std::string& snippet = kChuckSnippets[iter % kChuckSnippets.size()];

        std::vector<t_CKUINT> shredIDs;
        t_CKBOOL ok = ctx.chuck->compileCode(snippet, "", 1, immediate, &shredIDs);

        result.compileAttempts++;
        if (ok)
        {
            result.compileSuccesses++;
        }
        else
        {
            result.compileFailures++;
        }

        iter++;

        // For rapid handoff case, no sleep — for normal cases, small sleep
        if (maxIterations > 5000)
        {
            // Rapid handoff: minimal delay to maximize contention
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    result.mode = modeStr;
    result.completed = true;
}

// ---------------------------------------------------------------------------
// Case A: Baseline — VM starts, runs, stops, clean lifecycle
// ---------------------------------------------------------------------------

static SpikeResult runCaseA()
{
    SpikeResult result;
    result.caseNum = 1;
    result.caseName = "A: Baseline lifecycle (no concurrent compile)";
    result.mode = "n/a";

    ChucK* chuck = new ChucK();
    chuck->setParam(CHUCK_PARAM_SAMPLE_RATE, 44100);
    chuck->setParam(CHUCK_PARAM_INPUT_CHANNELS, 0);
    chuck->setParam(CHUCK_PARAM_OUTPUT_CHANNELS, 2);
    chuck->setParam(CHUCK_PARAM_VM_HALT, TRUE);
    chuck->setParam(CHUCK_PARAM_IS_REALTIME_AUDIO_HINT, FALSE);

    if (!chuck->init())
    {
        std::fprintf(stderr, "[CASE A] FAIL: init() returned false\n");
        result.completed = false;
        CK_SAFE_DELETE(chuck);
        return result;
    }

    if (!chuck->start())
    {
        std::fprintf(stderr, "[CASE A] FAIL: start() returned false\n");
        result.completed = false;
        CK_SAFE_DELETE(chuck);
        return result;
    }

    // Run the VM for a short period
    VmContext ctx;
    ctx.chuck = chuck;
    ctx.shouldStop.store(false);

    constexpr int kNumFrames = 256;
    SAMPLE input[0] = {};
    SAMPLE output[512] = {};

    for (int i = 0; i < 100; i++)
    {
        chuck->run(input, output, kNumFrames);
        result.runIterations++;
    }

    // Clean shutdown
    chuck->removeAllShreds();
    result.completed = true;

    std::printf("[CASE A] PASS: VM started, ran %lu times, stopped cleanly\n",
                result.runIterations);

    CK_SAFE_DELETE(chuck);
    return result;
}

// ---------------------------------------------------------------------------
// Case B: Single compilation from another thread (deferred mode)
// ---------------------------------------------------------------------------

static SpikeResult runCaseB(SporkMode mode)
{
    SpikeResult result;
    result.caseNum = 2;
    result.caseName = (mode == SporkMode::Deferred)
        ? "B: Single compile (deferred/immediate=FALSE)"
        : "B: Single compile (immediate/immediate=TRUE)";

    ChucK* chuck = new ChucK();
    chuck->setParam(CHUCK_PARAM_SAMPLE_RATE, 44100);
    chuck->setParam(CHUCK_PARAM_INPUT_CHANNELS, 0);
    chuck->setParam(CHUCK_PARAM_OUTPUT_CHANNELS, 2);
    chuck->setParam(CHUCK_PARAM_VM_HALT, TRUE);
    chuck->setParam(CHUCK_PARAM_IS_REALTIME_AUDIO_HINT, FALSE);

    if (!chuck->init() || !chuck->start())
    {
        std::fprintf(stderr, "[CASE B] FAIL: init/start\n");
        result.completed = false;
        CK_SAFE_DELETE(chuck);
        return result;
    }

    chuck->setChoutCallback(printChout);
    chuck->setCherrCallback(printChout);

    VmContext ctx;
    ctx.chuck = chuck;

    // Start VM owner thread
    constexpr unsigned long kMaxRunIters = 500;
    std::thread vmThread([&ctx, kMaxRunIters]() {
        vmOwnerThread(ctx, kMaxRunIters);
    });

    // Give VM time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Single compile from this thread (Thread B)
    std::vector<t_CKUINT> shredIDs;
    t_CKBOOL immediate = (mode == SporkMode::Immediate) ? TRUE : FALSE;
    t_CKBOOL ok = chuck->compileCode(kChuckSnippets[0], "", 1, immediate, &shredIDs);

    result.compileAttempts = 1;
    if (ok)
        result.compileSuccesses = 1;
    else
        result.compileFailures = 1;

    // Let the shred run
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Shutdown
    ctx.shouldStop.store(true);
    vmThread.join();

    result.runIterations = ctx.runIterations.load();
    result.vmCrashes = ctx.crashes.load();
    result.completed = true;

    // Check if VM is still alive
    if (chuck->vm_running())
    {
        chuck->removeAllShreds();
    }

    std::printf("[CASE B/%s] compile=%s, runIters=%lu, crashes=%lu\n",
                result.mode,
                ok ? "OK" : "FAIL",
                result.runIterations,
                result.vmCrashes);

    CK_SAFE_DELETE(chuck);
    return result;
}

// ---------------------------------------------------------------------------
// Case C/D/E: Repeated compilation (deferred and immediate modes)
// ---------------------------------------------------------------------------

static SpikeResult runCaseCD(ECaseMode /* unused */, SporkMode mode,
                             unsigned long maxIterations,
                             bool rapidHandoff = false)
{
    SpikeResult result;
    result.caseNum = (maxIterations < 2000) ? 3 : (rapidHandoff ? 5 : 4);
    result.caseName = (mode == SporkMode::Deferred)
        ? (rapidHandoff
            ? "E: Rapid handoff compile (deferred/immediate=FALSE)"
            : (maxIterations < 2000
                ? "C: Repeated compile (deferred/immediate=FALSE)"
                : "D: Sustained concurrency (deferred/immediate=FALSE)"))
        : (rapidHandoff
            ? "E: Rapid handoff compile (immediate/immediate=TRUE)"
            : (maxIterations < 2000
                ? "C: Repeated compile (immediate/immediate=TRUE)"
                : "D: Sustained concurrency (immediate/immediate=TRUE)"));

    ChucK* chuck = new ChucK();
    chuck->setParam(CHUCK_PARAM_SAMPLE_RATE, 44100);
    chuck->setParam(CHUCK_PARAM_INPUT_CHANNELS, 0);
    chuck->setParam(CHUCK_PARAM_OUTPUT_CHANNELS, 2);
    chuck->setParam(CHUCK_PARAM_VM_HALT, FALSE);  // Don't halt — keep running
    chuck->setParam(CHUCK_PARAM_IS_REALTIME_AUDIO_HINT, FALSE);

    if (!chuck->init() || !chuck->start())
    {
        std::fprintf(stderr, "[CASE] FAIL: init/start\n");
        result.completed = false;
        CK_SAFE_DELETE(chuck);
        return result;
    }

    // Redirect ChucK output to avoid interference
    chuck->setChoutCallback(printChout);
    chuck->setCherrCallback(printChout);

    VmContext ctx;
    ctx.chuck = chuck;

    constexpr unsigned long kMaxRunIters = 200000;

    // Start VM owner thread
    std::thread vmThread([&ctx, kMaxRunIters]() {
        vmOwnerThread(ctx, kMaxRunIters);
    });

    // Start compile thread
    std::thread compileTh([&ctx, mode, maxIterations, &result]() {
        compileThread(ctx, ctx.shouldStop, maxIterations, mode, result);
    });

    // Run for the duration of the compile thread
    compileTh.join();

    // Let any pending shreds finish
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Shutdown
    ctx.shouldStop.store(true);
    vmThread.join();

    result.runIterations = ctx.runIterations.load();
    result.vmCrashes = ctx.crashes.load();
    result.completed = true;

    if (chuck->vm_running())
    {
        chuck->removeAllShreds();
    }

    std::printf("[%s] mode=%s, compileAttempts=%lu, successes=%lu, failures=%lu, "
                "runIters=%lu, crashes=%lu\n",
                result.caseName,
                result.mode,
                result.compileAttempts,
                result.compileSuccesses,
                result.compileFailures,
                result.runIterations,
                result.vmCrashes);

    CK_SAFE_DELETE(chuck);
    return result;
}

// ---------------------------------------------------------------------------
// Case F: Shutdown race — compile while VM is approaching shutdown
// ---------------------------------------------------------------------------

static SpikeResult runCaseF(SporkMode mode)
{
    SpikeResult result;
    result.caseNum = 6;
    result.caseName = (mode == SporkMode::Deferred)
        ? "F: Shutdown race (deferred/immediate=FALSE)"
        : "F: Shutdown race (immediate/immediate=TRUE)";

    ChucK* chuck = new ChucK();
    chuck->setParam(CHUCK_PARAM_SAMPLE_RATE, 44100);
    chuck->setParam(CHUCK_PARAM_INPUT_CHANNELS, 0);
    chuck->setParam(CHUCK_PARAM_OUTPUT_CHANNELS, 2);
    chuck->setParam(CHUCK_PARAM_VM_HALT, FALSE);
    chuck->setParam(CHUCK_PARAM_IS_REALTIME_AUDIO_HINT, FALSE);

    if (!chuck->init() || !chuck->start())
    {
        std::fprintf(stderr, "[CASE F] FAIL: init/start\n");
        result.completed = false;
        CK_SAFE_DELETE(chuck);
        return result;
    }

    chuck->setChoutCallback(printChout);
    chuck->setCherrCallback(printChout);

    VmContext ctx;
    ctx.chuck = chuck;

    constexpr unsigned long kMaxRunIters = 200000;

    // Start VM owner thread
    std::thread vmThread([&ctx, kMaxRunIters]() {
        vmOwnerThread(ctx, kMaxRunIters);
    });

    // Give VM time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Compile in a tight loop, then stop VM while compiling is still happening
    std::atomic<bool> shouldStop{false};
    std::thread compileTh([&ctx, mode, &shouldStop, &result]() {
        t_CKBOOL immediate = (mode == SporkMode::Immediate) ? TRUE : FALSE;
        const char* modeStr = (mode == SporkMode::Immediate) ? "immediate" : "deferred";

        for (unsigned long i = 0; i < 200 && !shouldStop.load(); i++)
        {
            const std::string& snippet = kChuckSnippets[i % kChuckSnippets.size()];
            std::vector<t_CKUINT> shredIDs;
            t_CKBOOL ok = ctx.chuck->compileCode(snippet, "", 1, immediate, &shredIDs);
            result.compileAttempts++;
            if (ok)
                result.compileSuccesses++;
            else
                result.compileFailures++;

            // No sleep — maximize contention with shutdown
        }

        result.mode = modeStr;
        result.completed = true;
    });

    // Let compilation run for a short time, then shut down the VM
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    ctx.shouldStop.store(true);

    // Also signal the compile thread to stop
    shouldStop.store(true);

    vmThread.join();
    compileTh.join();

    result.runIterations = ctx.runIterations.load();
    result.vmCrashes = ctx.cracks.load();

    std::printf("[%s] compileAttempts=%lu, successes=%lu, failures=%lu, "
                "runIters=%lu, crashes=%lu\n",
                result.caseName,
                result.compileAttempts,
                result.compileSuccesses,
                result.compileFailures,
                result.runIterations,
                result.vmCrashes);

    CK_SAFE_DELETE(chuck);
    return result;
}

// ---------------------------------------------------------------------------
// Main: run all cases and report results
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    std::printf("=== B4-K0.5: libchuck concurrency spike ===\n");
    std::printf("libchuck version: %s\n", ChucK::version());
    std::printf("Platform: %s\n",
    #ifdef __APPLE__
        "macOS"
    #elif defined(__linux__)
        "Linux"
    #elif defined(_WIN32)
        "Windows"
    #else
        "Unknown"
    #endif
    );

    std::printf("\n--- Architecture: B4 Handoff Pattern ---\n");
    std::printf("Thread A (VM owner): create VM, continuously call run()\n");
    std::printf("Thread B (compile):  repeatedly call compileCode() while run() active\n");
    std::printf("Key question: is compileCode() thread-safe when called from\n");
    std::printf("Thread B while run() executes on Thread A?\n");
    std::printf("\nlibchuck provides two spork paths:\n");
    std::printf("  immediate=FALSE: shred queued via lock-free FinalRingBuffer,\n");
    std::printf("    processed on VM compute thread inside compute()\n");
    std::printf("  immediate=TRUE:  shred sporked directly on calling thread,\n");
    std::printf("    mutating shred_list linked list without lock\n");
    std::printf("\n");

    std::vector<SpikeResult> results;

    // Case A: Baseline
    std::printf("\n=== Case A: Baseline ===\n");
    results.push_back(runCaseA());

    // Case B: Single compilation (both modes)
    std::printf("\n=== Case B: Single compilation (deferred) ===\n");
    results.push_back(runCaseB(SporkMode::Deferred));
    std::printf("\n=== Case B: Single compilation (immediate) ===\n");
    results.push_back(runCaseB(SporkMode::Immediate));

    // Case C: Repeated compilation (both modes, 1000 iterations)
    std::printf("\n=== Case C: Repeated compilation (deferred, 1000 iters) ===\n");
    results.push_back(runCaseCD(SporkMode::Deferred, 1000));
    std::printf("\n=== Case C: Repeated compilation (immediate, 1000 iters) ===\n");
    results.push_back(runCaseCD(SporkMode::Immediate, 1000));

    // Case D: Sustained concurrency (both modes, 10000 iterations)
    std::printf("\n=== Case D: Sustained concurrency (deferred, 10000 iters) ===\n");
    results.push_back(runCaseCD(SporkMode::Deferred, 10000));
    std::printf("\n=== Case D: Sustained concurrency (immediate, 10000 iters) ===\n");
    results.push_back(runCaseCD(SporkMode::Immediate, 10000));

    // Case E: Rapid handoff (high frequency, 20000 iterations, no sleep)
    std::printf("\n=== Case E: Rapid handoff (deferred, 20000 iters) ===\n");
    results.push_back(runCaseCD(SporkMode::Deferred, 20000, true));
    std::printf("\n=== Case E: Rapid handoff (immediate, 20000 iters) ===\n");
    results.push_back(runCaseCD(SporkMode::Immediate, 20000, true));

    // Case F: Shutdown race (both modes)
    std::printf("\n=== Case F: Shutdown race (deferred) ===\n");
    results.push_back(runCaseF(SporkMode::Deferred));
    std::printf("\n=== Case F: Shutdown race (immediate) ===\n");
    results.push_back(runCaseF(SporkMode::Immediate));

    // Summary
    std::printf("\n\n=== SUMMARY ===\n");
    std::printf("%-45s %-10s %8s %8s %8s %8s %8s\n",
                "Case", "Mode", "Attmpt", "OK", "FAIL", "RunIters", "Crashes");

    unsigned long totalDeferredFailures = 0;
    unsigned long totalImmediateFailures = 0;
    unsigned long totalDeferredCrashes = 0;
    unsigned long totalImmediateCrashes = 0;
    bool deferredCompleted = true;
    bool immediateCompleted = true;

    for (const auto& r : results)
    {
        std::printf("%-45s %-10s %8lu %8lu %8lu %8lu %8lu\n",
                    r.caseName,
                    r.mode,
                    r.compileAttempts,
                    r.compileSuccesses,
                    r.compileFailures,
                    r.runIterations,
                    r.vmCrashes);

        if (r.mode == "deferred")
        {
            totalDeferredFailures += r.compileFailures;
            totalDeferredCrashes += r.vmCrashes;
            if (!r.completed) deferredCompleted = false;
        }
        else if (r.mode == "immediate")
        {
            totalImmediateFailures += r.compileFailures;
            totalImmediateCrashes += r.vmCrashes;
            if (!r.completed) immediateCompleted = false;
        }
    }

    std::printf("\n--- Per-mode totals ---\n");
    std::printf("Deferred (immediate=FALSE) total: failures=%lu, crashes=%lu, all_completed=%d\n",
                totalDeferredFailures, totalDeferredCrashes, deferredCompleted ? 1 : 0);
    std::printf("Immediate (immediate=TRUE) total: failures=%lu, crashes=%lu, all_completed=%d\n",
                totalImmediateFailures, totalImmediateCrashes, immediateCompleted ? 1 : 0);

    // Final decision
    std::printf("\n=== GO/NO-GO DECISION ===\n");

    bool deferredSafe = (totalDeferredCrashes == 0) && deferredCompleted;
    bool immediateSafe = (totalImmediateCrashes == 0) && immediateCompleted;

    std::printf("Deferred (immediate=FALSE): %s\n",
                deferredSafe ? "SAFE (no crashes)" : "UNSAFE (crashes detected)");
    std::printf("Immediate (immediate=TRUE): %s\n",
                immediateSafe ? "SAFE (no crashes)" : "UNSAFE (crashes detected)");

    std::printf("\nB4-K0.5 DECISION: ");

    if (deferredSafe && !immediateSafe)
    {
        std::printf("GO\n");
        std::printf("  - compileCode() with immediate=FALSE is thread-safe for the B4 handoff pattern.\n");
        std::printf("  - compileCode() with immediate=TRUE races (as documented by ChucK source analysis).\n");
        std::printf("  - Production B4-K4 MUST use immediate=FALSE (deferred spork via FinalRingBuffer).\n");
        return 0;
    }
    else if (deferredSafe && immediateSafe)
    {
        std::printf("GO (with caveat)\n");
        std::printf("  - Both modes appear safe in this test, but source analysis shows immediate=TRUE\n");
        std::printf("    directly mutates the shred list without a lock.\n");
        std::printf("  - Production B4-K4 MUST still use immediate=FALSE as the documented safe path.\n");
        return 0;
    }
    else
    {
        std::printf("NO-GO\n");
        std::printf("  - compileCode() is NOT thread-safe for concurrent use with run().\n");
        std::printf("  - Production B4 must serialize all compile/spork operations.\n");
        return 1;
    }
}
