# Hathor Frontend Production Plan — VSCode-Level Quality

Source: file-by-file audit of all ~190 `ui/` files (2026-09-16). Each subphase
is independently committable; ordering within a phase is by dependency.
Every subphase must build (`hathor-ui`, Debug + Release, no new `-Werror`
warnings) and keep `ctest` green before commit. Commit prefix shown per
subphase (e.g. `P1.3:`).

Legend: **[B]** fixes a blocker (crash / dead feature / data loss),
**[M]** major gap vs VSCode bar, **[m]** minor polish bundled with the parent.

---

## Phase 1 — Stability (blockers first, ~2 weeks)

No feature work lands until Phase 1 is green: several items are crash or
data-loss class, and later phases build on lifetime guarantees made here.
Cross-cutting rule for all of Phase 1: **no raw `this`/`Component*` captured
into detached threads or `callAsync`** — use `juce::Component::SafePointer`
/ `WeakReference` or session-owned cancellation flags.

### P1.1 — Lifetime: tab + diagnostics threads [B]

- `ui/HathorTab.cpp:1738-1748` (`triggerChuckDiagnostics`): replace detached
  `std::thread` + raw `this` with a tab-owned cancellable worker
  (atomic generation counter checked before `callAsync`; destructor bumps
  the generation so late callbacks no-op).
- Same treatment for every other detached thread in `HathorTab.cpp`
  (`eval` paths): audit each `std::thread(...).detach()` and either join
  via owned `std::thread` member or cancel via generation.
- `ui/ExplorerPanel.cpp:99-107,319`: `callAsync([this]...)` → `SafePointer`.
- `ui/LspDiagnosticsDisplay.hpp`: stop storing raw `const Diagnostic*` into
  the `byLine` index (dangles on `setDiagnostics` realloc). Store indices
  or value copies; add a document revision so stale squiggles are dropped
  after edits.
- `ui/MainWindow.cpp:743-749`: detach `UITimer` hooks / stop timer before
  panel teardown; document destruction order (timer first, panels after).
- **DoD:** no `detach()` with raw `this` in `ui/` (grep); open/close-tab
  stress loop (50×) under ASan/TSan clean; shutdown clean.

### P1.2 — LookAndFeel crash + theming contract [B]

- `ui/HathorLookAndFeel.hpp/cpp`: clear `globalPalette_` on destruction;
  forbid silent overwrite in `setGlobalPalette()` (assert or return bool).
- `fromComponent()`: replace `static_cast` with `dynamic_cast` + fallback
  to `globalPalette()` (covers native dialogs, third-party components).
- `globalPalette()`: null-check with safe fallback instead of blind deref.
- Broadcast theme changes (listener / `sendLookAndFeelChange` audit) so
  direct `getPalette()` readers repaint; fix `HathorTab` slot-button
  no-op on theme switch (`setSlotRunningVisual` early-return,
  `HathorTab.hpp:228-238,450`).
- **DoD:** theme switch × all 5 themes with native dialogs open, no crash;
  slot buttons re-tint on theme change.

### P1.3 — Filesystem walker safety [B]

- `ui/TreeBuilder.cpp:76-128`: per-entry error handling with
  `std::error_code` (`skip_permission_denied`), `noexcept` honored —
  permission-denied entries skipped, never thrown.
- Symlink policy: do not follow directory symlinks (or track visited
  canonical paths); recursion depth cap (e.g. 32).
- Sort directory iteration for deterministic asset resolution
  (first-audio-wins per stem, `TreeBuilder.cpp:212-224`, becomes defined).
- `ui/ExplorerPanel.cpp:117-118`: replace `~`-sentinel with an explicit
  `hasWorkspace_` bool so a real workspace at `~` works.
- **DoD:** open repo with unreadable dirs + symlink loop + `~` as root;
  no terminate, deterministic tree, empty-workspace message shown.

### P1.4 — Dead core features [B]

- `ui/CommandPalette.cpp`: attach a `ListBoxModel`, push `filteredActions_`
  into it; wire Up/Down/Enter/Esc; fix case-insensitive filter (lower both
  sides); clamp geometry; return focus on hide.
- `ui/WelcomeScreen.cpp:108-129`: fix New Project dialog logic (save-mode
  path handling) so scaffolding actually runs; validate + surface errors.
- `ui/HathorMenuBarModel.hpp:242-257`: route Edit actions to the focused
  `CodeEditorComponent`, not just `juce::TextEditor` (Undo/Redo/Cut/Copy/
  Paste work in the editor); add key equivalents to menu items; tick marks
  for View toggles.
- `ui/MainWindow.cpp:1296-1299`: map F-keys via `juce::KeyPress::F1Key…`
  domain instead of the `0xF700` range.
- `ui/SourceControlPanel.cpp:490-523`: `createBranch()` must actually call
  `repository_->createBranch`; validate empty names.
- `ui/SettingsComponent.cpp`: handle Detect/Browse buttons; fix
  sample-rate combo ID/text mismatch; read appearance caps after controller
  is installed (not in ctor).
- `ui/SymbolSearchPanel.cpp:161-164`: wire `searchWorkspaceFiles` + LSP
  results, not metadata-only.
- **DoD:** each feature demonstrably works end-to-end (palette executes,
  project scaffolds, Edit menu edits, F12 jumps, branch creates, agent
  browse/detect fills the field, Ctrl+T finds workspace symbols).

### P1.5 — Data-loss fixes [B]

- `ui/MainWindow.cpp:1362-1404` (`switchWorkspace`): honor the dirty-tab
  Cancel veto (abort re-root + MRU push on cancel); move `pushRecentProject`
  after the equality check.
- Explorer Delete (`ui/ExplorerPanel.cpp:513-532`): move to system trash
  (`juce::File::moveToTrash`) instead of `deleteRecursively()`; surface
  failures with a toast/dialog.
- `ui/ChatSessionState.cpp`: fail-open — skip malformed threads instead of
  dropping the whole session; clamp `activeIndex`; cap thread count.
- `ui/HathorTab.cpp:760-773`: make the editor context menu reachable
  (handle inside `editor_`, not parent `mouseUp`); add keyboard path
  (Shift+F10 / Menu key); enable/disable items from real state
  (canUndo/canRedo/selection).
- **DoD:** cancel-switch keeps old workspace; deleted files recoverable
  from trash; corrupt chat file preserves good threads; right-click in
  editor always opens a correct menu.

---

## Phase 2 — Editor truth (~2 weeks)

Goal: one source of truth for tabs, correct language handling, sane editing.

### P2.1 — Single tab-stack authority [M]

- Resolve `EditorArea` dual stacks (`tabs_/activeIndex_` vs
  `editorSplitSurface_` groups): declare the split surface (groups) the
  owner; `EditorArea` keeps a read-through active-tab accessor only.
- `toggleSplit()` semantics documented + tested (which pane splits, where
  focus goes).
- **DoD:** `tests-ui` cases for open/close/split/activate consistency; no
  divergent tab counts between the two models.

### P2.2 — Real tokeniser swap [B]

- `ui/HathorTab.cpp:170-186`: on Save-As / extension change, reconstruct
  the `GhostAwareEditor` with the correct tokeniser (JUCE 8 cannot swap
  post-construction) preserving content, caret, scroll, undo where
  possible; re-route LSP/ghost language IDs with the new type.
- `EditorGroup::reopenLastClosedTab` (`EditorGroup.cpp:573-609`): restore
  tab type (`.ck` vs `.hathor`) + cursor line/col (not raw offset) +
  front-matter/slot.
- `RecentlyClosedTabs.hpp`: snapshot `slotIndex`, tokeniser kind,
  `frontMatter`, scroll offset, pinned flag, workspace root; cap per-snapshot
  bytes (spill to disk for large files).
- **DoD:** `.hathor`→`.ck` Save-As re-highlights + re-routes LSP/ghost;
  reopen preserves type + cursor.

### P2.3 — Tab rendering + activation [B/M]

- `EditorGroup.cpp:640`: fix `toBack()` inversion (active tab to front).
- `EditorGroup.cpp:79-147`: paint from `HathorLookAndFeel` palette, not
  hardcoded greys (theme switch correct).
- `EditorArea.cpp:164-198,278-300`: tab-bar overflow (scroll + `…` menu),
  hover highlight, usable close hitbox, middle-click close, dirty-dot
  overrides `×`, double-click pin.
- `activateTab`: don't steal focus from palette/quick-open; fix bounds math
  vs breadcrumbs/find panels.
- `EnhancedTabBar` drag (`EditorGroup.cpp:185-238`): drag-position (not
  press-position) hit-testing, auto-scroll, drag image, Escape-cancel;
  fix `TabReorderModel` double-apply risk (model owns order, view mirrors).
- `EditorGroup.hpp:649-658`: implement `setTabPinned` or remove the lying
  API; enforce pin in `closeTab`.
- **DoD:** 20 tabs scroll; theme switch clean; drag-reorder + pin behave;
  focus stays where the user put it.

### P2.4 — Eval + close correctness [M]

- Unify Eval Line/Block semantics across `EditorGroup` (`1227-1247`
  swapped) and `EditorArea`/`handleKeyPress` (`789-807`): one documented
  contract (line vs selection-or-line vs whole-document).
- Close-tab async: capture tab identity (ID/pointer), not vector `index`,
  in `EditorArea.cpp:950-1080` and `EditorGroup.cpp:403-527`; unify the
  return contract for async paths.
- `openFile` parity between the two (`EditorArea` vs `EditorGroup:336-401`):
  exists-guard, encoding/BOM/binary guard, size cap, front-matter warning,
  LSP didOpen, `clearUnsavedDot`, no slot stealing without prompt.
- **DoD:** eval matrix test (line/block/whole × `.hathor`/`.ck` × menu/key);
  close-tab race test (reorder mid-dialog closes the right tab).

### P2.5 — Editing behavior [M]

- Bracket matching off the paint path: token-aware (comment/string aware),
  adjacent-or-enclosing, no full-document copy per paint
  (`HathorTab.cpp:498-661`).
- Auto-close (§4.2 exists): add prefs toggle, quote/smart-`"` handling,
  surrounding-char guard, single undo unit with the opener.
- Auto-indent: `}` dedent + electric indent + ChucK `;`/`{` rules, single
  undo step.
- Completion replace (`1356-1393`): UTF-8 safe, prefix-only replace,
  `filterText`/snippet/`additionalTextEdits` support.
- Find/replace panel: dedicated highlight layer (stop colliding with
  bracket match via `setTemporaryUnderlining`), match counts, Enter/
  Shift+Enter navigation, scope + preserve-case + history, focus return,
  null-safe target tracking across tab switches.
- Diagnostics: keep multi-line squiggles (stop dropping at `1597-1598`),
  trailing-edge debounce (reschedule, don't drop), gutter markers +
  Problems navigation integration.
- **DoD:** editing checklist passes (pairs/wrap/skip/undo granularity/
  indent/dedent/find counts/diagnostics under rapid typing).

---

## Phase 3 — Shell performance + layout (~1–2 weeks)

### P3.1 — Off-thread + coalesced sync [M]

- `MainWindow.cpp:776-808`: git status off the 60 Hz message-thread tick
  (worker thread + ~2 s throttle + dirty flag).
- Coalesce the three 60 Hz lambdas (`752-774`) into one sync pass with
  per-concern dirty flags.
- `UITimer.cpp:112-116`: cap drain iterations per tick; no `push_back`
  allocation in steady state; fix `reinterpret_cast` object-lifetime UB
  for `Event<ParamMap>`; skip `updateSamples→repaint` when idle (battery).
- `UITimer`: split view-refresh (60 Hz) from background polling (slow);
  exception-guard each hook so one throw doesn't abort the rest.
- **DoD:** idle CPU + repaint rate measured down; producer-flood test can't
  starve the message thread; git status never blocks typing.

### P3.2 — Panel system with sash [M]

- Replace exclusive-by-accident bottom stacking
  (`EditorArea.cpp:1120-1198`) with a real panel host: tab strip
  (Problems/Terminal/Search/Source/Debug), resize sash, persisted heights.
- Status messages: queued with priority (errors not overwritten by info);
  own the timer via `unique_ptr` (`EditorArea.cpp:309-326` leak).
- Z-order: explicit layer stack replacing the `toFront` ping-pong
  (`MainWindow.cpp:926-955`); new overlays register a layer.
- `welcomeScreen_`: destroy on dismiss, not hide (`MainWindow.hpp:374`).
- **DoD:** multiple panels coexist with tabs + sash + persistence; overlay
  z-order deterministic; no hidden-component leaks.

### P3.3 — Keyboard-first shell [M]

- Global shortcuts that survive editor focus (command-target chain, not
  just `MainWindow::keyPressed`); Esc dismisses palette/dialogs; wire
  Cmd+P / Cmd+Shift+P / Cmd+T at the top level.
- `ActionRegistry.cpp:86-96`: remove stale `keyToId_` on rebind; `dispatchKey`
  returns false when no callback installed; conflict detection surfaced.
- Activity ribbon: keyboard focus + arrows/Enter, tooltips with shortcuts,
  accessible names; Settings active-state distinct from `Panel::None`
  (stop overloading the sentinel, `MainWindow.cpp:303-383`).
- Status ribbon: keyboard-activatable items, `GitState` enum (no-repo vs
  detached vs loading), dB-mapped gain, overflow/elision rules.
- **DoD:** full keyboard run (switch panels, palette, recent, terminal)
  without a mouse; no duplicate/conflicting bindings.

### P3.4 — Breadcrumbs + splitters + explorer UX [M]

- Breadcrumbs: last-3 + `…` collapse with dropdown, theme colors, `›`
  separators, current-file emphasis, hover cursor, right-click (copy path,
  reveal); folder crumbs navigate, never `openFile` on a directory
  (`BreadcrumbsBar.cpp:120-142` correctness bug).
- `SplitterBar`: orientation-aware axis (fixes visualizer Y-drag,
  `SB-1`); 6 px hit target + hover affordance; double-click reset wired;
  keyboard resize; persist on drag-finish (kill-safe).
- Explorer: single-click preview + double-click pin (resolve the
  click-vs-open contradiction, `ET-1`); preserve expansion + scroll across
  `refresh()`; hide the redundant root level; multi-select groundwork;
  context menu gains Open-to-Side / Copy-Relative-Path / Open-in-Terminal;
  platform-correct Reveal string; validate create/rename input and check
  op results with error toasts.
- DnD: hover overlay (`fileDragEnter`), accept `.wav/.md/.json/.txt`
  (open or reveal, never silent-drop), coalesce multi-folder drops into
  one prompt.
- MRU: escaped storage (newline-safe paths), missing-folder UX, cap
  constant shared between writer and menu; unregister stale
  `workspace.openRecent.N` actions on refresh.
- **DoD:** breadcrumb/splitter/explorer/DnD/MRU checklist passes on small
  and narrow windows; layout + sizes survive kill -9.

---

## Phase 4 — Search, navigation, language intelligence (~2 weeks)

### P4.1 — Async search + replace safety [B/M]

- `WorkspaceSearchModel`: cancel token + progress + incremental
  `onFileResult` streaming; paging contract (`maxResults` defined +
  continuation); per-file language for the `searchInComments` flag; single
  source of truth for searchable extensions with `ExplorerFileTypes`.
- `replaceInFile`: dry-run diff preview, encoding preservation, binary
  skip, undo grouping.
- Panel: ripgrep-style streaming UI, match counts, go-to-match, replace
  preview list.
- **DoD:** repo-wide search stays responsive with cancel + progress; bulk
  replace previews before touching disk.

### P4.2 — Symbol navigation [M]

- `SymbolSearchModel`: debounce + `requestId` (stale LSP responses
  discarded); LSP/metadata dedup; ranking contract; one 0/1-based
  convention with conversion helpers at boundaries.
- Panel: remove input charset restriction; fix double-click row targeting;
  `keyPressed` consumes handled keys + PageUp/Down/Home/End; stop full-
  catalog dump on open; theme colors; fuzzy highlight of matched chars;
  kind icons.
- QuickOpen (`QuickOpenDialog.hpp`): async indexed scan with `.gitignore` +
  ignore-list respect; scored fuzzy match with highlight ranges;
  multi-token queries; preview-on-highlight; MRU ranking.
- Go-to-Line: `:col` / `line:col` parsing, `@symbol` / `#file` prefixes,
  live preview.
- `NavigationHistory`: dedup/coalesce (navigations, not caret moves);
  fix UTF-16 column round-trip; persist per-file cursor memory.
- Peek: inline (non-modal) widget with read-only highlighted preview,
  history, reuse for references/hover.
- **DoD:** Ctrl+P / Ctrl+T / Ctrl+G matrix passes; back/forward never
  spams; peek doesn't block editing.

### P4.3 — LSP protocol correctness [M]

- `LspJsonRpc`: string-ID support; `CompletionList`-vs-array tolerance;
  `shutdown`/`exit`/`$/cancelRequest` serializers; incremental
  `didChange` (or document the full-sync choice honestly); request
  cancellation + version-stale drop in `HathorLspClient` (+ restart/crash
  recovery with backoff, cancel-all on stop).
- `LspMessageFramer`: spec-strict headers (no bare `\n\n`), `Content-Type`
  handling, overflow-checked lengths, no buffer wipe on malformed headers,
  O(1) consume + size cap.
- `LspProtocol`: complete `CompletionItemKind` 1–25, `filterText`/
  `textEdit`/`insertTextFormat`/`tags`, diagnostic `relatedInformation`/
  `tags`, UTF-16↔UTF-8 position helpers.
- Bridges: `LspContextBridge::completionsAt/hoverAt` must not block the MCP
  worker (async or snapshot); normalize URI keys; thread-safe client
  handoff. `EditorContextBridge`: debounce + delta snapshots (not full
  text per keystroke), `shared_ptr<const Snapshot>`, version sequence,
  no dangling `HathorTab*`.
- Popup: fuzzy scoring, no-suggestions state, screen-edge clamping, docs
  panel, focus ownership documented, dedup.
- Hover: markdown/code rendering, actionable links, viewport clamping,
  sticky-while-hovered, keyboard dismiss, capped height + scroll.
- **DoD:** protocol torture test (string IDs, array completions, unicode
  positions, rapid typing, server crash) clean; no MCP stalls.

### P4.4 — Ghost lifecycle [M]

- `GhostLlmClient`: real timeout + client-side cancel; per-URI version map
  (multi-tab skew fixed).
- Trigger: idle delay before first request; don't kill ghost on tabs
  without LSP; `callAsync` debounce replaced with a timer (no message-
  queue flood on arrow-hold).
- `CompletionCoordinator`: `GhostPending` spinner state; safe ordering
  between `onDocumentChanged` clear and partial-accept path; document the
  triple-revision invariant (or collapse to one).
- Overlay: wrap/scroll/font-change sync, multi-line support, theme hook,
  whitespace-only suppression, screen-reader exposure.
- `GhostTriggerPolicy`: multi-line block-comment/string awareness; Unicode
  identifier safety; byte-vs-UTF-16 offset fix.
- **DoD:** ghost matrix (idle/typing/arrow-hold/multi-tab/rapid accept)
  behaves; no floods, no skew, no lost accepts.

---

## Phase 5 — Agent/chat quality (~1–2 weeks)

### P5.1 — Prompt + input reliability [B/M]

- `ChatThread.cpp:515-545`: never silently drop prompts — queue while
  disconnected with visible pending state, or disable Send with a reason.
- Multiline input (Shift+Enter), paste handling, history (Up/Down),
  `@file`/`#symbol` mentions, stop-generation button; raise or justify the
  2048-char cap (warn, don't truncate silently).
- Scroll: stick-to-bottom only when already at bottom; "new messages ↓"
  pill otherwise.
- Markdown + code rendering, copy buttons, token/cost indicator.
- Permission prompts: single owner (resolve the dual-timer race in
  `PermissionPromptComponent` vs `AcpAgentSession`); show diff/command
  preview; allow-always/deny-always; keyboard accelerators 1/2/3;
  focus trap; queue (don't drop the second request); unclip at any option
  count (drop the fixed `kPermissionH=120` assumption).
- Tool calls: collapsible args/result, elapsed time (stop discarding
  structured data).
- **DoD:** offline-then-send delivers on reconnect; permission storm
  queues correctly; long code answers render + copy.

### P5.2 — Session lifecycle [M]

- `AcpAgentSession::stop()`: async — never block the message thread
  (cap join, move to worker, callback on done).
- Join the MCP server thread on failure paths (`onStartFailed` leak);
  session-owned permission timers (no bare-`this` detached threads);
  fix id collision between init and prompt requests; fd ownership race
  with `stop()`; `SUN_LEN` sizing; stderr tail without 512 B splits;
  accept-loop close-wakeup so `stop()` can't hang.
- `ThreadConnState`: add `Connecting` initial + `Failed` distinct from
  `Disconnected`; backoff/retry count; allow manual restart of healthy
  sessions; fix `onDisconnect` clearing errors; document all edges.
- Chat sidebar: `+` affordance + shortcut + empty state; preserve focus
  across tab add/close/activate (stop full `buildTabButtons` rebuilds);
  persist per-tab agent paths; cap thread count (no unbounded
  subprocesses); basename collision fix; quote args with spaces; busy
  state on reconnect.
- `AgentRegistry`: validate merged JSON (non-empty argv, `isBundled`
  not user-settable, no path traversal); atomic load (no half-merge on
  corrupt file); `~` normalization; consistent identity with sidebar.
- `AcpAgentPath` vs `splitCommandLine`: one quoting implementation
  (paths with spaces work identically everywhere).
- History view: virtualize bubbles (or cap DOM cost), distinct User/Agent/
  Tool visuals with avatars + timestamps + actions, coalesced reflow per
  frame (not per token).
- **DoD:** kill-agent-mid-session → readable error, reconnect works, no
  leaked threads/sockets/fds; 10-thread stress relays cleanly.

### P5.3 — Provider config hardening [M]

- `GhostProviderConfig`: warn on `tlsSkipVerify`; stop sending OpenAI keys
  to local servers (explicit per-backend token mapping); consistent
  backend/model defaults; `parseBool` yes/on; clamp context window both
  ends; surface write errors; delete file on clear; XDG paths on Linux.
- Header honesty: document timeout values, expose pending-count for a UI
  spinner, `docVersion` per document.
- **DoD:** provider matrix (Ollama/TGI/HF/empty) resolves sanely; every
  failure surfaces in Settings, never just stderr.

---

## Phase 6 — VCS, terminal, debug (~2 weeks)

### P6.1 — Source control correctness [B/M]

- `GitRepository`: `setRepoPath` off the message thread; resolve the
  `runGit` locking contradiction (one documented discipline); throttle +
  cancel async ops (no unbounded detached threads); single-`git diff`
  instead of N+1 spawns.
- Parsers: preserve spaces in porcelain paths; drop duplicate flags;
  graph parsing that survives `*` in messages; real hunk/line-number
  parsing in `parseLineDiff` (headers aren't content).
- Fix `getMergeStatus` data race; trim `headSha_`; `unstageFile` via
  `git reset`, not `git rm --cached`.
- Panel: selection-aware stage/unstage/discard; separate models per list;
  fix `setVisible` timer inversion; queue (don't drop) refreshes;
  non-static tick counters; History layout that sizes all four panes;
  `selectedCommit` setter/getter confusion fixed; branch ids stable across
  remote refs; row mapping correct with both lists populated; staged +
  keyboard-focusable tabs; trimmed commit-message validation.
- Diff view: implement what the header promises (or cut the claims) —
  Myers/LCS alignment, correlation lines, hunk nav, word-level highlight,
  stage-hunk buttons; one renderer (not dual editors + custom paint).
- Graph: colorblind-safe palette, virtualized rows, lane algorithm
  documented, SHA wow/abbrev consistency, hover tooltips, keyboard nav.
- `GitProcess`: join/detach contract, no callback overwrite, cancel path,
  git-missing error, unambiguous argv.
- Problems panel: working collapse, created filter buttons, no-focus-steal
  navigation, no selection yank on refresh, row counts that count problems,
  filename disambiguation, no listener leaks, copy-message action.
- **DoD:** commit/stage/discard/branch/merge/history/diff matrix passes
  against a fixture repo including spaces-in-paths and `*` messages.

### P6.2 — Terminal honesty [M]

- Decide PTY vs pipe-runner and say so in the UI. If pipes: stdin
  interaction, ANSI stripping/rendering, O(1) append with cap + trim,
  4 KB-drain backlog fix, task-id without string surgery, implemented
  Up/Down history, mid-run selection changes that don't misattribute
  exit, no focus steal, no emoji-missing-glyph buttons, search + copy-path.
- `TerminalProcess`: marshaling guarantees, `waitForExit` semantics,
  stdin-flag defaults, dropped-bytes counter, fd race fixes, env/cwd
  validation errors.
- `TaskRunner`: `problemMatcher`, `dependsOn`, env/args/timeout/kill,
  `${workspaceFolder}`-style variables, validated commands, stable task
  handles, background `beginsPattern`.
- **DoD:** long-output run stays smooth and complete; tasks match a
  documented subset of `tasks.json`.

### P6.3 — Debugger credibility [M]

- MI/DAP instead of CLI scraping (or scope the CLI driver explicitly);
  async-output-safe parsing; debugger-reconciled breakpoint IDs;
  launch success semantics; concurrent watch labels; error taxonomy.
- Panel: tree/table views with copy/evaluate, validated breakpoint input
  with current-file prefill, exception + conditional breakpoints,
  empty-state for `None`, capped poll without full rerender.
- `DebugOutputParser`: Windows paths, `??`/inlined frames, column capture,
  locals-vs-watch disambiguation, thread/exception/stderr coverage.
- **DoD:** breakpoint/step/locals/watches survive async target output;
  parsers fuzz-tested against LLDB + GDB corpora.

---

## Phase 7 — Polish + accessibility + release bar (~1–2 weeks)

### P7.1 — Theme-complete custom paint [M]

- `HathorLookAndFeel.cpp`: `drawScrollbar` honors orientation/thumb/hover/
  pressed/min-size; `drawLinearSlider` honors style/vertical/disabled/
  focus/two-value; buttons honor background/disabled/focus/connected
  edges with ellipsis + accessible names; fill missing colour IDs
  (Toggle, Progress, TableHeader, CodeTokeniser…); fix light-accent text
  contrast (`textColourOn`, highlighted-text); bold+italic font paths;
  null-safe typeface loading; batched/debounced palette application.
- `IconLibrary`: bounded cache with theme-change eviction; `fill=` +
  quote/`style=` tint variants; verified `#AARRGGBB` acceptance;
  missing-glyph placeholder (never silent blank); `JUCE_ASSERT_MESSAGE_THREAD`;
  stronger cache key; distinct FileHathor/Music glyphs; expanded set
  (save, undo, diff, run, extensions, accounts) + a11y label map.
- `SettingsComponent`: responsive layout (no absolute 600 px), tab order,
  no leaks on rebuild, no disk-clobber of just-applied fields, UI-surfaced
  errors, theme-aware repaint.
- `WindowAppearanceController`: validated ranges, error returns, no raw
  window escape, refreshed caps.
- `SliderPanel`: labeled text entry, atomic/RAII dispatch guard, throttled
  gain dispatch, correct 120-default edge, min heights, theme repaint.
- Visualizer: read the *most recent* ring window; attack/release smoothing
  + dB scale (no pumping); power-of-two asserts; idle "no signal" state;
  no negative bar widths; `running`-gated idle flag; beat-sync use of
  stored cycle/bpm or removal; 60 Hz repaint only when visible + active.
- Runtime inspector: accessible list/table with copy + sort; no layout
  loop; truncated values; full worker states; click without message-thread
  block; labeled close.
- Bake dialogs: enforced stage order, thread-safe progress, retry + show-log
  on fail, no auto-dismiss while reading, non-modal target picker with
  overwrite confirm + path preview + disk-space check + remember-choice,
  concurrent-bake correlation IDs (fix single-slot `BakeOrchestrator`
  clobbering; stop extending `juce::Component` for a non-visual class).
- Pet/Petdex: HiDPI scaling, bounded sprite memory with downscale, disk I/O
  off the message thread, atomic cache writes + quota/eviction + locking,
  manifest ETag/retry/backoff/cancel, per-selection cancellation (no wasted
  bandwidth), integrity hashes, size-capped decode with animated-WebP
  policy, error codes (not strings), forward-compat frame geometry,
  strict attribution gate + clickable credit, i18n-safe strings.
- `.hathor` parser: BOM/case/comments tolerance, validated color/slot/bank,
  coded errors, duplicate-key rule, grapheme-aware label limit,
  precision-preserving serialise.
- **DoD:** theme × DPI × narrow-window matrix passes; no silent blanks,
  no leaks, no message-thread disk I/O in these paths.

### P7.2 — Accessibility + keyboard + i18n [M]

- Accessible names/roles for ribbon, tabs, lists, dialogs, bubbles,
  sliders, graph, visualizer (text alternative for level/beat), ghost
  (screen-reader exposure), permission countdown.
- Full keyboard maps: panel switching, list nav (arrows/PgUp/PgDn/Home/End),
  slider resize + value entry, dialog focus traps + Esc, Shift+F10 menus.
- Color-only signals get text/shape seconds (explorer status dots, PET
  status, diff severity).
- Focus-visible rings; minimum hit targets (close ×, splitter 6 px,
  tab close).
- Externalize user-visible strings (at minimum: errors, dialogs, menus,
  shortcuts doc generated from the registry so `onOpenShortcuts` can't
  drift again).
- **DoD:** keyboard-only run of open/edit/search/commit/chat; screen-reader
  pass names every control; no color-only meaning.

### P7.3 — Performance budgets + release bar [M]

- Budgets: idle CPU, keystroke-to-paint latency, 10k-line file scroll,
  repo-wide search time, 200-message chat scroll, large-output terminal
  run, 60 s soak (no growth in threads/fds/memory).
- Fix what the budgets find: tokeniser cache + allocation discipline,
  `getAllContent` copies off hot paths, snapshot/delta discipline in
  bridges, registry lookup allocs, framer O(1) consume.
- Shutdown: ordered teardown (timers → threads → processes → UI), no
  joins on the message thread, no hangs (manifest destructor cancel,
  MCP/ACP joins bounded).
- Release checklist: Debug + Release `HATHOR_BUILD_APP=ON` clean,
  `hathor-ui-tests` + full `ctest` green, ASan/TSan clean on stress
  loops, version string single-sourced (kill the hardcoded "2.3"),
  shortcuts doc regenerated, help links valid.
- **DoD:** budgets recorded in-repo and met; release checklist all green.

---

## Appendix — where the audit's exit criteria stand

| Criterion | Status after Waves 0–5 + this plan's Phase 1 |
|-----------|-----------------------------------------------|
| 1. Welcome → Open Folder → eval, no CWD dependence | Needs P1.4 (New Project) + P1.5 (veto) |
| 2. Two real ACP CLIs end-to-end, readable errors | Needs P5.2 lifecycle hardening |
| 3. Zero letter/emoji/tofu; font split | Done (verify in P7.1) |
| 4. Real waveform/spectrum + step grid | Needs P7.1 visualizer fixes |
| 5. Explorer CRUD + resize + menu + DnD | Needs P1.5 + P3.4 (trash, DnD types, veto) |
| 6. Chat/workspace/layout survive restart | Needs P1.5 (fail-open) + panel persistence (P3.2) |
| 7. Icon squircle inset | Done (verify at all dock sizes) |
| 8. Green build, no warnings | Done, must hold every subphase |
