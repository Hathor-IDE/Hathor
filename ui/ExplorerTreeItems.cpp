// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ExplorerTreeItems.cpp — JUCE TreeViewItem implementations.
 */

#include "ExplorerTreeItems.hpp"

#include <juce_gui_extra/juce_gui_extra.h>

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
// AssetTreeItem — B8-K5: managed logical ChucK instrument
// ===========================================================================

AssetTreeItem::AssetTreeItem(AssetNode node, SongClickedCallback onOpenSource)
    : node_(std::move(node)),
      onSourceClicked_(std::move(onOpenSource))
{
}

void AssetTreeItem::paintItem(juce::Graphics& g, int width, int height)
{
    const Palette& palette = paletteFor(*this);

    const juce::Colour textCol     = palette.textPrimary;
    const juce::Colour mutedCol    = palette.textSecondary;

    // Icon + label layout
    const int iconSize = 12;
    const int iconX = 2;
    const int iconY = (height - iconSize) / 2;
    const int textX = iconX + iconSize + 6;
    const int textW = width - textX - 12;

    // Icon: an instrument glyph — a small rectangle with a wave/note symbol
    // to distinguish from ordinary .ck song files.
    g.setColour(textCol.withAlpha(0.7f));
    g.fillRect(iconX, iconY, iconSize, iconSize);
    g.setColour(palette.accent);
    // Draw a small "sound wave" — two vertical bars indicating an audio asset.
    const int barW = 2;
    const int gap = 1;
    g.fillRect(iconX + 2, iconY + 2, barW, iconSize - 4);
    g.fillRect(iconX + 2 + barW + gap, iconY + 4, barW, iconSize - 6);
    g.fillRect(iconX + 2 + (barW + gap) * 2, iconY + 3, barW, iconSize - 5);

    // Draw the logical instrument name (stem, not ".ck" / ".wav")
    g.setColour(textCol);
    g.setFont(HathorLookAndFeel::fontRegular(13.0f));
    g.drawText(juce::String(node_.name), textX, 0, textW, height,
               juce::Justification::centredLeft, true);

    // Status badge: show whether the instrument is baked
    if (node_.hasBakedAudio())
    {
        // "baked" dot — green, like a "ready" indicator
        const juce::Colour badgeCol = palette.accent;
        const int dotSize = 6;
        const int dotX = width - dotSize - 4;
        const int dotY = (height - dotSize) / 2;
        g.setColour(badgeCol);
        g.fillEllipse(static_cast<float>(dotX),
                      static_cast<float>(dotY),
                      static_cast<float>(dotSize),
                      static_cast<float>(dotSize));
    }
    else if (node_.hasSource())
    {
        // "needs bake" indicator — amber dot, indicating source exists but
        // no .wav yet.
        g.setColour(palette.warning);
        const int dotSize = 6;
        const int dotX = width - dotSize - 4;
        const int dotY = (height - dotSize) / 2;
        g.fillEllipse(static_cast<float>(dotX),
                      static_cast<float>(dotY),
                      static_cast<float>(dotSize),
                      static_cast<float>(dotSize));
    }

    // Show source indicator (muted suffix)
    const juce::String suffix = node_.hasBakedAudio()
        ? "  baked"
        : (node_.hasSource() ? "  source-only" : "  audio-only");
    if (!suffix.isEmpty())
    {
        g.setColour(mutedCol);
        g.setFont(HathorLookAndFeel::fontRegular(11.0f));
        g.drawText(suffix, textX, 0, textW, height,
                   juce::Justification::centredRight, true);
    }
}

void AssetTreeItem::itemOpennessChanged(bool /*isOpen*/)
{
    // Assets are leaves — nothing to do.
}

void AssetTreeItem::itemClicked(const juce::MouseEvent& /*e*/)
{
    // Clicking an instrument opens its .ck source for editing (B8-K5 §4).
    // If no source exists, the .wav is still associated but there's
    // nothing to open — the click is a no-op in that case.
    if (onSourceClicked_)
        if (const juce::File f = sourceFile(); f.getFullPathName().isNotEmpty())
            onSourceClicked_(f);
}

void AssetTreeItem::itemDoubleClicked(const juce::MouseEvent& /*e*/)
{
    // Double-click: same as single-click — open .ck source.
    if (onSourceClicked_)
        if (const juce::File f = sourceFile(); f.getFullPathName().isNotEmpty())
            onSourceClicked_(f);
}

juce::File AssetTreeItem::sourceFile() const noexcept
{
    if (node_.hasSource() && node_.ckSource)
        return juce::File(juce::String(node_.ckSource->string()));
    return {};
}

juce::File AssetTreeItem::audioFile() const noexcept
{
    if (node_.hasBakedAudio() && node_.wavAsset)
        return juce::File(juce::String(node_.wavAsset->string()));
    return {};
}

// ===========================================================================
// FolderTreeItem
// ===========================================================================

FolderTreeItem::FolderTreeItem(FolderNode node, SongClickedCallback onClicked,
                               SongClickedCallback onSourceClicked)
    : node_(std::move(node)),
      onSongClicked_(std::move(onClicked)),
      onSourceClicked_(std::move(onSourceClicked))
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

    // Show child count as muted suffix — include managed assets in the count.
    const int childCount = static_cast<int>(node_.folders.size()
                                          + node_.songs.size()
                                          + node_.managedCategories.size()
                                          + node_.managedAssets.size());
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
            auto childItem = std::make_unique<FolderTreeItem>(childFolder, onSongClicked_, onSourceClicked_);
            addSubItem(childItem.release());
        }

        // Add managed category folders (B8-K5).  These are synthesized from
        // .hathor_assets and contain logical AssetTreeItem children.
        for (const auto& managedCat : node_.managedCategories)
        {
            auto catItem = std::make_unique<FolderTreeItem>(managedCat, onSongClicked_, onSourceClicked_);
            addSubItem(catItem.release());
        }

        // Add song leaves.
        for (const auto& song : node_.songs)
        {
            auto songItem = std::make_unique<SongTreeItem>(song, onSongClicked_);
            addSubItem(songItem.release());
        }

        // Add managed logical asset leaves (B8-K5).  These are direct
        // children of the root folder (synthesized from .hathor_assets).
        for (const auto& asset : node_.managedAssets)
        {
            auto assetItem = std::make_unique<AssetTreeItem>(asset, onSourceClicked_);
            addSubItem(assetItem.release());
        }

        childrenBuilt_ = true;
    }
}

bool FolderTreeItem::mightContainSubItems()
{
    return !node_.folders.empty()
        || !node_.songs.empty()
        || !node_.managedCategories.empty()
        || !node_.managedAssets.empty();
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
