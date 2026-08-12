// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetdexAnimation.hpp"

#include <algorithm>

namespace hathor::ui {

void PetdexAnimation::configure(const PetdexFrameGrid& grid) noexcept
{
    grid_ = grid;
    frame_ = 0;
    elapsedInStateMs_ = 0;
    stateId_.clear();
    if (grid_.valid)
        stateId_ = PetdexFrameGrid::kDefaultStateId;
}

void PetdexAnimation::setState(const std::string& id) noexcept
{
    // Resolve now so an unknown id collapses to idle immediately; only commit
    // a real change (transition resets the clock).
    const auto* next = resolveState(id);
    const std::string nextId = (next != nullptr) ? next->id
                                                 : std::string(PetdexFrameGrid::kDefaultStateId);

    if (!grid_.valid || nextId != stateId_)
    {
        frame_ = 0;
        elapsedInStateMs_ = 0;
    }
    stateId_ = nextId;
}

const PetdexAnimationState* PetdexAnimation::resolveState(const std::string& id) const noexcept
{
    if (!grid_.valid)
        return nullptr;

    if (const auto* state = grid_.findState(id))
        if (state->frames > 0)
            return state;

    return grid_.findState(PetdexFrameGrid::kDefaultStateId);
}

int PetdexAnimation::advance(int elapsedMs) noexcept
{
    if (!grid_.valid)
    {
        frame_ = 0;
        return frame_;
    }

    elapsedInStateMs_ += std::max(0, elapsedMs);

    const auto* state = resolveState(stateId_);
    if (state == nullptr || state->frames <= 0)
    {
        // Inert fallback (no idle row on the sheet).
        frame_ = 0;
        return frame_;
    }

    const int perFrameMs = std::max(1, state->durationMs / state->frames);
    frame_ = (elapsedInStateMs_ / perFrameMs) % state->frames;
    return frame_;
}

int PetdexAnimation::frameCount() const noexcept
{
    const auto* state = resolveState(stateId_);
    return state != nullptr ? state->frames : 0;
}

int PetdexAnimation::currentStateRow() const noexcept
{
    const auto* state = resolveState(stateId_);
    return state != nullptr ? state->row : 0;
}

} // namespace hathor::ui
