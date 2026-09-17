// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * BreadcrumbsBar.cpp — implementation.
 *
 * Requirement references: L-1 §5
 */

#include "BreadcrumbsBar.hpp"
#include "IconLibrary.hpp"
#include "HathorLookAndFeel.hpp"

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

    // Crumbs fill the remaining area. Deep trails collapse to "…" + the
    // last three segments so buttons never overflow.
    const size_t total = crumbs_.size();
    size_t firstVisible = 0;
    bool collapsed = false;
    if (total > 4)
    {
        firstVisible = total - 3;
        collapsed = true;
    }
    int x = area.getX() + 8;
    const int y = area.getY() + (area.getHeight() - 16) / 2;

    juce::Font font(HathorLookAndFeel::getUiFont(13.0f));
    auto placeCrumb = [&](Crumb& crumb) {
        int w = static_cast<int>(juce::GlyphArrangement::getStringWidth(font, crumb.label)) + 12;
        crumb.bounds = juce::Rectangle<int>(x, y, w, 16);
        x += w + 10; // room for the › separator
    };

    if (collapsed)
    {
        ellipsisCrumb_.label = juce::String("… (") + juce::String(total - 3) + ")";
        ellipsisCrumb_.isEllipsis = true;
        ellipsisCrumb_.file = juce::File();
        placeCrumb(ellipsisCrumb_);
    }
    else
    {
        ellipsisCrumb_.bounds = {};
        ellipsisCrumb_.isEllipsis = false;
    }

    for (size_t i = firstVisible; i < total; ++i)
        placeCrumb(crumbs_[i]);
    // Hidden crumbs get empty bounds so they never hit-test or paint.
    for (size_t i = 0; i < firstVisible; ++i)
        crumbs_[i].bounds = {};
}

void BreadcrumbsBar::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    juce::Font font(HathorLookAndFeel::getUiFont(13.0f));

    // Background
    g.fillAll(palette.surfaceLow);

    // Draw breadcrumbs: collapsed ancestors hide behind "…" when deep.
    if (!ellipsisCrumb_.bounds.isEmpty())
    {
        g.setColour(palette.textSecondary);
        g.setFont(font);
        g.drawFittedText(ellipsisCrumb_.label,
                         ellipsisCrumb_.bounds.reduced(4, 0),
                         juce::Justification::centredLeft, 1);
    }
    for (size_t i = 0; i < crumbs_.size(); ++i)
    {
        const auto& crumb = crumbs_[i];
        if (crumb.bounds.isEmpty())
            continue; // collapsed away
        const bool isLast = (i + 1 == crumbs_.size());
        if (i > 0)
        {
            g.setColour(palette.textSecondary);
            g.setFont(font);
            g.drawText("›", crumb.bounds.getX() - 8,
                       crumb.bounds.getY(), 8, crumb.bounds.getHeight(),
                       juce::Justification::centred, false);
        }

        g.setColour(crumb.label.isEmpty() ? palette.textSecondary
                    : isLast ? palette.textPrimary : palette.textSecondary);
        g.setFont(isLast ? HathorLookAndFeel::uiFontMedium(13.0f) : font);
        g.drawFittedText(crumb.label,
                         crumb.bounds.reduced(4, 0),
                         juce::Justification::centredLeft, 1);
    }

    // Editor name tag (sized to content)
    if (!editorName_.isEmpty())
    {
        const int tagW = static_cast<int>(
                             juce::GlyphArrangement::getStringWidth(font, editorName_))
                         + 16;
        juce::Rectangle<int> tagBounds(4, 2, tagW, 18);
        g.setColour(palette.accent.withAlpha(0.25f));
        g.fillRoundedRectangle(tagBounds.toFloat(), 3.0f);
        g.setColour(palette.textPrimary);
        g.setFont(font);
        g.drawFittedText(editorName_,
                         tagBounds.reduced(4, 2),
                         juce::Justification::centred, 1);
    }

    // Icon buttons (monochrome Lucide glyphs via IconLibrary, Agent 0.6)
    const juce::Colour iconCol(palette.textSecondary);
    IconLibrary::drawIcon(g, IconLibrary::Icon::Zap,
                          commandPaletteBtn_.toFloat(), iconCol);
    IconLibrary::drawIcon(g, IconLibrary::Icon::Search,
                          findBtn_.toFloat(), iconCol);
    IconLibrary::drawIcon(g, IconLibrary::Icon::Columns,
                          splitBtn_.toFloat(), iconCol);
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

    // Check crumbs: files open; directories show a sibling picker so a
    // folder click never tries to open a directory as a file.
    if (!ellipsisCrumb_.bounds.isEmpty()
        && ellipsisCrumb_.bounds.contains(e.position.toInt()))
    {
        showAncestorsPopup();
        return;
    }
    for (const auto& crumb : crumbs_)
    {
        if (crumb.bounds.isEmpty())
            continue;
        if (crumb.bounds.contains(e.position.toInt()))
        {
            if (crumb.isEllipsis)
                showAncestorsPopup();
            else if (crumb.file.isDirectory())
                showDirectoryPopup(crumb.file);
            else if (onBreadcrumbClicked && crumb.file != juce::File())
                onBreadcrumbClicked(crumb.file);
            break;
        }
    }
}

void BreadcrumbsBar::showDirectoryPopup(const juce::File& dir)
{
    juce::Array<juce::File> children = dir.findChildFiles(
        juce::File::findFilesAndDirectories, false);
    juce::PopupMenu menu;
    int id = 1;
    std::vector<juce::File> files;
    std::vector<juce::File> folders;
    for (const auto& c : children)
        (c.isDirectory() ? folders : files).push_back(c);
    auto byName = [](const juce::File& a, const juce::File& b) {
        return a.getFileName().compareIgnoreCase(b.getFileName()) < 0;
    };
    std::sort(folders.begin(), folders.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    std::vector<juce::File> ordered;
    for (auto& f : folders)
    {
        menu.addItem(id++, f.getFileName() + "  ›");
        ordered.push_back(f);
    }
    if (!folders.empty() && !files.empty())
        menu.addSeparator();
    for (auto& f : files)
    {
        menu.addItem(id++, f.getFileName());
        ordered.push_back(f);
    }
    if (ordered.empty())
        menu.addItem(-1, "(empty folder)", false);
    menu.showMenuAsync(juce::PopupMenu::Options(),
                       [this, ordered](int result) {
                           if (result <= 0 || result > static_cast<int>(ordered.size()))
                               return;
                           const juce::File chosen = ordered[static_cast<size_t>(result - 1)];
                           if (chosen.isDirectory())
                               showDirectoryPopup(chosen);
                           else if (onBreadcrumbClicked)
                               onBreadcrumbClicked(chosen);
                       });
}

void BreadcrumbsBar::showAncestorsPopup()
{
    juce::PopupMenu menu;
    std::vector<juce::File> listed;
    int id = 1;
    for (const auto& crumb : crumbs_)
    {
        if (crumb.isEllipsis || crumb.file == juce::File())
            continue;
        menu.addItem(id++, crumb.file.getFullPathName());
        listed.push_back(crumb.file);
    }
    menu.showMenuAsync(juce::PopupMenu::Options(),
                       [this, listed](int result) {
                           if (result <= 0 || result > static_cast<int>(listed.size()))
                               return;
                           const juce::File chosen = listed[static_cast<size_t>(result - 1)];
                           if (chosen.isDirectory())
                               showDirectoryPopup(chosen);
                           else if (onBreadcrumbClicked)
                               onBreadcrumbClicked(chosen);
                       });
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
