// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ExplorerTreeItems.cpp — JUCE TreeViewItem implementations.
 */

#include "ExplorerTreeItems.hpp"

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Helper: fetch the current Palette from the owning TreeView's LookAndFeel.
// ---------------------------------------------------------------------------

static const Palette& paletteFor(const juce::TreeViewItem& item) noexcept
{
    const juce::TreeView* tv = item.getOwnerView();
    if (tv != nullptr)
    {
        const juce::LookAndFeel& lf = tv->getLookAndFeel();
        if (auto* hlf = dynamic_cast<const HathorLookAndFeel*>(&lf))
            return hlf->getPalette();
    }
    // Fallback: use the global palette accessor.
    return HathorLookAndFeel::globalPalette();
}

// ===========================================================================
// SongTreeItem
// ===========================================================================

SongTreeItem::SongTreeItem(SongNode node, SongClickedCallback onClicked)
    : node_(std::move(node)),
      onSongClicked_(std::move(onClicked))
{
}

void SongTreeItem::paintItem(juce::Graphics& g, int width, int height)
{
    const Palette& palette = paletteFor(*this);

    const juce::Colour textCol = palette.textPrimary;

    // Icon + label layout
    const int iconSize = 12;
    const int iconX = 2;
    const int iconY = (height - iconSize) / 2;
    const int textX = iconX + iconSize + 6;
    const int textW = width - textX - 4;

    // Draw file-type icon
    if (node_.fileType == FileType::SongHathor)
    {
        // .hathor: a small rectangle with an accent dot — pattern icon
        g.setColour(textCol.withAlpha(0.7f));
        g.fillRect(iconX, iconY, iconSize, iconSize);
        g.setColour(palette.accent);
        const int dotSize = 4;
        g.fillEllipse(
            static_cast<float>(iconX + (iconSize - dotSize) / 2),
            static_cast<float>(iconY + (iconSize - dotSize) / 2),
            static_cast<float>(dotSize),
            static_cast<float>(dotSize));
    }
    else if (node_.fileType == FileType::SongChuck)
    {
        // .ck: a small rectangle with brackets — ChucK icon
        g.setColour(textCol.withAlpha(0.7f));
        g.fillRect(iconX, iconY, iconSize, iconSize);
        g.setColour(textCol);
        g.drawText("[ ]", iconX, iconY, iconSize, iconSize,
                   juce::Justification::centred, false);
    }
    else
    {
        // Fallback for any other song type
        g.setColour(textCol.withAlpha(0.7f));
        g.fillRect(iconX, iconY, iconSize, iconSize);
    }

    // Draw filename (stem + extension so user sees ".hathor")
    const juce::File f(juce::String(node_.path.string()));
    const juce::String fileName = f.getFileName();
    g.setColour(textCol);
    g.setFont(HathorLookAndFeel::fontRegular(13.0f));
    g.drawText(fileName, textX, 0, textW, height,
               juce::Justification::centredLeft, true);
}

void SongTreeItem::itemOpennessChanged(bool /*isOpen*/)
{
    // Songs are leaves — nothing to do.
}

void SongTreeItem::itemClicked(const juce::MouseEvent& /*e*/)
{
    if (onSongClicked_)
        onSongClicked_(file());
}

void SongTreeItem::itemDoubleClicked(const juce::MouseEvent& /*e*/)
{
    if (onSongClicked_)
        onSongClicked_(file());
}

// ===========================================================================
// FolderTreeItem
// ===========================================================================

FolderTreeItem::FolderTreeItem(FolderNode node, SongClickedCallback onClicked)
    : node_(std::move(node)),
      onSongClicked_(std::move(onClicked))
{
    // Root is expanded by default; child folders inherit their node's
    // expanded flag (false for children).
    if (node_.expanded)
        setOpen(true);
}

void FolderTreeItem::paintItem(juce::Graphics& g, int width, int height)
{
    const Palette& palette = paletteFor(*this);

    const juce::Colour textCol = palette.textPrimary;
    const juce::Colour mutedCol = palette.textSecondary;

    // Icon + label layout
    const int iconSize = 12;
    const int iconX = 2;
    const int iconY = (height - iconSize) / 2;
    const int textX = iconX + iconSize + 6;
    const int textW = width - textX - 4;

    // Draw folder icon: a small folder shape (rectangle + tab)
    g.setColour(textCol.withAlpha(0.7f));
    const int tabW = 8;
    const int tabH = 4;
    g.fillRect(iconX, iconY, tabW, tabH);       // folder tab
    g.fillRect(iconX, iconY + tabH, iconSize, iconSize - tabH); // folder body

    // Draw folder name
    g.setColour(textCol);
    g.setFont(HathorLookAndFeel::fontRegular(13.0f));
    g.drawText(juce::String(node_.name), textX, 0, textW, height,
               juce::Justification::centredLeft, true);

    // Show child count as muted suffix
    const int childCount = static_cast<int>(node_.folders.size() + node_.songs.size());
    if (childCount > 0)
    {
        const juce::String suffix = "(" + juce::String(childCount) + ")";
        g.setColour(mutedCol);
        g.setFont(HathorLookAndFeel::fontRegular(11.0f));
        g.drawText(suffix, textX, 0, textW, height,
                   juce::Justification::centredRight, true);
    }
}

void FolderTreeItem::itemOpennessChanged(bool isOpen)
{
    // Lazily build children the first time the folder is expanded.
    if (isOpen && !childrenBuilt_)
    {
        clearSubItems();

        // Add child folders first.
        for (const auto& childFolder : node_.folders)
        {
            auto childItem = std::make_unique<FolderTreeItem>(childFolder, onSongClicked_);
            addSubItem(childItem.release());
        }

        // Add song leaves.
        for (const auto& song : node_.songs)
        {
            auto songItem = std::make_unique<SongTreeItem>(song, onSongClicked_);
            addSubItem(songItem.release());
        }

        childrenBuilt_ = true;
    }
}

bool FolderTreeItem::mightContainSubItems()
{
    return !node_.folders.empty() || !node_.songs.empty();
}

void FolderTreeItem::itemClicked(const juce::MouseEvent& /*e*/)
{
    // Folders are not clickable for opening — only expandable.
}

void FolderTreeItem::itemDoubleClicked(const juce::MouseEvent& /*e*/)
{
    // Double-click a folder toggles its openness.
    setOpen(!isOpen());
}

} // namespace hathor::ui
