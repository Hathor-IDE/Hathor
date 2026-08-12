// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PetWidget.hpp"

#include "HathorLookAndFeel.hpp"

#include <algorithm>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PetWidget::PetWidget()
{
    setAlwaysOnTop(true);
    setInterceptsMouseClicks(true, true);
    startTimer(kTickMs);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void PetWidget::setSelectedPet(const PetdexAttributionSnapshot& snapshot)
{
    if (snapshot.slug.empty())
    {
        clearPet();
        return;
    }

    snapshot_ = snapshot;
    sheet_    = {};
    grid_     = PetdexFrameGrid::analyze(0, 0);   // invalid until a sprite arrives
    errorMessage_.clear();
    working_ = false;

    // ---- D4 gate (blocking) ----
    if (!snapshot_.canDisplay)
    {
        setStatus(Status::AttributionBlocked);
        return;
    }

    setStatus(Status::Loading);
}

void PetWidget::clearPet()
{
    snapshot_ = {};
    sheet_    = {};
    grid_     = PetdexFrameGrid::analyze(0, 0);
    errorMessage_.clear();
    working_ = false;
    setStatus(Status::NoPet);
}

void PetWidget::onSpriteResult(const PetdexSpriteResult& result)
{
    // Ignore results for pets we are no longer showing (selection changed).
    if (result.slug != snapshot_.slug)
        return;

    if (!result.ok)
    {
        errorMessage_ = result.error.empty() ? "Sprite unavailable." : result.error;
        setStatus(Status::Unavailable);
        return;
    }

    buildSheetImage(result);

    grid_ = PetdexFrameGrid::analyze(result.width, result.height);
    if (!grid_.valid)
    {
        errorMessage_ = grid_.error;
        setStatus(Status::Unavailable);
        return;
    }

    anim_.configure(grid_);
    anim_.setState(PetdexFrameGrid::kDefaultStateId);
    setStatus(Status::Ready);
}

// ---------------------------------------------------------------------------
// juce::Timer — animation (message thread only; never the audio thread)
// ---------------------------------------------------------------------------

void PetWidget::timerCallback()
{
    if (status_ != Status::Ready)
        return;

    const bool busy = activityProbe_ ? activityProbe_() : false;
    if (busy != working_)
    {
        working_ = busy;
        anim_.setState(busy ? "running" : PetdexFrameGrid::kDefaultStateId);
    }

    anim_.advance(kTickMs);
    repaint();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void PetWidget::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    switch (status_)
    {
        case Status::NoPet:
            return;

        case Status::Loading:
        {
            g.setColour(palette.textMuted);
            g.setFont(HathorLookAndFeel::fontMedium(11.0f));
            g.drawText("Loading\xE2\x80\xA6", getLocalBounds(),
                       juce::Justification::centred);
            return;
        }

        case Status::AttributionBlocked:
        {
            drawNotice(g, "!", palette.warning);
            return;
        }

        case Status::Unavailable:
        {
            drawNotice(g, "!", palette.error);
            return;
        }

        case Status::Ready:
        {
            const auto frame = grid_.frameRect(
                anim_.currentStateRow(),
                anim_.currentFrame());

            const juce::Rectangle<int> petArea(
                0, 0, getWidth(), getHeight() - kCaptionHeight);

            // JUCE 8 drawImage has no source-rectangle overload; slice the
            // sheet with getClippedImage (a shared reference, no pixel copy)
            // and stretch that single frame into the widget area.
            const juce::Image frameImage = sheet_.getClippedImage(
                juce::Rectangle<int>(frame.x, frame.y, frame.w, frame.h));
            g.drawImage(frameImage, petArea.toFloat(),
                        juce::RectanglePlacement::stretchToFit);

            // D4: attribution credit is always visible for a displayed pet.
            juce::String credit = snapshot_.displayName;
            if (!snapshot_.submitter.empty())
                credit = credit + " \xC2\xB7 " + juce::String(snapshot_.submitter);
            g.setColour(palette.textMuted);
            g.setFont(HathorLookAndFeel::fontRegular(9.0f));
            g.drawFittedText(credit,
                             juce::Rectangle<int>(0, getHeight() - kCaptionHeight,
                                                  getWidth(), kCaptionHeight),
                             juce::Justification::centred, 1);
            return;
        }
    }
}

void PetWidget::drawNotice(juce::Graphics& g, const char* glyph, juce::Colour colour)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    const auto bounds = getLocalBounds();

    g.setColour(palette.surfaceLow);
    g.fillRoundedRectangle(bounds.toFloat(), 6.0f);

    const float r = 14.0f;
    g.setColour(colour);
    g.fillEllipse(bounds.getCentreX() - r, bounds.getCentreY() - r,
                  2.0f * r, 2.0f * r);
    g.setColour(palette.background);
    g.setFont(HathorLookAndFeel::fontSemiBold(13.0f));
    g.drawText(juce::String(glyph), juce::Rectangle<int>(
                   bounds.getCentreX() - static_cast<int>(r),
                   bounds.getCentreY() - static_cast<int>(r),
                   static_cast<int>(2.0f * r), static_cast<int>(2.0f * r)),
               juce::Justification::centred);
}

// ---------------------------------------------------------------------------
// Internal
// ---------------------------------------------------------------------------

void PetWidget::setStatus(Status status)
{
    if (status_ == status)
        return;
    status_ = status;

    switch (status_)
    {
        case Status::Ready:
            setTooltip(snapshot_.creditLine + "\n" + snapshot_.notice);
            break;
        case Status::AttributionBlocked:
            setTooltip(snapshot_.notice);
            break;
        case Status::Unavailable:
            setTooltip(errorMessage_);
            break;
        case Status::Loading:
            setTooltip("Downloading the selected pet\xE2\x80\xA6");
            break;
        case Status::NoPet:
            setTooltip({});
            break;
    }

    if (onStatusChanged)
        onStatusChanged(status_);
    repaint();
}

void PetWidget::buildSheetImage(const PetdexSpriteResult& result)
{
    if (result.rgba.size() <
        static_cast<std::size_t>(result.width) * static_cast<std::size_t>(result.height) * 4u)
    {
        errorMessage_ = "Decoded sprite data is incomplete.";
        setStatus(Status::Unavailable);
        return;
    }
    sheet_ = rgbaToArgbImage(result.rgba, result.width, result.height);
}

juce::Image PetWidget::rgbaToArgbImage(const std::vector<std::uint8_t>& rgba,
                                       int width, int height)
{
    // libwebp gives unpremultiplied R,G,B,A. JUCE Image::ARGB is PREMULTIPLIED
    // with memory byte order [B,G,R,A] on little-endian (PixelARGB: components
    // are b,g,r,a). Verified against the JUCE 8.0.4 sources.
    juce::Image image(juce::Image::ARGB, width, height, true);
    juce::Image::BitmapData bmp(image, juce::Image::BitmapData::writeOnly);

    for (int y = 0; y < height; ++y)
    {
        auto* dst = bmp.getLinePointer(y);
        const auto* src = rgba.data()
                        + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4u;
        for (int x = 0; x < width; ++x)
        {
            const std::size_t i = static_cast<std::size_t>(x) * 4u;
            const int a = src[i + 3];
            dst[i + 0] = static_cast<std::uint8_t>((src[i + 2] * a) / 255u); // B
            dst[i + 1] = static_cast<std::uint8_t>((src[i + 1] * a) / 255u); // G
            dst[i + 2] = static_cast<std::uint8_t>((src[i + 0] * a) / 255u); // R
            dst[i + 3] = static_cast<std::uint8_t>(a);                       // A
        }
    }
    return image;
}

} // namespace hathor::ui
