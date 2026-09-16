# Hathor Keyboard Shortcuts

Generated from the `ActionRegistry` (Wave 4.3). Source of truth is the
`registerAction` / `bindKey` calls in `ui/EditorArea.cpp`.

## File

| Keys | Action |
|------|--------|
| Cmd+S | Save |
| Cmd+Shift+S | Save As… |
| Cmd+N | New Tab |
| Cmd+W | Close Tab |

## Editor

| Keys | Action |
|------|--------|
| Cmd+Enter (or Ctrl+Enter in editor) | Evaluate Block |
| Cmd+F | Find… |
| Cmd+Option+F | Replace… |
| Cmd+\ | Toggle Split |
| Cmd+Shift+P | Command Palette… |
| Cmd+Shift+T | Reopen Closed Tab |

## Go / Navigation

| Keys | Action |
|------|--------|
| Cmd+P | Quick Open… |
| Cmd+T | Go to Symbol… |
| Cmd+Shift+F | Search in Files… |
| F12 | Go to Definition |
| Shift+F12 | Find References |
| F8 / Shift+F8 | Next / Previous Error |
| Cmd+Option+Left / Right | Go Back / Forward |

## Terminal / View

| Keys | Action |
|------|--------|
| Cmd+Shift+` | Toggle Terminal |

Typing in editors is unaffected: global shortcuts only fire when the key
press maps to a registered `KeyEquivalent` (digits, punctuation and
Ctrl/Cmd+Shift chords included since Wave 4.3).
