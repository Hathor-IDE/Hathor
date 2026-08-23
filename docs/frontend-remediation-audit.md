# Hathor Frontend Remediation Audit

> **Status at writing**: the backend is real and tested (engine, audio, ChucK, Git, MCP,
> control protocol). The frontend launches but is **not usable as an IDE** and **does not
> look finished**. This document inventories every gap and organizes the fix into
> **waves of parallel agents**, ordered so each wave unblocks real usage.

## Design decisions (binding for all agents)

| Decision | Ruling |
|----------|--------|
| **ACP model** | Hathor stays the **ACP client**; users connect *any* ACP-compatible CLI agent (Claude Code, Gemini CLI, Codex, Cline, Kilo, etc.) by picking it from a list or browsing to its binary. No Zed-server mode in scope. |
| **Font** | **JetBrains Mono stays** — but only where monospace belongs: code editors, terminal, logs. All other UI chrome uses a proper proportional UI font stack. The global typeface hijack must go. |
| **Icons** | Ship a real icon system (embedded SVG set rendered via `juce::Drawable`, single-colour, tinted from theme). No letters, no emoji, no Font Awesome PUA codepoints. |
| **Visualizer** | Must be driven by **real audio data** (PCM waveform / spectrum) and real musical events — not beat-phase sawtooths. |

---

# Part 1 — Full Gap Inventory

## 1.1 Project lifecycle (startup)

| # | Issue | Evidence |
|---|-------|----------|
| P1 | Workspace root = process CWD, hardcoded. Launching outside a project gives broken IDE. | `ui/MainWindow.cpp:181-186` |
| P2 | No welcome screen; no Open Folder / New Project dialog anywhere (`browseForDirectory` never used for opening). | repo-wide grep |
| P3 | No way to change workspace root after launch — `ExplorerPanel::setDirectory` has no caller in any UI control. | `ExplorerPanel.cpp:60-76` |
| P4 | No recent-projects MRU. Only `explorerLastDirectory` persisted. | `ExplorerPanel.cpp:140-166` |
| P5 | Bug: `setWorkspaceRoot` only rebuilds search model `if (workspaceSearchModel_)` — inverted guard, stale state on first call. | `EditorArea.cpp:2446-2447` |
| P6 | `.hathor_assets/chuck_instruments` resolved under CWD. | `app/Main.cpp:216-217`, `HathorApplication.cpp:125-128` |

## 1.2 Icons & visual design

| # | Issue | Evidence |
|---|-------|----------|
| I1 | No icon system exists: no SVG/Drawable/ImageCache/icon font anywhere in `ui/`. | repo-wide grep |
| I2 | ActivityRibbon buttons are letters `"E","S","V","D",">","!","A"` despite "icon-only" header claims. | `ActivityRibbon.hpp:92-103`, `ActivityRibbon.cpp:101-108` |
| I3 | ChatSidebar close button renders Font Awesome U+F00D with no icon font → tofu box. | `ChatSidebar.cpp:256` |
| I4 | Breadcrumbs use emoji fallbacks ("⚡","🔍","⊞"). | `BreadcrumbsBar.cpp:107-112` |
| I5 | Explorer file-type icons are stub geometry, deferred "to later phases". | `ExplorerTreeItems.hpp:18-20`, `ExplorerFileTypes.hpp:53` |
| I6 | `getTypefaceForFont()` hijacks ALL fonts to JetBrains Mono — menus/buttons/prose all monospace. | `HathorLookAndFeel.cpp:450-473` |
| I7 | Default theme neon green #0F0 on near-black; 5 themes copy-paste identical except accent. | `HathorLookAndFeel.cpp:286-429` |
| I8 | StatusRibbon claims icon-based indicators, ships painted text/shapes. | `StatusRibbon.hpp:11-19` |
| I9 | App icon "bleeds" to the canvas edge — artwork not inset within the macOS squircle margin, so it renders visually larger than every other dock icon. Icon ships as pre-built `.icns` (1024px master). | `ui/CMakeLists.txt:33-70`, `ui/resources/HathorAppIcon.icns` |

## 1.3 ACP agent connection

| # | Issue | Evidence |
|---|-------|----------|
| A1 | Agent binary must be an absolute path (posix_spawn, no PATH lookup); `claude-agent-acp` style names silently fail with generic error. No existence/executability pre-check. | `AcpAgentSession.cpp:338-403, 480-484` |
| A2 | No known-agent registry/presets (Claude Code, Gemini CLI, Codex, Cline, Kilo) and no picker UI in Settings beyond a raw path text field. | `SettingsComponent.cpp:444-455` |
| A3 | Hardcoded `protocolVersion: 1`, no negotiation handling. | `AcpAgentSession.cpp:500` |
| A4 | Reader reads fixed 4096-byte `fgets` chunks — long JSON-RPC lines split and silently dropped. | `AcpAgentSession.cpp:599-628` |
| A5 | Post-init prompt failures not surfaced to chat; permission timer threads detached capturing bare `this` (use-after-free risk within 30 s window). | `AcpAgentSession.cpp:655-668, 787-816` |
| A6 | 5 s init timeouts trip on Node-based agents' cold start; reconnect loop sleeps hardcoded. | `AcpAgentSession.cpp:503, 530, 829` |
| A7 | Spawn failure diagnostics generic; no stderr capture from dead agent shown to user. | `AcpAgentSession.cpp:480-484`, `ChatThread.cpp:145,244,514` |

## 1.4 Editor (.hathor / .ck)

| # | Issue | Evidence |
|---|-------|----------|
| E1 | Cmd/Ctrl+Enter via ActionRegistry routes `.hathor` eval → `bakeActiveTab()` → rejected ("Bake only applies to ChucK tabs") while direct Ctrl+Enter keylistener works — two conflicting semantics for same command. | `EditorArea.cpp:1699-1708, 1918-1921, 2023-2043` |
| E2 | `.ck` files auto-evaluate on open — clicking a file in Explorer compiles + starts audio. | `EditorArea.cpp:754-758, 779-782` |
| E3 | Closing untitled dirty buffer in split-view path errors "cannot save untitled buffer"; no save-as fallback; buffer trapped. | `EditorGroup.cpp:434-438` |
| E4 | No Cmd+N binding for `tab.new`; new-tab reachable only via palette/context menu. | `EditorArea.cpp:3107-3150` |
| E5 | New-buffer flow has no template/language choice; tokeniser fixed at construction, unsaved buffer has no highlighting until saved. | `HathorTab.cpp:172-180` |
| E6 | No bracket auto-close; bracket matcher false-matches `<`/`>`. | `HathorTab.cpp:493-593, 518-519` |
| E7 | Context-menu "Eval Block" branches identical (dead differentiation). | `HathorTab.cpp:650-660` |

## 1.5 Visualizer ("the thing below the editor")

| # | Issue | Evidence |
|---|-------|----------|
| V1 | `SpscSampleRing` (raw PCM) is completely dead — nothing pushes audio samples, nothing consumes them. No real waveform possible today. | only refs: own header + tests |
| V2 | "Waveform" mode plots `fmod(cyclePos_, 1.0)` history = sawtooth of beat phase, identical every play. Conveys zero audio content. | `VisualizerPanel.cpp:305-306` |
| V3 | StepGrid cell mapping = `hash(sampleName) % 32` — arbitrary scatter, no musical grid position. | `VisualizerPanel.cpp:91-93` |
| V4 | Ring repaints are event-driven; idle ring animation stalls without frames. | `VisualizerPanel.cpp:119, 148-154` |

## 1.6 Shell / infrastructure

| # | Issue | Evidence |
|---|-------|----------|
| S1 | No menu bar at all (zero MenuBarModel usage). | repo-wide grep |
| S2 | No drag-and-drop file open. | repo-wide grep |
| S3 | Panels fixed-width: ribbon 48 px, explorer 240 px, chat 320 px; no splitters. Visualizer permanently ≥120 px, no toggle. | `MainWindow.cpp:669-693, 671, 690, 699` |
| S4 | Orphan SliderPanel: constructed + synced at 60 Hz but never added to layout (invisible duplicate of ChatSidebar's instance). | `MainWindow.cpp:173, 571-576, 530-543` |
| S5 | Keymap mapping incomplete: digits, punctuation, Ctrl+Shift chords don't register. | `MainWindow.cpp:963-992` |
| S6 | Explorer starts hidden every launch despite persisting last directory. | `MainWindow.cpp:550` |
| S7 | Explorer polls full recursive tree every 2 s on timer thread; no ignore rules (crawls node_modules-scale trees). Stale doc comments reference nonexistent DirectoryWatcher. | `ExplorerPanel.cpp:207-274, 44-47` |
| S8 | Settings opacity slider admits "unsupported" on macOS — dead control. | `SettingsComponent.cpp:338-352` |

## 1.7 Explorer functionality

| # | Issue | Evidence |
|---|-------|----------|
| X1 | Explorer is **read-only**: no create/rename/delete of files or folders, no context menu, no popup menu APIs anywhere. | `ExplorerPanel.*`, `ExplorerTreeItems.*` greps |
| X2 | Single-click and double-click fire identical open callback (no selection vs open distinction). | `ExplorerTreeItems.cpp:98-104, 198-203` |

## 1.8 Chat persistence & misc

| # | Issue | Evidence |
|---|-------|----------|
| C1 | Chat restore persists tab titles only — all message history silently lost on restart. | `ChatSidebar.cpp:498-575` |
| C2 | SliderPanel drag spawns `std::thread(...).detach()` per change event capturing `ControlInterface&` — thread churn + dangling ref risk. | `SliderPanel.cpp:84-87, 134-137` |
| C3 | LSP notify uses static map keyed by raw `Component*` + global shared version across tabs (leak + wrong versioning). | `HathorTab.cpp:956-967` |
| C4 | Ghost provider endpoints hardcoded (`localhost:11434` Ollama, `localhost:8080` llm-ls), not editable in Settings UI. | `GhostProviderConfig.cpp:130-133` |
| C5 | Worker/MCP binaries required as exact siblings of executable — breaks dev-run layouts. | `HathorApplication.cpp:159-161, 215-217` |
| C6 | Samples assumed 44100 Hz regardless of device; BPM default 120 and version "0.1.0" hardcoded. | `Main.cpp:201, 102, 277`; `AudioEngine.hpp:428` |
| C7 | Windows DebugSession returns None (out of scope per Phase 4 decision — document, don't fix). | `DebugSession.cpp:42-45` |

---

# Part 2 — Remediation Waves (parallel agents)

Each wave's tasks are independent of each other; dependencies are between waves.
Every agent must build with `cmake --build build --target hathor-ui` and run
`hathor-ui-tests` before reporting done. Commit one logical change per agent.

---

## Wave 0 — Unblock usability fundamentals (no deps, all parallel)

### Agent 0.1 — Welcome / Open Folder / New Project flow
- Add startup welcome overlay when no workspace persisted: [Open Folder…] (native `FileChooser::browseForDirectory`), [New Project…] (folder name + scaffold minimal `.hathor` + samples dir), [Open Recent…].
- Persist `lastWorkspacePath` in PropertiesFile; on launch restore it instead of CWD (`ui/MainWindow.cpp:181-186`).
- Fix inverted guard bug `EditorArea.cpp:2446-2447`.
- Files: new `ui/WelcomeScreen.{hpp,cpp}`, `ui/MainWindow.cpp`, `ui/EditorArea.cpp`.

### Agent 0.2 — Runtime workspace switching + recent projects
- Add "Open Folder…" + "Change Explorer Root" actions wired to `ExplorerPanel::setDirectory` and `EditorArea::setWorkspaceRoot`; close tabs belonging to old root (prompt).
- MRU list (max 10) persisted in PropertiesFile; surfaced in welcome screen + command palette.
- Move `.hathor_assets` resolution to workspace-root-relative, not process-CWD-relative (`app/Main.cpp:216-217`).

### Agent 0.3 — Eval command unification (E1, E7)
- Route ActionRegistry `"editor.eval"` through the same handler as TabKeyListener Ctrl+Enter: block-eval for `.hathor`, whole-buffer ChucK eval for `.ck`. Delete the `bakeActiveTab()` misroute (`EditorArea.cpp:1699-1708`). Deduplicate the identical context-menu branches (`HathorTab.cpp:650-660`).
- Remove `.ck` auto-eval-on-open (E2, `EditorArea.cpp:754-782`) — eval only on explicit user action.

### Agent 0.4 — Untitled buffer trap + save-as (E3, E4, E5)
- `EditorGroup::closeTab` untitled-dirty path: fall back to native save-as chooser (mirror `EditorArea::closeTab`).
- Bind Cmd+N/Ctrl+N to `tab.new`; add New File dialog offering template (empty `.hathor` front-matter, empty `.ck`, blank).
- Allow tokeniser selection for unsaved buffers (deferred tokeniser swap or lazy editor construction).

### Agent 0.5 — Kill orphan SliderPanel + detach-thread fixes (S4, C2)
- Delete MainWindow's invisible SliderPanel instance; keep ChatSidebar's.
- Replace per-drag detached threads with a message-thread async dispatch or debounced worker; verify no dangling `ControlInterface&` at shutdown.

### Agent 0.6 — Icon system foundation (I1, I2, I3, I4, I5, I8)
- Embed an SVG icon set (e.g. Lucide, ISC-licensed) via BinaryData; add `IconComponent`/`drawIcon(g, id, bounds, colour)` helper in LookAndFeel using `Drawable::createFromSVGBinary`.
- Replace: ActivityRibbon letters → icons (files, search, git, debug, terminal, problems, robot, settings gear); ChatSidebar U+F00D → proper close icon; breadcrumb emoji → icons; StatusRibbon transport/git indicators → icons.
- Explorer file-type glyphs route through same system (`.hathor`, `.ck`, folder, audio).

---

## Wave 1 — Make the AI connection real (deps: none hard; 1.2 benefits from 0.x stability)

### Agent 1.1 — ACP robustness (A1, A4, A5, A6, A7)
- PATH resolution for agent names (use `execvp` semantics or resolve via `/usr/bin/env`); pre-spawn check file exists+executable with actionable error.
- Replace fixed `fgets(4096)` reader with growable line buffer.
- Surface prompt errors + agent stderr tail in ChatThread messages; kill bare-`this` detached permission timers (own thread object owned by session, or AsyncUpdater).
- Configurable init timeout (default 15 s) + progress feedback while connecting.

### Agent 1.2 — Known-agent registry + picker UI (A2)
- Built-in presets table: Claude Code (`claude-code-acp`), Gemini CLI (`gemini --experimental-acp`), Codex, Cline, Kilo — name, default argv, notes; stored in JSON config, user-extensible.
- Settings: dropdown of detected agents (scan PATH) + browse-for-file fallback + args field. ChatSidebar gets an agent selector in its header with reconnect button.
- Version negotiation: read `protocolVersion` from initialize response, handle mismatch gracefully (A3).

### Agent 1.3 — Ghost provider settings (C4)
- Expose Ollama/llm-ls endpoints in Settings UI, persisted via existing provider-config mechanism.

---

## Wave 2 — Look & feel pass (deps: Wave 0.6 icons exist)

### Agent 2.1 — Typography split (I6)
- Stop hijacking all fonts: JetBrains Mono only for code editors, terminal, log views. Introduce a proportional UI face (embed Inter/SF-proportional fallback) for buttons/menus/labels via distinct colour-ID/font-ID plumbing in LookAndFeel.

### Agent 2.2 — Theme quality (I7)
- Refactor theme table: single base palette + accent parameterisation (kill 25-literal × 5 copy-paste).
- Tone down default accent; verify contrast ratios; update previews in Settings.

### Agent 2.3 — Layout: splitters, menubar, DnD (S1, S2, S3, S6)
- Resizable vertical splitters between ribbon/explorer/editor/chat; collapsible visualizer strip with toggle; remember sizes in PropertiesFile.
- Native menu bar: File (New File, Open File/Folder, Open Recent, Save, Save As, Close Tab, Quit), Edit, View (panel toggles), Transport, Help. Reuse existing ActionRegistry actions as menu targets.
- Drag-and-drop files/folders onto window → open/set-root.
- Explorer visible on launch when workspace restored.

### Agent 2.4 — App icon sizing fix (I9)
- Regenerate `ui/resources/HathorAppIcon.icns`: inset the 1024px master so artwork occupies ~80–82% of canvas (Apple's standard squircle margin, per HIG grid) instead of edge-to-edge.
- Verify at all dock sizes (16→1024px) that it optically matches Xcode/Finder neighbours; keep rounded-rect mask baked in or let macOS mask it consistently.
- Rebuild bundle and visually compare against other apps in Dock before/after screenshot.

---

## Wave 3 — Real visualizer (deps: none; parallel with Waves 1–2)

### Agent 3.1 — Wire PCM into the UI (V1)
- Push actual audio samples from `AudioEngine::processBlock` into the existing SPSC float ring (`SpscSampleRing`) alongside `VisualizerFrame`; UITimer drains both. Keep seqlock discipline; no allocations on audio thread.

### Agent 3.2 — Rewrite visualizer modes (V2, V3, V4)
- Waveform mode: render true PCM ring (post/pre-gain), decimated to panel width.
- Step grid: map cells to actual slot/step positions from engine events, not name hash.
- Spectrum option (simple FFT over ring) if cheap; ensure continuous repaint via timer even when idle.
- Tests: extend `tests-ui/test_spsc_ring_buffer.cpp` for PCM path.

---

## Wave 4 — Explorer + editor depth (deps: Wave 0.6 icons; parallel internally)

### Agent 4.1 — Explorer file management (X1, X2)
- Context menu: New File…, New Folder…, Rename, Duplicate, Delete (with confirm), Reveal in Finder, Copy Path.
- Single-click selects; double-click opens (separate callbacks).
- Replace 2 s full-tree poll with JUCE `FileSystemWatcher`-style change detection (or kqueue/FSEvents wrapper); honour ignore list (`.git`, `node_modules`, build dirs) configurable in Settings.

### Agent 4.2 — Editor ergonomics (E6, C3)
- Bracket auto-close + wrap-selection; remove `<`/`>` false matches.
- Fix LSP static-map leak: per-tab version counter, weak-ref-safe registration.

### Agent 4.3 — Keymap completeness (S5)
- Extend key-mapping layer to digits/punctuation/modifier combos; document full shortcut table; add shortcut cheat-sheet to Help menu.

---

## Wave 5 — Persistence & cleanup (parallel)

### Agent 5.1 — Chat history persistence (C1)
- Serialise full message threads (role/content/timestamps/tool events) to workspace-scoped JSON; restore verbatim on relaunch.

### Agent 5.2 — Hardcoded-value sweep (C5, C6)
- Worker/MCP sibling resolution: fall back to search near executable then configured path; surface clear error if missing.
- Device-rate-aware sample loading (resample or warn); move defaults (BPM, version string) to single constants/config.

### Agent 5.3 — Dead UI removal (S8)
- Remove or implement the macOS opacity control; remove stale DirectoryWatcher doc comments (`ExplorerPanel.cpp:44-47, 95`).

---

## Exit criteria

1. Fresh launch with no prior state → welcome screen → Open Folder to any project → explorer shows it, editor opens files, Ctrl+Enter plays audio. No CWD dependence anywhere.
2. Settings → pick "Claude Code" (or any preset) → chat connects, prompts stream, errors readable. Works with at least two real ACP CLIs end-to-end.
3. Every chrome element that looks like a button/icon **is** one; zero letter-buttons, emoji, tofu boxes; UI prose in proportional font, code in JetBrains Mono.
4. Visualizer below the editor visibly reacts to actual audio content (waveform/spectrum), and step grid reflects real pattern events.
5. Explorer can create/rename/delete; panels resize; menu bar covers File/Edit/View/Transport; DnD opens files.
6. Chat history, workspace, layout all survive restart.
7. App icon optically matches other macOS dock icons (proper squircle inset, no bleed).
8. Full build green (Debug + Release, `HATHOR_BUILD_APP=ON`), `hathor-ui-tests` pass, no new `-Werror` warnings.

---

# Part 3 — Copy-Pastable Agent Prompts

> Each prompt is self-contained. Launch agents within a wave **in parallel**; never start
> a wave before its dependency wave is merged. Every prompt repeats its own guardrails and
> DoD so it works standalone.

## Wave 0

### Prompt — Agent 0.1 (Welcome / Open Folder / New Project)

```
Repo: Hathor (JUCE C++ live-coding IDE, macOS). Read docs/frontend-remediation-audit.md §1.1 first.

TASK: Implement the project open/create flow.
1. Create ui/WelcomeScreen.{hpp,cpp}: overlay shown on launch when no workspace was
   previously persisted. Buttons: [Open Folder…] (native FileChooser::browseForDirectory),
   [New Project…] (asks folder name + parent dir via native choosers, then scaffolds a
   project dir with a minimal starter .hathor file and a samples/ subfolder), [Open Recent…]
   (list from persisted MRU — may be empty initially).
2. In ui/MainWindow.cpp (~181-186): stop using process CWD as workspace root. Restore
   lastWorkspacePath from the existing juce::PropertiesFile instead; only if unset/invalid,
   show WelcomeScreen. Persist the chosen path on selection and on quit.
3. Fix inverted guard bug at ui/EditorArea.cpp:2446-2447 (`if (workspaceSearchModel_)`
   rebuilds only when it already exists) so the search model is rebuilt on EVERY call.

SCOPE GUARDRAILS: Do not touch AcpAgentSession, VisualizerPanel, ExplorerPanel internals,
fonts/icons/theme, or engine/app code beyond passing the workspace root through.
No new third-party deps. Keep JUCE idioms. No unrelated refactors.

DOD:
- hathor-ui builds Debug AND Release (HATHOR_BUILD_APP=ON), no new -Werror warnings;
  hathor-ui-tests pass.
- Fresh launch (properties cleared) shows welcome screen; Open Folder opens that directory
  in Explorer + EditorArea; relaunch restores it without welcome screen.
- New Project creates the scaffold on disk and opens it.
- grep confirms zero remaining use of getCurrentWorkingDirectory as workspace root.
- Commit starting "0.1:".
```

### Prompt — Agent 0.2 (Runtime workspace switching + recent projects)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.1.
NOTE: Agent 0.1 owns WelcomeScreen + lastWorkspacePath persistence. Read current HEAD of
MainWindow.cpp first: if 0.1 merged, build on it; if not, implement against existing startup
path without duplicating its work (your deliverable = runtime switching + MRU).

TASK:
1. Add "Open Folder…" action (command palette + ribbon context) using
   FileChooser::browseForDirectory that switches workspace at runtime: call
   ExplorerPanel::setDirectory (ExplorerPanel.cpp:60-76) AND EditorArea::setWorkspaceRoot.
   Before switching, close tabs under the old root via the existing close-tab save prompts.
2. Recent-projects MRU (max 10, most-recent-first, dedup) in PropertiesFile, surfaced in
   the command palette as "Open Recent: <path>" entries.
3. Resolve .hathor_assets relative to WORKSPACE ROOT, not process CWD
   (app/Main.cpp:216-217, ui/HathorApplication.cpp:125-128).

SCOPE GUARDRAILS: Don't modify WelcomeScreen, editor keybindings, tokenisers, ACP,
visualizer, LookAndFeel, or ControlInterface/engine behaviour.

DOD:
- Build Debug+Release clean; tests pass.
- Runtime switch works: explorer re-roots, foreign tabs close with save prompt, workspace
  search still functions afterwards.
- MRU persists across restart; max 10; no dupes.
- .hathor_assets resolves under the opened project even when launched from /.
- Commit "0.2:".
```

### Prompt — Agent 0.3 (Eval command unification)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.4 items E1, E2, E7.

TASK:
1. Bug: ActionRegistry binds Cmd+Enter → "editor.eval" → tab->onEvalBlock, which for
   non-ChucK tabs is wired to bakeActiveTab() (ui/EditorArea.cpp:1699-1708) and rejects
   .hathor tabs with "Bake to Song only applies to ChucK (.ck) tabs." (:1918-1921), while
   the direct Ctrl+Enter listener path (handleKeyPress :2023-2044) works correctly.
   Rewire onEvalBlock for .hathor tabs to the SAME logic handleKeyPress uses (block eval
   via extractEvalBlock; whole-file for Ctrl+Alt+Enter). One source of truth; delete the
   bake misroute. Deduplicate identical if/else context-menu branches
   (ui/HathorTab.cpp:650-660).
2. Remove .ck auto-eval-on-open (EditorArea.cpp:754-758, 779-782): opening a .ck must NOT
   compile or start audio; eval only on explicit user action.

SCOPE GUARDRAILS: EditorArea eval wiring + HathorTab context menu ONLY. No ChucK worker
protocol, AudioEngine, save-flow, or keybinding changes beyond these bindings.

DOD:
- Build clean Debug+Release; tests-ui pass.
- Verify: Ctrl+Enter evaluates block in .hathor; Cmd+Enter does the same; context-menu Eval
  Block works on .hathor; opening a .ck produces no audio until user evals.
- Commit "0.3:".
```

### Prompt — Agent 0.4 (Untitled buffer trap + new-file flow)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.4 items E3, E4, E5.

TASK:
1. Fix trap: ui/EditorGroup.cpp:434-438 — closing an untitled dirty buffer errors "cannot
   save untitled buffer" and aborts. Mirror EditorArea::closeTab's fallback (~970-1008):
   route through native save-as chooser (*.hathor;*.ck); Cancel cancels close; successful
   save closes cleanly.
2. Bind Cmd+N/Ctrl+N to the existing "tab.new" action (registry at
   ui/EditorArea.cpp:3107-3150).
3. New-buffer dialog offering templates: Empty .hathor (front-matter skeleton), Empty .ck
   (skeleton comment), Blank. Template seeds buffer and selects matching tokeniser
   immediately. If tokeniser cannot swap post-construction (see HathorTab.cpp:172-180),
   construct the editor lazily / defer until type known — least invasive option.

SCOPE GUARDRAILS: EditorGroup/EditorArea/HathorTab only. No eval-logic changes (Agent 0.3
owns those), no visualizer/ACP/font work.

DOD:
- Build clean; tests-ui pass.
- Untitled dirty buffer closes via Save(save-as)/Discard/Cancel without dead ends in BOTH
  single-editor and split-view paths.
- Cmd+N opens dialog; ".ck template" gives ChucK highlighting before any save.
- Commit "0.4:".
```

### Prompt — Agent 0.5 (SliderPanel cleanup + thread fixes)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.6 S4 and §1.8 C2.

TASK:
1. Delete MainWindow's orphan SliderPanel (constructed ~MainWindow.cpp:173, synced by
   UITimer :571-576, never added/made visible). ChatSidebar's instance (:81-82) becomes the
   single BPM/gain UI. Remove dead sync code.
2. Fix SliderPanel threading (ui/SliderPanel.cpp:84-87, 134-137): per-drag-change
   std::thread(...).detach() capturing ControlInterface& → replace with debounced dispatch
   on the JUCE message thread (timer/AsyncUpdater coalescing latest value); nothing may
   outlive the panel.

SCOPE GUARDRAILS: MainWindow slider wiring, UITimer sync block, SliderPanel.{hpp,cpp},
ChatSidebar slider integration only. No ControlInterface/AudioEngine changes.

DOD:
- Build clean; tests pass.
- Exactly ONE BPM/gain slider surface exists, draggable, values reach the engine (verify via
  RuntimeInspectorPanel or control ping).
- Zero detached threads spawned during a fast drag; app quits cleanly mid-drag.
- Commit "0.5:".
```

### Prompt — Agent 0.6 (Icon system foundation)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.2 (I1-I5, I8).

TASK: Introduce the icon system and replace ALL placeholder glyphs.
1. Embedded SVG icon set (Lucide, ISC licence — include attribution) via JUCE BinaryData +
   central helper (e.g. ui/IconLibrary): cached juce::Drawable per name, tintable from theme.
2. Replace: ActivityRibbon letters "E","S","V","D",">","!","A","P"
   (ActivityRibbon.hpp:92-103, painted :101-108) with real icons (files, search, git branch,
   bug, terminal, warning, robot/sparkles, gear); ChatSidebar PUA glyph U+F00D
   (ChatSidebar.cpp:256) → proper X icon; BreadcrumbsBar emoji ⚡🔍⊞ (BreadcrumbsBar.cpp:
   107-112) → icons; StatusRibbon text indicators → play/stop/bpm/git icons;
   ExplorerTreeItems inline geometry glyphs → IconLibrary icons (folder, .hathor, .ck, wav,
   generic).
3. Icons tint with theme accent/foreground and follow theme switching.

SCOPE GUARDRAILS: Glyph layer only. Do NOT change font policy (Agent 2.1), theme palette
structure (Agent 2.2), layout sizes, or panel logic.

DOD:
- Build clean Debug+Release; tests-ui pass.
- Zero letter-labels / breadcrumb emoji / U+F00D remain (grep).
- Every ribbon/status/explorer entry renders a recognisable monochrome icon that recolours
  when themes switch.
- Lucide licence file added (e.g. ui/resources/icons/LICENSE).
- Commit "0.6:".
```

## Wave 1

### Prompt — Agent 1.1 (ACP robustness)

```
Repo: Hathor (JUCE C++ IDE, ACP client). Read docs/frontend-remediation-audit.md §1.3
(A1, A3-A7) and skim ui/AcpAgentSession.cpp fully first.

TASK: Harden the ACP client so any standard ACP CLI agent connects reliably.
1. PATH resolution: allow bare names (e.g. "claude-code-acp") resolved against $PATH;
   pre-spawn validate exists+executable; actionable errors naming what was searched.
2. Replace fixed 4096-byte fgets reader (:599-607) with growable line assembly so long
   JSON-RPC lines never split/drop silently (:624-628).
3. Surface failures: post-init prompt errors parsed-but-ignored in readerLoop (:655-668)
   propagate into ChatThread as visible messages; show last N lines of agent stderr after
   abnormal exit.
4. Replace bare-`this` detached permission-timer threads (:787-816) with a session-owned
   mechanism safe against destruction within the timeout window.
5. Configurable init timeout default 15 s (:503,:530); visible "connecting" state during
   handshake; graceful protocolVersion mismatch handling instead of hardcoded version 1
   (:500).

SCOPE GUARDRAILS: AcpAgentSession.{hpp,cpp} + ChatThread display integration only. No MCP
server changes; no Settings/picker UI (Agent 1.2 owns); no new protocol features.

DOD:
- Build clean; existing ACP tests pass; new unit test proves >4096-char lines parse.
- Verified end-to-end with TWO real ACP CLIs (e.g. claude-code-acp and gemini
  --experimental-acp): connect, prompt, streamed reply, permission flow.
- Killing the agent mid-session shows readable error, no crash, reconnect possible.
- Commit "1.1:".
```

### Prompt — Agent 1.2 (Known-agent registry + picker UI)

```
Repo: Hathor (JUCE C++ IDE, ACP client). Read docs/frontend-remediation-audit.md §1.3 A2.
DEPENDENCY: builds on Agent 1.1 — rebase on current main first.

TASK:
1. Known-agent registry: presets for Claude Code (claude-code-acp), Gemini CLI (gemini
   --experimental-acp), Codex, Cline, Kilo — {name, argv template, notes}; JSON config
   (bundled defaults + user overrides in app data), user-extensible.
2. Settings UI: replace raw "Agent exe:" field (SettingsComponent.cpp:444-455) with preset
   dropdown + Detect button (scan $PATH, mark found) + Browse… fallback + args field; keep
   persisted agentExePath semantics compatible.
3. ChatSidebar header: compact current-agent selector + reconnect button; switching agent
   restarts that tab's session. Apply path (MainWindow.cpp:250-269) keeps working.

SCOPE GUARDRAILS: Settings agent section, ChatSidebar header, config layer only. No
protocol-level changes (Agent 1.1 owns), no other settings sections.

DOD:
- Build clean; tests pass.
- Selecting a detected preset connects next chat session with zero path typing; browse-path
  still works; user-added registry entries persist across restart.
- Commit "1.2:".
```

### Prompt — Agent 1.3 (Ghost provider endpoints configurable)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.8 C4.

TASK: Ghost LLM endpoints are hardcoded (Ollama http://localhost:11434, llm-ls
http://localhost:8080 — ui/GhostProviderConfig.cpp:130-133). Add editable URL/port fields
per provider in the existing Settings completion section, persisted via the existing
provider-config mechanism. Validate URL format on apply; blank falls back to defaults.

SCOPE GUARDRAILS: GhostProviderConfig + relevant Settings section only. No ACP settings
(Agent 1.2 owns), no GhostLlmClient protocol logic changes.

DOD: Build clean; endpoint change persists across restart and takes effect on next
completion request; invalid input rejected inline. Commit "1.3:".
```

## Wave 2

### Prompt — Agent 2.1 (Typography split)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.2 I6.
DEPENDENCY: after Agent 0.6 merged (icon system).

TASK: End the global monospace hijack. ui/HathorLookAndFeel.cpp:450-473 forces JetBrains
Mono for ALL fonts. New policy: JetBrains Mono ONLY where code is displayed (code editor,
terminal, log/output views); everything else (buttons, menus, labels, chat prose,
breadcrumbs, status) uses a proportional UI face — embed Inter (OFL licence, add
attribution) with system proportional fallback. Implement explicit helpers
(getCodeFont()/getUiFont()) in LookAndFeel instead of intercepting getTypefaceForFont for
sans-serif; migrate components that fetch fonts directly.

SCOPE GUARDRAILS: Font plumbing only. No icon/colour/layout/editor-feature changes.

DOD: Build clean; menus/buttons/chat render proportional; editor+terminal remain JetBrains
Mono; theme switching unaffected; Inter licence included. Commit "2.1:".
```

### Prompt — Agent 2.2 (Theme quality)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.2 I7.
DEPENDENCY: after Agent 0.6 (icons tint from theme).

TASK: Refactor ui/HathorLookAndFeel.cpp theme registry (:286-429) — five copy-paste themes
identical except accent → ONE base palette parameterised by accent (+optional secondary),
deriving all colour IDs. Tone down the default neon #00ff41 to something legible/tasteful on
dark surfaces (document choice); WCAG-check text-on-surface pairs and adjust; keep Settings
theme previews working.

SCOPE GUARDRAILS: Colour system only. No layout/font/icon changes; preserve existing
colour-ID names so all consumers compile.

DOD: Build clean; themes switch correctly; single parameterised definition (no duplicated
literal blocks); contrast check documented. Commit "2.2:".
```

### Prompt — Agent 2.3 (Splitters, menubar, DnD)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.6 (S1,S2,S3,S6).

TASK:
1. Resizable vertical splitters between ribbon/explorer/editor/chat (fixed 48/240/320 px at
   MainWindow.cpp:669-693, MainWindow.hpp:172); persist widths in PropertiesFile; clamp min/max.
2. Visualizer strip collapsible with toggle instead of always eating max(h/4,120); persist state.
3. Native menu bar (MenuBarModel): File (New File…, Open File…, Open Folder…, Open Recent ▸,
   Save, Save As…, Close Tab, Quit), Edit, View (panel toggles), Transport, Help — wire to
   EXISTING ActionRegistry actions, no duplicated handlers.
4. Drag-and-drop: file drops open tabs; folder drop offers "Set as workspace"
   (FileDragAndDropTarget on main content).
5. Explorer visible on launch when workspace restored (MainWindow.cpp:550 always hidden now).

SCOPE GUARDRAILS: MainWindow shell/layout/menu/DnD only; panel internals untouched; no
editor/eval changes.

DOD: Build clean; panels resize + persist; menu items perform real actions; dropping a .ck
opens it without auto-eval; folder drop prompts workspace switch; layout survives restart.
Commit "2.3:".
```

### Prompt — Agent 2.4 (App icon sizing fix)

```
Repo: Hathor (JUCE C++ IDE, macOS bundle). Read docs/frontend-remediation-audit.md §1.2 I9.

TASK: App icon bleeds edge-to-edge and looks oversized vs other dock icons. Regenerate
ui/resources/HathorAppIcon.icns (referenced from ui/CMakeLists.txt:33-70): inset artwork to
~80–82% of the 1024px canvas (Apple squircle grid margin), preserving the design. Export the
full icns ladder (16→1024 + @2x). Rebuild bundle; compare optically against neighbouring
Dock apps including small sizes.

SCOPE GUARDRAILS: Resources/build-icon plumbing only; no app code changes.

DOD: Correctly inset icon in Dock/Finder/Command-Tab; before/after screenshots in commit
notes; build still copies icns into bundle. Commit "2.4:".
```

## Wave 3

### Prompt — Agent 3.1 (Wire PCM into UI)

```
Repo: Hathor (JUCE C++ IDE + audio engine). Read docs/frontend-remediation-audit.md §1.5 V1;
skim app/AudioEngine.cpp processBlock, app/SpscSampleRing.hpp, ui/UITimer.cpp:110-118.

TASK: The float PCM ring SpscSampleRing is dead — nothing pushes samples, UI never sees raw
audio. Wire it: in AudioEngine's processBlock push a decimated/downmixed block of output
samples (mono, ≤256 samples/callback, post-gain) into the ring using its seqlock discipline —
zero allocation, lock-free, no shared_ptr, audio-thread safe. UITimer drain consumes PCM
alongside VisualizerFrame and hands it to VisualizerPanel via a new updateSamples API.
Extend tests-ui/test_spsc_ring_buffer.cpp for the PCM path.

SCOPE GUARDRAILS: AudioEngine write-site, ring, UITimer pump, VisualizerPanel input API
ONLY. Do not rewrite rendering modes (Agent 3.2 owns). Preserve VisualizerFrame event flow.

DOD: Build clean Debug+Release; tests pass incl. new PCM cases; samples demonstrably arrive
while playing and stop when stopped; no allocation/blocking on audio thread. Commit "3.1:".
```

### Prompt — Agent 3.2 (Rewrite visualizer modes)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.5 (V2,V3,V4).
DEPENDENCY: Agent 3.1 merged (real PCM arriving).

TASK: Rewrite ui/VisualizerPanel.cpp rendering:
1. Waveform mode: plot actual incoming PCM (decimated to panel width), replacing the fake
   fmod(cyclePos_,1.0) sawtooth (:305-306). Amplitude follows gain; flatline when silent.
2. Step grid: cells map to REAL slot/step positions from VisualizerFrame events — delete
   hash(sampleName)%32 scatter (:91-93); light in musical order/time position.
3. Pulse mode reacts to event hits.
4. Continuous repaint from UITimer regardless of frame arrival (:148-154 stall fix).
5. Optional spectrum mode (small FFT over ring) only if dependency-free.

SCOPE GUARDRAILS: VisualizerPanel painting/input consumption only. No ring/AudioEngine/
layout changes (collapsible strip is Agent 2.3's).

DOD: Build clean; playing bd/sn shows amplitude-reactive waveform and ordered step lighting;
stopped = flatline + idle anim; per-mode screenshots included. Commit "3.2:".
```

## Wave 4

### Prompt — Agent 4.1 (Explorer file management)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.7 (X1,X2), §1.6 S7.
DEPENDENCY: after 0.6 icons.

TASK:
1. Context menu on explorer tree: New File… (inline name editor, extension-aware), New
   Folder…, Rename, Duplicate, Delete (confirm dialog), Reveal in Finder, Copy Path. Via
   juce::File; refresh tree; dirty open tabs on deleted/renamed files handled gracefully
   (keep buffer, mark orphaned).
2. Single-click selects, double-click opens (currently identical callbacks —
   ExplorerTreeItems.cpp:98-104,198-203,361-366).
3. Replace 2 s whole-tree poll (ExplorerPanel.cpp:207-274) with FSEvents/kqueue watching or
   incremental mtime-of-dirs check; ignore list (.git, node_modules, build*, DerivedData)
   configurable in Settings; remove stale DirectoryWatcher doc comments (:44-47,:95).

SCOPE GUARDRAILS: ExplorerPanel/ExplorerTreeItems + one Settings ignore-list field. Editor
open/save untouched beyond rename/delete notifications.

DOD: Build clean; all file ops work and reflect immediately; deleting an open file doesn't
crash; double-click-only opens; node_modules-scale trees don't peg CPU. Commit "4.1:".
```

### Prompt — Agent 4.2 (Editor ergonomics)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.4 E6, §1.8 C3.

TASK:
1. Bracket auto-close in GhostAwareEditor (ui/HathorTab.cpp): typing ( [ { " inserts pair
   (skip-over typed closer, single undo unit); opener wraps selection. Remove '<'/'>' from
   bracket-match set (:518-519).
2. Fix LSP bookkeeping (:956-967): static Component*-keyed map leaks + global changeVersion
   interleaves across tabs → per-tab version counter, weak/safe registration.

SCOPE GUARDRAILS: HathorTab editor behaviours only. No eval/keybind/tokeniser changes.

DOD: Build clean; pair typing behaves like a modern editor incl. undo granularity; highlight
correct for ()[]{} only; LSP versions monotonic per tab; no leak growth across open/close
stress loop. Commit "4.2:".
```

### Prompt — Agent 4.3 (Keymap completeness)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.6 S5.

TASK: Global key mapping (ui/MainWindow.cpp:963-992) covers only A–Z/F1–F12 — digits,
punctuation, modifier combos unmapped so registered actions silently fail. Extend mapping
(digits row, punctuation, Ctrl/Cmd+Shift combos); ensure keys reach DocumentWindow when an
editor has focus while editor-local bindings win locally (JUCE focus model). Generate
docs/SHORTCUTS.md from the ActionRegistry and surface it via Help ▸ Keyboard Shortcuts.

SCOPE GUARDRAILS: Key mapping layer + docs + help dialog only. No action implementation
changes.

DOD: Build clean; every registered action reachable via documented keystroke; normal typing
in editors unregressed; SHORTCUTS.md matches registry. Commit "4.3:".
```

## Wave 5

### Prompt — Agent 5.1 (Chat history persistence)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.8 C1.

TASK: ChatSidebar::saveChatState (:498-523) stores only tab titles; restoreChatThreads
(:525-575) recreates blank sessions — history lost every restart. Serialise FULL threads
(role, text chunks, timestamps, tool_call events, errors) to <workspace>/.hathor/
chat-history.json (atomic write, versioned schema); restore verbatim on launch; cap ~200
msgs/thread with truncation note. Restored transcript is read-only until next prompt starts
a live continuation.

SCOPE GUARDRAILS: ChatSidebar/ChatThread serialisation only. No ACP protocol changes.

DOD: Build clean; message → quit → relaunch preserves transcripts incl. tool events;
corrupt file degrades gracefully; schema version present. Commit "5.1:".
```

### Prompt — Agent 5.2 (Hardcoded-value sweep)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.8 (C5,C6).

TASK:
1. Worker/MCP sibling-binary resolution (ui/HathorApplication.cpp:159-161,215-217): exact-
   sibling requirement breaks dev layouts. Resolution order: sibling of exe → common CMake
   build-layout subdirs near exe → configured override property (hathor.xml). Clear startup
   error naming searched locations if missing.
2. Sample loading assumes 44100 Hz regardless of device (app/Main.cpp:201,
   HathorApplication.cpp:130, AudioEngine.hpp:428): load/resample relative to actual device
   rate, or resample samples once at load; at minimum warn loudly on mismatch.
3. Move magic defaults to named constants/config: BPM default 120 (app/Main.cpp:102,
   HathorApplication.cpp:69), version string "0.1.0" (app/Main.cpp:277-278) sourced from
   build definition.

SCOPE GUARDRAILS: Startup/config plumbing only. No engine DSP behaviour changes beyond rate
handling; no UI work.

DOD: Build clean; app runs with binaries in dev build layout; sample playback correct at
48000 Hz device; defaults centralised (single source each). Commit "5.2:".
```

### Prompt — Agent 5.3 (Dead UI removal)

```
Repo: Hathor (JUCE C++ IDE). Read docs/frontend-remediation-audit.md §1.6 S8.

TASK:
1. macOS opacity control admits "unsupported" (SettingsComponent.cpp:338-352) — remove it
   (and its persisted property) rather than shipping dead UI; if trivially implementable via
   JUCE setOpacity on the peer, you may implement instead, but removal is acceptable.
2. Remove stale DirectoryWatcher doc comments (ExplorerPanel.cpp:44-47,:95).
3. Sweep ui/ for any other visibly inert controls (labels describing future features,
   disabled-but-visible controls) — remove or gate them behind real functionality; list what
   you found and did in commit notes.

SCOPE GUARDRAILS: Removal/cleanup only; do not implement new features while sweeping; if an
inert control maps to another agent's task (visualizer toggle = 2.3), leave it alone.

DOD: Build clean; settings panel contains no dead controls; sweep findings documented.
Commit "5.3:".
```
```
