// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * VMManager.cpp — per-tab ChucK VM registry implementation (B4-K3).
 */

#include "VMManager.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

namespace hathor::audio_worker {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

VMManager::VMManager()
{
}

VMManager::~VMManager()
{
    // Explicitly destroy all VMs to ensure threads are joined before shutdown.
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [tabId, vm] : vms_) {
        if (vm) {
            vm->destroy();
        }
    }
    vms_.clear();
    lruList_.clear();
}

// ---------------------------------------------------------------------------
// Lifecycle control
// -----------------------------------------------------------------------

VMResult VMManager::activateVM(TabId tabId, unsigned sampleRate, unsigned channels)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Enforce the resource ceiling before creating a new VM.
    int activeCount = countActiveLocked();
    if (!policy_.canActivate(activeCount)) {
        // Ceiling reached — apply configured behavior.
        if (!handleCeilingEviction()) {
            // Could not evict; reject based on ceiling behavior.
            if (policy_.ceilingBehavior == CeilingBehavior::RejectWithError) {
                return {false, 429, "resource ceiling reached: maxConcurrentLiveVMs="
                                    + std::to_string(policy_.maxConcurrentLiveVMs)
                                    + " tab=" + std::to_string(tabId)};
            }
        }

        // Re-check after eviction attempt.
        activeCount = countActiveLocked();
        if (!policy_.canActivate(activeCount)) {
            if (policy_.ceilingBehavior == CeilingBehavior::RejectWithError) {
                return {false, 429, "resource ceiling reached after eviction attempt"};
            }
            // For LRU behaviors, we fell through — proceed to create
            // (eviction made room, or this tab's VM already exists).
        }
    }

    // Get or create the VM for this tab.
    ChuckVM* vm = getOrCreateVM(tabId);
    if (!vm) {
        return {false, 1, "failed to create VM for tab " + std::to_string(tabId)};
    }

    // Activate (or resume if suspended).
    VMResult result;
    if (vm->state() == VMState::Suspended) {
        result = vm->resume();
    } else {
        result = vm->activate(sampleRate, channels);
    }

    if (result.ok) {
        // Update LRU: move this tab to the MRU position.
        lruList_.erase(std::remove(lruList_.begin(), lruList_.end(), tabId), lruList_.end());
        lruList_.push_back(tabId);
        lastActiveTs_[tabId] = nowMs();
    }

    return result;
}

VMResult VMManager::deactivateVM(TabId tabId, bool suspend)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = vms_.find(tabId);
    if (it == vms_.end() || !it->second) {
        return {true, 0, "no VM for tab " + std::to_string(tabId)};
    }

    VMResult result = it->second->deactivate(suspend);

    if (result.ok) {
        lastPauseTs_[tabId] = nowMs();
        lruList_.erase(std::remove(lruList_.begin(), lruList_.end(), tabId), lruList_.end());
    }

    return result;
}

VMResult VMManager::resumeVM(TabId tabId)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = vms_.find(tabId);
    if (it == vms_.end() || !it->second) {
        return {false, 2, "no VM for tab " + std::to_string(tabId)};
    }

    VMResult result = it->second->resume();
    if (result.ok) {
        lastActiveTs_[tabId] = nowMs();
        lruList_.erase(std::remove(lruList_.begin(), lruList_.end(), tabId), lruList_.end());
        lruList_.push_back(tabId);
    }

    return result;
}

VMResult VMManager::destroyVM(TabId tabId)
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = vms_.find(tabId);
    if (it == vms_.end()) {
        return {true, 0, "no VM for tab " + std::to_string(tabId)};
    }

    if (it->second) {
        it->second->destroy();
    }

    vms_.erase(it);
    lruList_.erase(std::remove(lruList_.begin(), lruList_.end(), tabId), lruList_.end());
    lastActiveTs_.erase(tabId);
    lastPauseTs_.erase(tabId);

    return {true, 0, "vm destroyed for tab " + std::to_string(tabId)};
}

VMResult VMManager::compileVM(TabId tabId, const std::string& code)
{
    // K0.5: serialize compilation per VM.  The mutex ensures only one
    // compile is in flight for this VM at a time.
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = vms_.find(tabId);
    if (it == vms_.end() || !it->second) {
        return {false, 3, "no VM for tab " + std::to_string(tabId)
                          + " — activate first"};
    }

    // K0.5: ChuckVM::compileCode() enforces that the VM is not in a
    // concurrent compile/run state.
    return it->second->compileCode(code);
}

// ---------------------------------------------------------------------------
// Status / introspection
// -----------------------------------------------------------------------

VMResult VMManager::queryVM(TabId tabId) const
{
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = vms_.find(tabId);
    if (it == vms_.end() || !it->second) {
        return {true, 0, "tab=" + std::to_string(tabId) + " state=inactive blocks=0 heartbeat=0 memory=0"};
    }

    auto* vm = it->second.get();
    VMState state = vm->state();
    uint64_t blocks = vm->blocksProduced();
    uint64_t beat = vm->heartbeat();
    std::size_t mem = vm->memoryUsage();

    return {true, 0, "tab=" + std::to_string(tabId)
          + " state=" + [&state]{
              switch (state) {
                  case VMState::Inactive:   return "inactive";
                  case VMState::Active:     return "active";
                  case VMState::Suspended:  return "suspended";
                  case VMState::Destroyed:  return "destroyed";
                  case VMState::Error:      return "error";
              }
              return "unknown";
          }()
          + " blocks=" + std::to_string(blocks)
          + " heartbeat=" + std::to_string(beat)
          + " memory=" + std::to_string(mem)};
}

std::string VMManager::listVMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    int active = 0, suspended = 0, destroyed = 0;
    for (const auto& [tabId, vm] : vms_) {
        if (!vm) continue;
        switch (vm->state()) {
            case VMState::Active:     active++; break;
            case VMState::Suspended:  suspended++; break;
            case VMState::Destroyed:  destroyed++; break;
            default: break;
        }
    }

    return "ok vm_list count=" + std::to_string(vms_.size())
         + " active=" + std::to_string(active)
         + " suspended=" + std::to_string(suspended)
         + " destroyed=" + std::to_string(destroyed);
}

// ---------------------------------------------------------------------------
// Counting helpers
// ---------------------------------------------------------------------------

int VMManager::countStateLocked(VMState state) const
{
    int count = 0;
    for (const auto& [tabId, vm] : vms_) {
        if (vm && vm->state() == state) {
            count++;
        }
    }
    return count;
}

int VMManager::countState(VMState state) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return countStateLocked(state);
}

int VMManager::countActiveLocked() const
{
    return countStateLocked(VMState::Active);
}

int VMManager::countActive() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return countActiveLocked();
}

bool VMManager::canActivateMore() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return policy_.canActivate(countActiveLocked());
}

ResourcePolicy VMManager::getPolicy() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return policy_;
}

void VMManager::setPolicy(const ResourcePolicy& policy)
{
    std::lock_guard<std::mutex> lock(mutex_);
    policy_ = policy;
}

void VMManager::setRenderCallback(ChuckVM::RenderCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    renderCallback_ = cb;
}

// ---------------------------------------------------------------------------
// Watchdog integration (B4-K5)
// ---------------------------------------------------------------------------

int VMManager::checkHeartbeats(std::chrono::milliseconds timeout)
{
    // This is a legacy entry point.  The actual per-VM watchdog is implemented
    // in VmWatchdog, which runs as a dedicated thread in the worker process.
    // This method is retained for API compatibility and for tests that want
    // to trigger a single check cycle manually.
    //
    // Per PROGRAM.md B4-K5, the watchdog must:
    //   - Only monitor VMs in the Live/Active state (not suspended/stopping/etc.)
    //   - Use the per-VM heartbeat (ChuckVM::heartbeat()), not the worker-level
    //     shared-memory heartbeat (which detects worker death, not per-VM hangs).
    //
    // The actual stall detection + recovery is delegated to VmWatchdog.
    // Here we just count how many active VMs would be checked.
    int checked = 0;
    for (const auto& [tabId, vm] : vms_) {
        if (!vm) continue;
        if (vm->state() == VMState::Active) {
            ++checked;
        }
    }
    (void)timeout;
    return checked;
}

// ---------------------------------------------------------------------------
// VM lookup for watchdog (B4-K5)
// ---------------------------------------------------------------------------

ChuckVM* VMManager::findVM(TabId tabId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = vms_.find(tabId);
    if (it == vms_.end() || !it->second)
        return nullptr;
    return it->second.get();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

ChuckVM* VMManager::getOrCreateVM(TabId tabId)
{
    auto it = vms_.find(tabId);
    if (it != vms_.end() && it->second) {
        return it->second.get();
    }

    // Create a new VM for this tab.
    // If we have a render callback, use it; otherwise use silence.
    if (renderCallback_) {
        vms_[tabId] = std::make_unique<ChuckVM>(tabId, renderCallback_);
    } else {
        // Default: silence callback.
        vms_[tabId] = std::make_unique<ChuckVM>(tabId,
            [](float* outBuf, unsigned numFrames, unsigned /*numChannels*/) {
                std::memset(outBuf, 0, numFrames * sizeof(float));
            });
    }

    return vms_[tabId].get();
}

bool VMManager::handleCeilingEviction()
{
    // Evict the least-recently-used suspended VM (not active VMs).
    // LRU list is ordered oldest → newest; we iterate from the front
    // and find the first suspended VM to evict.
    for (TabId lruTab : lruList_) {
        auto it = vms_.find(lruTab);
        if (it != vms_.end() && it->second) {
            if (it->second->state() == VMState::Suspended) {
                // Destroy the LRU suspended VM to make room.
                it->second->destroy();
                vms_.erase(it);
                lruList_.erase(
                    std::remove(lruList_.begin(), lruList_.end(), lruTab),
                    lruList_.end());
                lastActiveTs_.erase(lruTab);
                lastPauseTs_.erase(lruTab);
                return true;
            }
        }
    }

    // No suspended VM to evict — ceiling cannot be relieved.
    return false;
}

void VMManager::checkIdleSuspension()
{
    // Per PROGRAM.md B4-K3 §5: idle tabs should be suspended or destroyed
    // per policy.  This is called periodically by the worker's idle checker.
    auto now = nowMs();

    for (auto& [tabId, vm] : vms_) {
        if (!vm) continue;
        if (vm->state() != VMState::Active) continue;

        auto it = lastActiveTs_.find(tabId);
        if (it == lastActiveTs_.end()) continue;

        auto idleDuration = now - it->second;
        if (idleDuration > std::chrono::milliseconds(policy_.idleSuspendTimeoutSec * 1000)) {
            // Idle timeout exceeded — suspend or destroy per policy.
            if (policy_.preferSuspendOverDestroy) {
                vm->deactivate(true);  // suspend
            } else {
                vm->deactivate(false); // destroy
            }
        }
    }
}

} // namespace hathor::audio_worker
