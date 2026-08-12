// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * BreadcrumbsBar.cpp — implementation.
 *
 * Requirement references: L-1 §5
 */

#include "BreadcrumbsBar.hpp"

namespace hathor::ui {

BreadcrumbsBar::BreadcrumbsBar()
{
    buildCrumbs(juce::File());
}

BreadcrumbsBar::~BreadcrumbsBar() = default;

void BreadcrumbsBar::setCurrentFile(const juce::File& fullPath, juce::String editorName)
{
    editorName_ = std::move(editorName);
    buildCrumbs(fullPath);
    repaint();
}

void BreadcrumbsBar::clear()
{
    editorName_.clear();
    buildCrumbs(juce::File());
    repaint();
}

void BreadcrumbsBar::resized()
{
    const int iconW = 16;
    const int iconSpacing = 8;

    // Right-side buttons
    juce::Rectangle<int> area(getLocalBounds());
    splitBtn_ = area.removeFromRight(iconW + 8);
    area.removeFromRight(iconSpacing);
    findBtn_ = area.removeFromRight(iconW + 8);
    area.removeFromRight(iconSpacing);
    commandPaletteBtn_ = area.removeFromRight(iconW + 8);
    area.removeFromRight(iconSpacing);

    // Crumbs fill the remaining area
    int x = area.getX() + 8;
    const int y = area.getY() + (area.getHeight() - 16) / 2;

    for (auto& crumb : crumbs_)
    {
        juce::Font font(juce::FontOptions{}.withHeight(14.0f));
        int w = static_cast<int>(juce::GlyphArrangement::getStringWidth(font, crumb.label)) + 12;

        crumb.bounds = juce::Rectangle<int>(x, y, w, 16);
        x += w + 6;
    }
}

void BreadcrumbsBar::paint(juce::Graphics& g)
{
    juce::Font font(juce::FontOptions{}.withHeight(14.0f));

    // Background
    g.fillAll(juce::Colours::darkgrey.darker(0.6f));

    // Draw breadcrumbs (right-to-left: each segment)
    for (size_t i = 0; i < crumbs_.size(); ++i)
    {
        const auto& crumb = crumbs_[i];
        if (i > 0)
        {
            g.setColour(juce::Colours::grey.darker(0.3f));
            g.drawLine(static_cast<float>(crumb.bounds.getX() - 4),
                       static_cast<float>(crumb.bounds.getY() + crumb.bounds.getHeight() / 2),
                       static_cast<float>(crumb.bounds.getX()),
                       static_cast<float>(crumb.bounds.getY() + crumb.bounds.getHeight() / 2), 1.0f);
        }

        g.setColour(crumb.label.isEmpty() ? juce::Colours::grey : juce::Colours::lightgrey);
        g.setFont(font);
        g.drawFittedText(crumb.label,
                         crumb.bounds.reduced(4, 0),
                         juce::Justification::centredLeft, 1);
    }

    // Editor name tag
    if (!editorName_.isEmpty())
    {
        juce::Rectangle<int> tagBounds(4, 2, 60, 18);
        g.setColour(juce::Colours::blue.darker(0.2f));
        g.fillRoundedRectangle(
            static_cast<float>(tagBounds.getX()),
            static_cast<float>(tagBounds.getY()),
            static_cast<float>(tagBounds.getWidth()),
            static_cast<float>(tagBounds.getHeight()), 3.0f);
        g.setColour(juce::Colours::white);
        g.setFont(font);
        g.drawFittedText(editorName_,
                         tagBounds.reduced(4, 2),
                         juce::Justification::centred, 1);
    }

    // Icon buttons (simplified as text)
    g.setColour(juce::Colours::lightgrey);
    g.setFont(font);
    g.drawText("⚡", commandPaletteBtn_, juce::Justification::centred, true);
    g.drawText("🔍", findBtn_, juce::Justification::centred, true);
    g.drawText("⊞", splitBtn_, juce::Justification::centred, true);
}

void BreadcrumbsBar::mouseDown(const juce::MouseEvent& e)
{
    // Check icon buttons
    if (commandPaletteBtn_.contains(e.position.toInt()) && onCommandPaletteClicked)
        onCommandPaletteClicked();
    else if (findBtn_.contains(e.position.toInt()) && onFindClicked)
        onFindClicked();
    else if (splitBtn_.contains(e.position.toInt()) && onSplitClicked)
        onSplitClicked();

    // Check crumbs
    for (const auto& crumb : crumbs_)
    {
        if (crumb.bounds.contains(e.position.toInt()))
        {
            if (onBreadcrumbClicked && crumb.file == juce::File())
                break;  // root crumb is not a file
            if (onBreadcrumbClicked && crumb.file != juce::File())
                onBreadcrumbClicked(crumb.file);
            break;
        }
    }
}

void BreadcrumbsBar::buildCrumbs(const juce::File& file)
{
    crumbs_.clear();

    if (file == juce::File())
        return;

    // Build path segments from root down to leaf
    std::vector<juce::File> pathParts;
    juce::File current = file;
    while (current != juce::File())
    {
        pathParts.push_back(current);
        current = current.getParentDirectory();
    }

    // Reverse to get root-first order
    std::reverse(pathParts.begin(), pathParts.end());

    for (size_t i = 0; i < pathParts.size(); ++i)
    {
        Crumb crumb;
        crumb.file = pathParts[i];
        crumb.label = pathParts[i].getFileName().isNotEmpty()
                          ? pathParts[i].getFileName()
                          : pathParts[i].getVolumeLabel().isNotEmpty()
                              ? pathParts[i].getVolumeLabel()
                              : "/";
        crumbs_.push_back(crumb);
    }
}

} // namespace hathor::ui
