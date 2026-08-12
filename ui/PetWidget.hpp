// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * PetWidget.hpp — the animated Petdex mascot (Phase G / D2–D4).
 *
 * A small always-on-top corner widget. It owns the *display* of the selected
 * pet and enforces the D4 gate at render time:
 *
 *   NoPet             no selection (widget invisible)
 *   Loading           selection applied, sprite being fetched/decoded
 *   Ready             sprite decoded, attribution verified — animating
 *   AttributionBlocked  selection cannot be displayed: attribution/licensing
 *                       information could not be established (D4 refuses)
 *   Unavailable       sprite download/decode/grid analysis failed
 *
 * The D4 gate is not informational: a pet whose attribution snapshot has
 * canDisplay == false is NEVER drawn — the widget renders an explanatory
 * blocked state instead. Every displayed pet shows its attribution credit as
 * a caption (and the full credit line + license notice as a tooltip).
 *
 * Animation is a pure UI concern: a juce::Timer on the message thread drives
 * PetdexAnimation (deterministic timing from the verified state convention).
 * It never executes on the audio thread and performs no allocation in steady
 * state. App-state reactivity is minimal by design: an injected activity probe
 * (e.g. "any slot playing") maps to the "running" state; otherwise "idle".
 */

#include <juce_gui_basics/juce_gui_basics.h>

#include "PetdexAnimation.hpp"
#include "PetdexTypes.hpp"

#include <functional>
#include <string>

namespace hathor::ui {

class PetWidget : public juce::Component,
                  public juce::SettableTooltipClient,   // JUCE 8: setTooltip mixin
                  private juce::Timer
{
public:
    enum class Status
    {
        NoPet,
        Loading,
        Ready,
        AttributionBlocked,
        Unavailable,
    };

    PetWidget();
    ~PetWidget() override = default;

    // -----------------------------------------------------------------------
    // Public API (message thread)
    // -----------------------------------------------------------------------

    /// Set (or clear, when snapshot.slug is empty) the selected pet. The D4
    /// gate is enforced here: a non-displayable snapshot puts the widget into
    /// AttributionBlocked and the pet is never drawn.
    void setSelectedPet(const PetdexAttributionSnapshot& snapshot);

    /// Clear the selection back to NoPet.
    void clearPet();

    /// Deliver a decoded sprite from PetdexResourceService.
    void onSpriteResult(const PetdexSpriteResult& result);

    /// Inject an app-state probe (called from the animation timer; e.g.
    /// returns true while any pattern slot is playing → "running" state).
    void setActivityProbe(std::function<bool()> probe) noexcept
    {
        activityProbe_ = std::move(probe);
    }

    /// Notified whenever the display status changes (MainWindow re-lays-out).
    std::function<void(Status)> onStatusChanged;

    Status status() const noexcept { return status_; }

    static constexpr int kPetWidth  = 96;
    static constexpr int kPetHeight = 104;

    // juce::Component
    void paint(juce::Graphics& g) override;

private:
    void timerCallback() override;
    void setStatus(Status status);
    void buildSheetImage(const PetdexSpriteResult& result);
    void drawNotice(juce::Graphics& g, const char* glyph, juce::Colour colour);
    static juce::Image rgbaToArgbImage(const std::vector<std::uint8_t>& rgba,
                                       int width, int height);

    Status status_ = Status::NoPet;
    PetdexAttributionSnapshot snapshot_;
    PetdexFrameGrid grid_;
    PetdexAnimation anim_;
    juce::Image sheet_;
    std::function<bool()> activityProbe_;
    bool working_ = false;
    std::string errorMessage_;

    static constexpr int kTickMs        = 40;      ///< ~25 fps animation timer
    static constexpr int kCaptionHeight = 14;      ///< attribution caption strip

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PetWidget)
};

} // namespace hathor::ui
