// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PermissionPromptComponent.hpp — Inline permission prompt for ChatSidebar.
 *
 * Renders a non-blocking inline permission prompt inside ChatSidebar.
 * Shows the permission option buttons and a live 30-second countdown.
 * Calls onRespond(requestId, optionId) when the user picks an option.
 * Dismisses itself (setVisible(false)) after responding or on timeout.
 *
 * Usage:
 *   auto* prompt = new PermissionPromptComponent(requestId, options,
 *       [this](int id, std::string opt) {
 *           session_.respondPermission(id, opt);
 *           // remove prompt from layout / resized()
 *       });
 *   addAndMakeVisible(prompt);
 *   prompt->start();
 *
 * Requirements: 32.6
 */

#include <functional>
#include <string>

#include <nlohmann/json.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

namespace hathor::ui {

/**
 * PermissionPromptComponent
 *
 * A self-contained JUCE Component that:
 *   - Displays a "Agent is requesting permission:" label
 *   - Renders one TextButton per option from the JSON options array
 *   - Shows a live countdown (30 → 0 seconds) updated via juce::Timer at 1 Hz
 *   - Calls onRespond_ with the chosen optionId when a button is clicked
 *   - Auto-responds with "cancelled" when the countdown reaches zero
 *   - Hides itself (setVisible(false)) after any response
 *
 * Requirements: 32.6
 */
class PermissionPromptComponent : public juce::Component,
                                  public juce::Timer
{
public:
    /// Callback invoked on the JUCE message thread when the user responds or the
    /// countdown expires.  Parameters: requestId (from JSON-RPC), optionId chosen.
    using OnRespondFn = std::function<void(int requestId, std::string optionId)>;

    /**
     * @param requestId   The JSON-RPC request id (passed through to onRespond)
     * @param options     The permission options array from session/request_permission params.
     *                    Each element may contain "id", "title", and/or "description".
     * @param onRespond   Callback invoked (on message thread) when user responds or
     *                    the timeout fires.
     */
    PermissionPromptComponent(int requestId,
                              const nlohmann::json& options,
                              OnRespondFn onRespond);

    ~PermissionPromptComponent() override;

    /**
     * Start the 30-second countdown timer.
     * Call this after adding the component to its parent with addAndMakeVisible().
     */
    void start();

    /**
     * Force-respond with "cancelled" and hide the component.
     * Safe to call if the user has already responded (no-op in that case).
     * May be called by external timeout logic in AcpAgentSession as a belt-and-suspenders
     * guard, though the component manages its own 30-second timer independently.
     */
    void cancelIfPending();

    // -----------------------------------------------------------------------
    // juce::Component overrides
    // -----------------------------------------------------------------------
    void resized() override;
    void paint(juce::Graphics& g) override;

    // -----------------------------------------------------------------------
    // juce::Timer override
    // -----------------------------------------------------------------------
    void timerCallback() override;

private:
    /// Respond with the given optionId.  No-op if already responded.
    /// Stops the timer, fires onRespond_, and hides the component.
    void respond(const std::string& optionId);

    // -----------------------------------------------------------------------
    // Data
    // -----------------------------------------------------------------------
    int             requestId_;
    nlohmann::json  options_;       ///< JSON array of option objects
    OnRespondFn     onRespond_;
    bool            responded_   = false;
    int             secondsLeft_ = 30;

    // -----------------------------------------------------------------------
    // Child components
    // -----------------------------------------------------------------------
    juce::Label     promptLabel_;
    juce::Label     countdownLabel_;
    juce::OwnedArray<juce::TextButton> optionButtons_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PermissionPromptComponent)
};

} // namespace hathor::ui
