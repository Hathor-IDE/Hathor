// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * EditorContextBridge.cpp — JUCE-dependent implementation of
 * EditorContextProvider.
 *
 * Requirement references: AI-8 §2, §4
 */

#include "EditorContextBridge.hpp"

#include "EditorArea.hpp"
#include "HathorTab.hpp"
#include "HathorFileParser.hpp"

#include <chrono>
#include <ctime>
#include <cstdio>

namespace hathor::ui {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace {

std::string isoTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

/// Build a file:// URI or synthetic URI from a file path.
std::string uriFromFile(const std::string& path)
{
    if (path.empty())
        return {};
    return "file://" + path;
}

/// Build a synthetic URI for an untitled buffer.
std::string uriForSlot(const std::string& slotName)
{
    if (slotName.empty())
        return "untitled://untitled";
    return "slot://" + slotName;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

EditorContextBridge::EditorContextBridge(EditorArea& editorArea)
    : editorArea_(editorArea)
{
    refresh();
}

// ---------------------------------------------------------------------------
// refresh — called from the JUCE message thread
// ---------------------------------------------------------------------------

void EditorContextBridge::refresh()
{
    hathor::control::EditorContextSnapshot snap;
    snap.capturedAt = isoTimestamp();

    HathorTab* tab = editorArea_.activeTab();
    if (tab == nullptr)
    {
        snap.hasContent = false;
        std::lock_guard<std::mutex> lock(snapshotMtx_);
        snapshot_ = std::move(snap);
        return;
    }

    snap.hasContent = true;

    // File path
    const auto& filePath = tab->filePath();
    if (filePath.has_value() && !filePath->isFullScreen())
    {
        const std::string path = filePath->getFullPathName().toStdString();
        snap.file = path;
        snap.uri = uriFromFile(path);
    }
    else
    {
        // Untitled — synthetic URI
        snap.file = "";
        snap.slotName = tab->slotName().toStdString();
        snap.uri = uriForSlot(snap.slotName);
    }

    // Language
    snap.language = tab->isChuckTab() ? "chuck" : "mininotation";

    // Document content — from the CodeDocument
    snap.content = tab->document().getAllContent().toStdString();

    // Cursor position
    const auto& editor = tab->editor();
    const auto cursorPos = editor.getCaretPosition();
    snap.cursorLine = cursorPos.getLineNumber();
    snap.cursorChar = cursorPos.getIndexInLine();

    // Selection
    const juce::TextEditor::TextSelection sel = editor.getTextSelection();
    snap.hasSelection = sel.isSet;
    if (sel.isSet)
    {
        snap.selStartLine  = sel.start.getPosition().getLine();
        snap.selStartChar  = sel.start.getPosition().getIndexInLine();
        snap.selEndLine    = sel.end.getPosition().getLine();
        snap.selEndChar    = sel.end.getPosition().getIndexInLine();
        snap.selectedText   = sel.ToString().toStdString();
    }

    // Slot info
    snap.slotIndex = tab->slotIndex();
    if (snap.slotName.empty())
        snap.slotName = tab->slotName().toStdString();

    // Front-matter
    if (const auto* fm = tab->frontMatter())
    {
        if (fm->slot)    snap.frontMatterSlot  = *fm->slot;
        if (fm->bpm)     snap.frontMatterBpm   = *fm->bpm;
        if (fm->bank)    snap.frontMatterBank  = *fm->bank;
    }

    std::lock_guard<std::mutex> lock(snapshotMtx_);
    snapshot_ = std::move(snap);
}

void EditorContextBridge::refreshFromTab(HathorTab* tab)
{
    if (tab == nullptr)
    {
        refresh();
        return;
    }

    hathor::control::EditorContextSnapshot snap;
    snap.capturedAt = isoTimestamp();
    snap.hasContent = true;

    const auto& filePath = tab->filePath();
    if (filePath.has_value() && !filePath->isFullScreen())
    {
        const std::string path = filePath->getFullPathName().toStdString();
        snap.file = path;
        snap.uri = uriFromFile(path);
    }
    else
    {
        snap.file = "";
        snap.slotName = tab->slotName().toStdString();
        snap.uri = uriForSlot(snap.slotName);
    }

    snap.language = tab->isChuckTab() ? "chuck" : "mininotation";
    snap.content = tab->document().getAllContent().toStdString();

    const auto& editor = tab->editor();
    const auto cursorPos = editor.getCaretPosition();
    snap.cursorLine = cursorPos.getLineNumber();
    snap.cursorChar = cursorPos.getIndexInLine();

    snap.slotIndex = tab->slotIndex();
    if (snap.slotName.empty())
        snap.slotName = tab->slotName().toStdString();

    if (const auto* fm = tab->frontMatter())
    {
        if (fm->slot)    snap.frontMatterSlot  = *fm->slot;
        if (fm->bpm)     snap.frontMatterBpm   = *fm->bpm;
        if (fm->bank)    snap.frontMatterBank  = *fm->bank;
    }

    std::lock_guard<std::mutex> lock(snapshotMtx_);
    snapshot_ = std::move(snap);
}

// ---------------------------------------------------------------------------
// snapshot — thread-safe read
// ---------------------------------------------------------------------------

hathor::control::EditorContextSnapshot EditorContextBridge::snapshot() const
{
    std::lock_guard<std::mutex> lock(snapshotMtx_);
    return snapshot_;
}

} // namespace hathor::ui
