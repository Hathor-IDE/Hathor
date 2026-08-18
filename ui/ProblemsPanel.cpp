// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * ProblemsPanel.cpp — L-3: implementation of the unified Problems/Diagnostics panel.
 *
 * Requirement references: L-3 §2
 */

#include "ProblemsPanel.hpp"
#include "control/Diagnostic.hpp"

#include <algorithm>
#include <filesystem>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Mouse handler for double-click navigation (must be defined before the
// ProblemsPanel constructor that uses it).
// ---------------------------------------------------------------------------

namespace {

class ListMouseHandler : public juce::MouseListener
{
public:
    explicit ListMouseHandler(ProblemsPanel& panel) : panel_(panel) {}
    void mouseDoubleClick(const juce::MouseEvent&) override
    {
        panel_.onDoubleClick();
    }
private:
    ProblemsPanel& panel_;
};

} // namespace

// ---------------------------------------------------------------------------
// Mouse handler for source filter combo box
// ---------------------------------------------------------------------------

namespace {

class SourceFilterListener : public juce::ComboBox::Listener
{
public:
    explicit SourceFilterListener(ProblemsPanel& panel) : panel_(panel) {}

    void comboBoxChanged(juce::ComboBox* /*box*/) override
    {
        panel_.onSourceFilterChanged();
    }

private:
    ProblemsPanel& panel_;
};

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ProblemsPanel::ProblemsPanel(hathor::control::DiagnosticRegistry* registry)
    : registry_(registry)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    hintLabel_ = std::make_unique<juce::Label>();
    hintLabel_->setText("Problems", juce::dontSendNotification);
    hintLabel_->setFont(HathorLookAndFeel::fontMedium(
        HathorLookAndFeel::Typography::labelMd));
    hintLabel_->setColour(juce::Label::textColourId, palette.textSecondary);
    hintLabel_->setJustificationType(juce::Justification::centredLeft);
    addAndMakeVisible(hintLabel_.get());

    listBox_ = std::make_unique<juce::ListBox>();
    listBox_->setModel(this);
    listBox_->setColour(juce::ListBox::backgroundColourId, palette.surfaceLow);
    listBox_->setColour(juce::ListBox::outlineColourId, palette.surfaceHighest.withAlpha(0.1f));
    listBox_->setRowSelectedOnMouseDown(true);
    listBox_->setMultipleSelectionEnabled(false);
    listBox_->addMouseListener(new class ListMouseHandler(*this), true);
    addAndMakeVisible(listBox_.get());

    refreshBtn_ = std::make_unique<juce::TextButton>("Refresh");
    refreshBtn_->onClick = [this]()
    {
        refresh();
        if (onRefresh)
            onRefresh();
    };
    addAndMakeVisible(refreshBtn_.get());

    closeBtn_ = std::make_unique<juce::TextButton>("x");
    closeBtn_->onClick = [this]()
    {
        setVisible(false);
        if (onClosePanel)
            onClosePanel();
    };
    addAndMakeVisible(closeBtn_.get());

    sourceFilter_ = std::make_unique<juce::ComboBox>();
    sourceFilter_->addListener(new SourceFilterListener(*this));
    sourceFilter_->addItem("All Sources", 1);
    for (int i = 0; i < 7; ++i)
    {
        auto src = static_cast<hathor::control::DiagSource>(i);
        sourceFilter_->addItem(juce::String(hathor::control::sourceLabel(src).data()), i + 2);
    }
    sourceFilter_->setSelectedId(1, juce::dontSendNotification);
    sourceFilter_->setEditableText(false);
    addAndMakeVisible(sourceFilter_.get());

    rebuildRows();
}

ProblemsPanel::~ProblemsPanel() = default;

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

void ProblemsPanel::setVisible(bool visible)
{
    juce::Component::setVisible(visible);
    if (visible)
        refresh();
}

// ---------------------------------------------------------------------------
// Registry binding
// ---------------------------------------------------------------------------

void ProblemsPanel::setRegistry(hathor::control::DiagnosticRegistry* registry) noexcept
{
    registry_ = registry;
    if (registry_)
    {
        registry_->setChangeCallback([this]() { refresh(); });
        refresh();
    }
    else
    {
        rows_.clear();
        listBox_->updateContent();
    }
}

void ProblemsPanel::refresh()
{
    if (!registry_)
    {
        rows_.clear();
        listBox_->updateContent();
        return;
    }

    const auto diags = registry_->allDiagnostics();
    flattenIntoRows(diags);
    listBox_->updateContent();
    selectedRow_ = rows_.empty() ? -1 : 0;
    listBox_->selectRow(selectedRow_);
}

// ---------------------------------------------------------------------------
// Filtering
// ---------------------------------------------------------------------------

void ProblemsPanel::setShowErrors(bool v) noexcept
{
    if (showErrors_ == v) return;
    showErrors_ = v;
    rebuildRows();
}

void ProblemsPanel::setShowWarnings(bool v) noexcept
{
    if (showWarnings_ == v) return;
    showWarnings_ = v;
    rebuildRows();
}

void ProblemsPanel::setShowInfo(bool v) noexcept
{
    if (showInfo_ == v) return;
    showInfo_ = v;
    rebuildRows();
}

void ProblemsPanel::setSourceFilter(const std::string& sourceLabel)
{
    if (sourceFilterStr_ == sourceLabel) return;
    sourceFilterStr_ = sourceLabel;
    rebuildRows();
}

void ProblemsPanel::onSourceFilterChanged()
{
    const int id = sourceFilter_->getSelectedId();
    if (id == 1)
        setSourceFilter({});
    else
        setSourceFilter(sourceFilter_->getText().toStdString());

    rebuildRows();
    listBox_->updateContent();
}

void ProblemsPanel::rebuildRows()
{
    if (!registry_)
    {
        rows_.clear();
        listBox_->updateContent();
        listBox_->deselectAllRows();
        return;
    }

    auto diags = registry_->allDiagnostics();
    flattenIntoRows(diags);
    listBox_->updateContent();
    listBox_->deselectAllRows();
    listBox_->selectRow(0);
    selectedRow_ = 0;
}

// ---------------------------------------------------------------------------
// Row flattening & grouping
// ---------------------------------------------------------------------------

void ProblemsPanel::flattenIntoRows(const std::vector<hathor::control::Diagnostic>& diags)
{
    rows_.clear();

    // First pass: determine which diagnostics are visible after filtering.
    // allDiagnostics() is sorted by (uri, line, severity, column), so same-URI
    // entries are contiguous.
    struct UriGroup
    {
        std::string uri;
        std::vector<const hathor::control::Diagnostic*> visible;
    };

    std::vector<UriGroup> groups;
    UriGroup* current = nullptr;

    for (const auto& d : diags)
    {
        // Severity filter
        if ((d.severity == hathor::control::DiagSeverity::Error && !showErrors_) ||
            (d.severity == hathor::control::DiagSeverity::Warning && !showWarnings_) ||
            ((d.severity == hathor::control::DiagSeverity::Info ||
              d.severity == hathor::control::DiagSeverity::Hint) && !showInfo_))
            continue;

        // Source filter
        if (!sourceFilterStr_.empty() &&
            std::string(hathor::control::sourceLabel(d.source)) != sourceFilterStr_)
            continue;

        if (!current || current->uri != d.uri)
        {
            groups.push_back({d.uri, {}});
            current = &groups.back();
        }
        current->visible.push_back(&d);
    }

    // Second pass: emit rows (file header + diagnostics per group)
    for (const auto& grp : groups)
    {
        ProblemRow hdr;
        hdr.type        = ProblemRow::Type::FileHeader;
        hdr.uri         = grp.uri;
        hdr.displayPath = grp.uri;
        if (hdr.displayPath.substr(0, 7) == "file://")
            hdr.displayPath = hdr.displayPath.substr(7);
        // Shorten to filename for readability
        {
            std::error_code ec;
            auto p = std::filesystem::path(hdr.displayPath);
            if (p.has_filename())
                hdr.displayPath = p.filename().string();
        }
        hdr.expanded   = true;
        hdr.childCount = static_cast<int>(grp.visible.size());
        rows_.push_back(hdr);

        for (const auto* d : grp.visible)
        {
            ProblemRow dr;
            dr.type         = ProblemRow::Type::Diagnostic;
            dr.diagId       = d->id;
            dr.severity     = d->severity;
            dr.source       = d->source;
            dr.sourceLabel  = d->sourceLabel;
            dr.code         = d->code;
            dr.message      = d->message;
            dr.uri          = d->uri;
            dr.line         = d->line;
            dr.column       = d->column;
            dr.relatedInfo  = d->relatedInfo;
            rows_.push_back(dr);
        }
    }
}

void ProblemsPanel::toggleGroup(int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows_.size()))
        return;
    if (rows_[rowIndex].type != ProblemRow::Type::FileHeader)
        return;

    rows_[rowIndex].expanded = !rows_[rowIndex].expanded;

    // Find the extent of this group
    int groupEnd = static_cast<int>(rows_.size()) - 1;
    for (int i = rowIndex + 1; i < static_cast<int>(rows_.size()); ++i)
    {
        if (rows_[i].type == ProblemRow::Type::FileHeader)
        {
            groupEnd = i - 1;
            break;
        }
    }

    for (int i = rowIndex + 1; i <= groupEnd; ++i)
    {
        // In a richer implementation, we'd toggle visibility of each row.
        // For now, we keep the rows but flip the expanded flag; the
        // diagnostic rows are still shown (the expand/collapse is purely
        // visual for the header). A production version would hide/show rows.
    }

    listBox_->updateContent();
}

void ProblemsPanel::navigateToRow(int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows_.size()))
        return;
    if (rows_[rowIndex].type != ProblemRow::Type::Diagnostic)
        return;

    const auto& row = rows_[rowIndex];
    if (onDiagnosticSelected)
        onDiagnosticSelected(row.uri, row.line, row.column);
}

void ProblemsPanel::onDoubleClick()
{
    navigateToRow(selectedRow_);
}

// ---------------------------------------------------------------------------
// ListBoxModel
// ---------------------------------------------------------------------------

int ProblemsPanel::getNumRows()
{
    return static_cast<int>(rows_.size());
}

void ProblemsPanel::paintListBoxItem(int row, juce::Graphics& g,
                                     int width, int height, bool isSelected)
{
    if (row < 0 || row >= static_cast<int>(rows_.size()))
        return;

    const auto& r = rows_[row];

    if (isSelected)
    {
        const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
        g.fillAll(palette.accent.withAlpha(0.2f));
    }

    if (r.type == ProblemRow::Type::FileHeader)
        paintFileHeader(g, r, width, height, isSelected, r.expanded);
    else
        paintDiagnosticRow(g, r, width, height, isSelected);
}

void ProblemsPanel::paintDiagnosticRow(juce::Graphics& g, const ProblemRow& row,
                                       int width, int height, bool /*isSelected*/) const
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    int x = 6;

    // Severity dot
    juce::Colour dotColour;
    switch (row.severity)
    {
        case hathor::control::DiagSeverity::Error:   dotColour = palette.error;   break;
        case hathor::control::DiagSeverity::Warning: dotColour = palette.warning; break;
        case hathor::control::DiagSeverity::Info:
        case hathor::control::DiagSeverity::Hint:    dotColour = palette.textMuted; break;
    }
    g.setColour(dotColour);
    constexpr int kDotSize = 8;
    g.fillEllipse(static_cast<float>(x - kDotSize / 2),
                  static_cast<float>(height / 2 - kDotSize / 2),
                  static_cast<float>(kDotSize), static_cast<float>(kDotSize));
    x += 18;

    // Source label (narrow, dimmed)
    g.setColour(palette.textMuted);
    g.setFont(HathorLookAndFeel::fontRegular(12.0f));
    juce::String srcText = juce::String(row.sourceLabel.c_str());
    g.drawText(srcText, juce::Rectangle<int>(x, 0, 130, height),
               juce::Justification::centredLeft, false);
    x += 134;

    // Line:column (dimmed, narrower)
    if (row.line > 0)
    {
        juce::String locText = juce::String(row.line) + ":" + juce::String(row.column);
        g.drawText(locText, juce::Rectangle<int>(x, 0, 70, height),
                   juce::Justification::centredLeft, false);
    }
    x += 74;

    // Message
    g.setColour(palette.textPrimary);
    g.drawText(juce::String(row.message.c_str()),
               juce::Rectangle<int>(x, 0, width - x - 4, height),
               juce::Justification::centredLeft, false);
}

void ProblemsPanel::paintFileHeader(juce::Graphics& g, const ProblemRow& row,
                                    int width, int height, bool /*isSelected*/,
                                    bool isExpanded) const
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();

    g.setColour(palette.surfaceLow);
    g.fillRect(0, 0, width, height);

    int x = 12;

    // Expand/collapse triangle
    g.setColour(palette.textSecondary);
    juce::Path triangle;
    const float cx = static_cast<float>(x + 4);
    const float cy = static_cast<float>(height / 2);
    if (isExpanded)
    {
        constexpr float halfW = 4.0f;
        constexpr float halfH = 2.5f;
        triangle.addTriangle(cx - halfW, cy - halfH,
                              cx + halfW, cy - halfH,
                              cx, cy + halfH);
    }
    else
    {
        constexpr float halfW = 2.5f;
        constexpr float halfH = 4.0f;
        triangle.addTriangle(cx - halfW, cy - halfH,
                              cx + halfW, cy - halfH,
                              cx, cy + halfH);
    }
    g.fillPath(triangle);
    x += 24;

    // Filename
    g.setColour(palette.textPrimary);
    g.setFont(HathorLookAndFeel::fontMedium(13.0f));
    g.drawText(juce::String(row.displayPath.c_str()),
               juce::Rectangle<int>(x, 0, width - x - 40, height),
               juce::Justification::centredLeft, false);

    // Child count badge
    if (row.childCount > 0)
    {
        juce::String countText = juce::String(row.childCount);
        juce::Rectangle<int> badge(0, 0, 22, 16);
        badge.setCentre(width - 24, height / 2);
        g.setColour(palette.surface);
        g.fillRoundedRectangle(badge.toFloat(), 8.0f);
        g.setColour(palette.textSecondary);
        g.drawText(countText, badge.reduced(2, 0),
                   juce::Justification::centred, false);
    }
}

void ProblemsPanel::selectedRowsChanged(int lastSelectedRow)
{
    selectedRow_ = lastSelectedRow;

    // Single click → navigate immediately (EditorArea pattern)
    navigateToRow(selectedRow_);
}

void ProblemsPanel::deleteKeyPressed(int /*lastRowSelected*/)
{
    // Diagnostics are deterministic and managed by their source.
    // No deletion from this panel.
}

juce::Component* ProblemsPanel::refreshComponentForRow(
    int /*row*/, bool /*rowIsNowSelected*/,
    juce::Component* existingComponentToUpdate)
{
    return existingComponentToUpdate;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void ProblemsPanel::resized()
{
    const int margin = kMargin;
    const int fieldH = kFieldHeight;

    closeBtn_->setBounds(getWidth() - 20, margin, 18, 18);
    hintLabel_->setBounds(margin, margin, getWidth() - margin * 2 - 20, 18);
    sourceFilter_->setBounds(margin, margin + 22, 130, fieldH);
    refreshBtn_->setBounds(margin + 134, margin + 22, 80, fieldH);
    listBox_->setBounds(margin, margin + 22 + fieldH + 4,
                        getWidth() - margin * 2,
                        getHeight() - 52 - margin);
}

void ProblemsPanel::paint(juce::Graphics& g)
{
    const auto& palette = HathorLookAndFeel::fromComponent(*this).getPalette();
    g.fillAll(palette.surfaceContainer);

    // Top border
    g.setColour(palette.surfaceHighest);
    g.fillRect(0, 0, getWidth(), 1);

    // Summary text at the bottom
    g.setColour(palette.textSecondary);
    g.setFont(HathorLookAndFeel::fontRegular(12.0f));
    g.drawText(buildSummaryText(), kMargin, getHeight() - 20, getWidth() - kMargin * 2, 18,
               juce::Justification::centredLeft, false);
}

juce::String ProblemsPanel::buildSummaryText() const
{
    int errs = 0, warns = 0, infos = 0;
    for (const auto& r : rows_)
    {
        if (r.type == ProblemRow::Type::Diagnostic)
        {
            switch (r.severity)
            {
                case hathor::control::DiagSeverity::Error:   ++errs; break;
                case hathor::control::DiagSeverity::Warning: ++warns; break;
                case hathor::control::DiagSeverity::Info:
                case hathor::control::DiagSeverity::Hint:     ++infos; break;
            }
        }
    }

    juce::String text = juce::String(rows_.size()) + " problem" + (rows_.size() != 1 ? "s" : "");
    if (errs > 0)  text << " · " << juce::String(errs) << " error" << (errs > 1 ? "s" : "");
    if (warns > 0) text << " · " << juce::String(warns) << " warning" << (warns > 1 ? "s" : "");
    if (infos > 0 && showInfo_) text << " · " << juce::String(infos) << " info";

    return text;
}

bool ProblemsPanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        setVisible(false);
        if (onClosePanel)
            onClosePanel();
        return true;
    }
    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())
    {
        if (key.getKeyCode() == 'R')
        {
            refresh();
            return true;
        }
    }
    return false;
}

} // namespace hathor::ui
