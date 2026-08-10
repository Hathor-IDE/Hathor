// Copyright (C) 2024 Hathor Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/**
 * EditorContextProvider.hpp — JUCE-free abstract interface for capturing
 * the current editor state (file, cursor, selection) as a thread-safe
 * snapshot.
 *
 * This interface is the boundary between the JUCE UI layer (which owns the
 * CodeDocument / CodeEditorComponent instances) and the control layer
 * (which assembles authoring context for AI-8).
 *
 * The provider implementation in ui/ maintains a mutex-protected snapshot
 * that is updated on the JUCE message thread whenever the editor state
 * changes (cursor movement, tab switch, document edit, file open).  The
 * snapshot() method is safe to call from any thread (including the MCP
 * accept-loop worker thread).
 *
 * Requirement references: AI-8 §2, §3, §4
 */

#include <string>
#include <string_view>

namespace hathor::control {

// ---------------------------------------------------------------------------
// EditorContextSnapshot — plain-data snapshot of the active editor state.
// ---------------------------------------------------------------------------

/**
 * A point-in-time snapshot of the currently active editor.
 *
 * All strings are UTF-8 (matching the document encoding).  Positions are
 * 0-based byte offsets into the document content, matching the encoding
 * used by parseMini() and the mini-notation tokeniser.
 *
 * When no editor tab is active, all fields are empty/zero (hasContent == false).
 */
struct EditorContextSnapshot {
    /// Human-readable ISO-8601 UTC timestamp captured when the snapshot was taken.
    std::string capturedAt;

    /// True if there is an active editor tab with content.
    bool hasContent = false;

    // --- File identity ---
    /// Absolute file path, or empty for untitled buffers.
    std::string file;
    /// file:// URI, or a synthetic URI like "slot://d0" for untitled buffers.
    std::string uri;
    /// "mininotation" for .hathor files, "chuck" for .ck files,
    /// "settings" for the Settings tab, or "unknown".
    std::string language;

    // --- Document content ---
    /// Full text content of the editor's active buffer.
    std::string content;

    // --- Cursor position ---
    /// 0-based line number of the primary cursor.
    int cursorLine = 0;
    /// 0-based character column of the primary cursor.
    int cursorChar = 0;

    // --- Selection ---
    /// True if a non-empty text selection is active.
    bool hasSelection = false;
    /// 0-based line of the selection anchor.
    int selStartLine = 0;
    /// 0-based character column of the selection anchor.
    int selStartChar = 0;
    /// 0-based line of the selection focus.
    int selEndLine = 0;
    /// 0-based character column of the selection focus.
    int selEndChar = 0;
    /// The selected text (empty if no selection).
    std::string selectedText;

    // --- Slot / pattern context ---
    /// The pattern slot name assigned to this tab (e.g. "d0"), or empty.
    std::string slotName;
    /// 0-based slot index [-1, 15] (-1 if no slot assigned).
    int slotIndex = -1;

    // --- Pattern context (for .hathor mini-notation tabs) ---
    /// The front-matter slot declared in the .hathor file, if any.
    std::string frontMatterSlot;
    /// The front-matter BPM, if any (0 if not set).
    double frontMatterBpm = 0.0;
    /// The front-matter sample bank, if any.
    std::string frontMatterBank;
};

/**
 * Convert a cursor (line, character) to a 0-based byte offset in the
 * document content.
 *
 * Lines are split on '\n'.  Character is a UTF-8 byte offset within the line.
 * If the cursor position is past the end of the document, the offset is
 * clamped to content.size().
 */
inline std::size_t cursorToOffset(std::string_view content, int line, int character) noexcept
{
    std::size_t offset = 0;
    int currentLine = 0;
    for (std::size_t i = 0; i < content.size(); ++i)
    {
        if (currentLine == line)
        {
            offset += static_cast<std::size_t>(character);
            break;
        }
        if (content[i] == '\n')
        {
            ++currentLine;
            offset = i + 1;
        }
    }
    if (currentLine == line)
        offset += static_cast<std::size_t>(character);
    // Clamp to content size.
    if (offset > content.size())
        offset = content.size();
    return offset;
}

// ---------------------------------------------------------------------------
// EditorContextProvider — abstract interface
// ---------------------------------------------------------------------------

/**
 * Provides a thread-safe snapshot of the current editor state.
 *
 * The concrete implementation (in ui/) holds a mutex-protected
 * EditorContextSnapshot that is refreshed on the JUCE message thread.
 * The control layer calls snapshot() from any thread (typically the
 * MCP accept-loop worker thread) to get a point-in-time view of the
 * editor for context assembly.
 *
 * All methods are const and thread-safe by contract — the implementation
 * must synchronize internally.
 */
class EditorContextProvider {
public:
    virtual ~EditorContextProvider() = default;

    /**
     * Return a copy of the current editor state snapshot.
     * Thread-safe: safe to call from any thread.
     */
    virtual EditorContextSnapshot snapshot() const = 0;
};

} // namespace hathor::control
