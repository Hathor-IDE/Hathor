# Hathor — Phase 2.5 Program (merged)

**Source of truth for Phase 2.5 and beyond.** This single document defines the decisions, phases,
acceptance criteria, and execution order. Follow phases **in order**; within a phase, run listed
items **in parallel** unless a dependency is noted. Do not re-litigate a decision in this document
while an item is in progress — a change requires a new decision recorded here.

This is the **canonical, merged** version: it supersedes both the original Phase 2.5 program and the
`addition?` revision, folding in the settings-as-tab decision, the Apply/Reset edit-buffer model,
and the real ChucK / real EQ scope correction (decisions #8–#10, B4, B7 below). Where a revision
and this document differ, this document wins. The standalone `addition?` file has been merged in
and removed. **Before starting Phase 0, ensure the working tree matches commit `5ba7710`** (git
pull or reset if it drifted) — all `file:line` citations and audited gaps assume that exact
state.

**B4 (Real ChucK, V2), B7 (Real EQ), and B8 (Bake → asset lifecycle) now each have a fully-decided,
direct-implementation design** (decisions #11–#13; §Phase C/D/E below). B4 adopts the V2
architecture in the dedicated V2 Architecture section (decision #12: per-tab VM isolation,
out-of-process worker, Tidal-as-master event queue, bake-to-asset lifecycle). Each has been
promoted from a single `own-pass` item into **its own sub-phase** — every sub-item is a
single-agent-sized task. Their earlier "own-pass: yes" markings are **superseded**: do not run a
further requirements→design→tasks round; implement directly from §Phase C (B4) / §Phase D (B7) / §Phase E (B8)
and the V2 Architecture section.

**Phase H–K (AI Authoring & Hathor MCP v2)** is its own program phase (decision #14), building a
canonical application contract + canonical language metadata (UI / MCP / AI-authoring / tests all
share one model), then read-only intelligence, authoring intelligence, safe mutations, ChucK
lifecycle, rendering, and finally inline AI completion (Phase I/J) + the agentic workflow
(Phase K, AI-10), two
tracks). Track B explicitly splits **deterministic language intelligence** (Strudel LSP + ChucK
compiler + versioned metadata) from **AI ghost-writing** (llm-ls/FIM, project-aware context,
few-shot, inline ghost text) — complementary systems that must not be collapsed into "LSP support"
(decisions #19–#21; §AI-G1…AI-G9).

**Phase L (Traditional IDE Foundation)** completes the non-AI IDE experience around the native
JUCE editor — editor/workspace ergonomics, navigation/search, unified diagnostics, tasks/terminal,
Git source control/history/graph, debugging & Hathor runtime inspection, workspace/session
persistence, and unified contextual actions. It consumes Phases H–K rather than adding a second AI
subsystem, keeps all UI JUCE-native, and runs off the real-time audio thread.

**B4's cross-process audio architecture is tightened (decisions #22–#24):** timestamped musical
events (delivery timing ≠ execution timing), a pre-implementation two-process audio IPC spike
(**B4-K0.6**) that must *prove* RT-safety, worker-crash/shared-memory recovery, and a bounded
per-tab VM resource policy. The audio IPC spike gates B4-K2+.

Status marker per item: `[ ]` not started · `[~]` in progress · `[x]` done (verified).

---

## 0. How to read this document

- **Stable IDs** (`H1`, `A1`, … `D4`) are the canonical way to refer to a work item. Use them in
  commit messages, PRs, and the tracking index (§7). Do not renumber.
- **Size** is relative to a Phase-1/2 task: `small` / `medium` / `large`.
- **Subsystem** = the layer(s) an item touches: `engine` / `audio` / `control` / `ui` / `new` /
  `docs`.
- **Acceptance** is written in the same testable `SHALL` style as the Phase 1/2 requirements; it is
  the Definition of Done.
- **Own-pass** = "Needs its own requirements→design→tasks pass" (like Phase 1/2 each received). If
  set, do not begin implementation from this doc alone.

---

## 1. Audit baseline (captured 2026-08-07; current source @ `5ba7710`)

Ground truth for what exists vs. what is missing, established by reading the code (not the Phase-2
docs). Where a doc claims a feature but the code does not wire it into the app, the code wins.

> **Line-reference caveat:** the `file:line` citations in this audit are a snapshot at commit
> `5ba7710`. H0/H1 directly touch `MainWindow`, `ExplorerPanel`, and `AcpAgentSession`, so these
> references (and any later feature that shifts those files) will drift once Phase 0 ships. Before
> implementing any item, **re-verify its file:line citations against the current source**; do not
> treat the audit's citations as still-accurate after Phase 0 has landed.

| # | Feature | Status (audited) | Evidence |
|---|---------|------------------|----------|
| 1 | Play/Stop per tab | **Not present.** `play`/`stop` exist only as **global transport** commands; never called from UI; not per-slot | `ControlInterface::handlePlay/Stop` → `audio_.play()/stop()` (`control/ControlInterface.cpp:124-126,198-208`); no per-slot API in `AudioEngine`/`AudioEngineFacade`; slots are passive buffers on one global clock |
| 2 | Now-playing highlight | **Not present.** Token byte offset exists but is dropped at the AST; `Event<ParamMap>` carries no source/step field; no editor consumer | `Token.pos` (`engine/include/hathor/MiniTokeniser.hpp:43-47`); `MiniAtom` is `{string token}` only (`engine/src/MiniAst.hpp:32`); `Event={Arc whole,active;ParamMap}` (`engine/include/hathor/Event.hpp:24-28`) |
| 3 | Recursive Explorer tree | **Not present — and the flat Explorer today is dead code** (never wired) | `ExplorerPanel` is `#include`d only (`ui/MainWindow.hpp:49,134`); no member in `MainWindow.hpp:138-151`; flat single-dir `.hathor` list (`ui/ExplorerPanel.cpp:53-74`) |
| 4 | `.ck`(ChucK) support | **Not present.** Editor has no extension recognizer | always `MiniNotationTokeniser` (`ui/HathorTab.cpp:21`); save filter `.hathor` only (`ui/EditorArea.cpp:371`) |
| 5 | Settings child window | **Not present.** No settings window at all | agent path via CLI/env only (`ui/HathorApplication.cpp:59-73`); only window-bounds + agent-path persisted (`ui/MainWindow.cpp:193-228`) |
| 6 | Theme system | **Not present.** Colors are compile-time constants | `static constexpr` in `HathorLookAndFeel::Colours` duplicated per component (see §4) |
| 7 | Window opacity | **Not present** | no setting/rendering; JUCE matrix in §B5 |
| 8 | Chat threads | **Not present** — single conversation; one subprocess per session | `ChatSidebar` one history (`ui/ChatSidebar.cpp:301-336`); `AcpAgentSession` one `subprocess/sessionId_` (`ui/AcpAgentSession.cpp:434-506`); `session/update` carries no turn id (`ui/AcpAgentSession.cpp:657-707`) |
| 9 | Petdex mascot | **Not present.** Manifest verified live (§D audited facts) | 4425 pets, 7 fields; `spritesheetUrl` is WebP; no license field |

### Pre-existing Phase-2 gaps discovered (block, Phase 0)

- **G1 (→H0):** nothing in `app/` or `ui/` ever runs `accept()` on the Unix socket, so agent MCP
  tool calls dead-end and never reach `ControlInterface`. (`accept()` appears only in the spike
  test `tests-ui/test_acp_spike.cpp:364`; `AcpAgentSession` binds+listens but has no accept loop,
  `ui/AcpAgentSession.cpp:268-291`.)
- **G2 (→H1):** `ExplorerPanel` and `ActivityRibbon::onPanelToggled` are unwired
  (`ActivityRibbon.hpp:55` has the callback member but nothing assigns it; `MainWindow` never
  instantiates the explorer). E/S/V/A and the Settings button do nothing today.

---

## 2. Decisions (inviolable unless re-decided)

| # | Decision | Consequence / reference |
|---|----------|--------------------------|
| 1 | Do **not** create `docs/deviations-from-strudel.md` (current behavior is Strudel-exact, so zero deviations). **Do** create `docs/future-chuck-integration.md` as item **A6** (a tiny task — it was discussed but never written). | None. |
| 2 | Play/Stop = **true independent per-slot** start/stop (matches Tidal's d1–d16 orbit model). Requires engine work + new `slot-play`/`slot-stop` command — not a wrapper over the global transport. | Drives A3 → B1 |
| 3 | Chat threads = **one subprocess per tab** (simpler, correct). Revisit single-process multiplexing only if per-tab cost proves real. | Drives B6 |
| 4 | Each theme defines its **complete token set explicitly** (current + Purple/Neon, Capuchin, Sand, Light). Light is not derived from dark assumptions. | Drives A1 → B3 |
| 5 | **No default mascot.** Opt-in only; a pet is fetched/cached after explicit selection, which avoids the per-asset licensing question for the default case. | Drives D1–D4 |
| 6 | `.ck` tabs: **recognized and syntax-highlighted from A5 onward; eval is a real, functioning
      feature — never a silent no-op, never a disabled-with-tooltip stub.** Superseded by decision
      #9: ChucK is built for real. Eval cannot work, it must say why; otherwise it executes. | Drives A5 → B4 |
| 7 | **Settings is a tab, not a modal window.** It opens in the same Tab_Bar as `.hathor`/`.ck`
      tabs (focusable/closable like any tab), with a dedicated settings component as its content
      instead of a `CodeEditorComponent`. The Ribbon Settings button opens/focuses it. **Why this
      changed from the earlier "child window" plan:** a tab keeps Settings consistent with the
      IDE metaphor and lets it be persisted/switched just like a working file, and avoids a
      separate window/lifecycle. Supersedes the A2 "Settings child window" wording. | Drives A2, B3, B5, D1 |
| 8 | **Settings persistence model:** an edit buffer distinct from applied/persisted state.
      **Apply** commits the edit buffer live + persists via `ApplicationProperties`. **Reset**
      discards in-progress edits back to last-applied (NOT factory defaults — a factory-reset, if
      wanted, is a separate clearly-labeled third action). **Closing the tab without Apply**
      discards in-progress edits, same as Reset; whatever was already Applied remains in effect
      and persisted. Reopen always shows applied/persisted values, never stale discarded edits. | Drives A2 |
| 9 | **ChucK and EQ are both REAL, functioning features for Phase 2.5 — not stubs, not
      disabled-with-tooltip.** This is a correction of the earlier default-to-safe scoping (the
      original program's decision #6). Both are now fully specified as direct-implementation
      designs in §Phase C (B4) (ChucK) and §Phase D (B7) (EQ), replacing any earlier own-pass requirement
      (decision #11). Overrides decision #6's original "eval disabled" wording. | Drives A5/A6→B4, B7 |
| 10 | **Opacity default: 70% on macOS/Windows; 100% opaque fallback on Linux** (transparency is
      unreliable there — feature-detect and degrade gracefully per B5). | Drives B5 |
| 11 | **B4 and B7 are now direct-implementation designs, promoted to their own sub-phases — as
      one agent per sub-item.** No further own-pass round. The B4 architecture described in this
      row is **superseded by decision #12** (per-tab VM isolation, out-of-process
      `hathor-audio-worker`, Tidal-as-master event queue). B7 stands as stated in §Phase D (B7). | Drives §Phase C (B4), §Phase D (B7) |
| 12 | **ChucK execution adopts the "V2" architecture — see the dedicated "V2 Architecture"
      section below.** Hathor executes arbitrary AI-generated code, so it needs a stronger trust
      boundary than a conventional ChucK host. Concretely: **per-tab `Chuck_VM` isolation** (own
      thread, own watchdog, own lifecycle per active `.ck` tab — one tab's failure never silences
      another); **out-of-process execution** via a small companion `hathor-audio-worker` process (a
      native libchuck crash cannot take down the main process); an explicit **B4-K0.5 libchuck
      concurrency spike** before K2+ builds on the compile/run thread-safety assumption; and
      **Tidal as master musical conductor** — the Phase-1 pattern scheduler emits an explicit
      **timestamped** ChucK event queue (Note On/Off, parameter changes, instrument/control
      triggers, with authoritative sample/frame timestamps; the worker schedules by timestamp, not
      by IPC arrival — decision #22) instead of relying on polling BPM alone. | Drives V2 Architecture, §Phase C (B4) |
| 13 | **Master-bus audio chain order is fixed: `Master EQ → Final Master Gain → Output`** (the B7
      master filters are the last signal-shaping stage; master gain is applied after them). An
      architectural constant, not left to individual implementers. | Drives B7-K2 |
| 14 | **AI tooling is its own program phase (§Phase H–K), built on a canonical application
      contract.** MCP must NOT accumulate its own model of Hathor. First define one canonical
      internal API/command layer (callable by UI, MCP, AI-authoring, and tests); then build the
      MCP read surface, authoring intelligence, safe mutations, ChucK lifecycle, and rendering on
      top of it. Two explicit tracks: **Track A — Agent Interface** (MCP v2, project inspection,
      diagnostics, ChucK lifecycle, rendering, asset management, safe song editing) and
      **Track B — Authoring Intelligence** (canonical Hathor/Tidal/mini-notation/ChucK language
      metadata, project-aware context, deterministic completion first, inline AI completion last).
      See §Phase H–K. | Drives §Phase H–K |
| 15 | **Deterministic authoring intelligence precedes LLM generation, and no custom LSP is built.**
       The Hathor language metadata + deterministic completion (AI-3, AI-4) precede any inline AI
       completion (AI-9). Don't make an LLM the autocomplete engine for basic syntax. **Reuse an
       existing Strudel LSP (`strudel-lsp-server`) rather than designing/building a custom Language
       Server** (decision #16). Do not build a `suggest_pattern`/agentic generator up front — that
       comes after the AI reliably understands the language and project. | Drives AI-3, AI-4, AI-9, AI-10 |
| 16 | **Reuse existing language-intelligence implementations; do not build a custom LSP/parser.**
       `.hathor` files are **standard Strudel mini-notation** (reimplemented + differential-tested
       in Hathor's C++ engine), *not* a custom dialect or superset — so the `strudel-lsp-server`
       implementation is a natural fit. Integrate it (subject to §"Verification-first" below) as the
       primary language-intelligence provider for Strudel/`.hathor`: completion, hover, function/
       pattern/sample discovery, diagnostics. Do NOT fork or duplicate its intelligence; only add
       Hathor-specific integration where Hathor's supported surface / project model / editor
       requires it. | Drives AI-3, AI-4 |
| 17 | **LSP and MCP are separate concerns.** LSP is the *language-intelligence service*; MCP is the
       *agent capability/interface layer*. Do not collapse them. Use an existing LSP→MCP bridge
       (e.g. `isaacphi/mcp-language-server` or an equivalent mature one) rather than writing a custom
       protocol bridge. The Hathor editor may consume the LSP directly, and the AI reaches it through
       the LSP→MCP bridge. | Drives AI-3, AI-4, AI-8 |
| 18 | **Language metadata/versioning & ChucK diagnostics authority.** (a) The AI-3 language metadata
       is **versioned** (schema + compatibility identifiers) so editor/AI/docs/validation/MCP always
       know which language/runtime surface they describe — preventing stale definitions when parser,
       Strudel, or vendored libchuck changes. (b) ChucK diagnostics must come from the **actual
       vendored ChucK/libchuck compiler/runtime** API — never an approximate external parser; the
       implementer verifies the real API before coding (no assumed `checkSyntax`). (c) AI-3's
       "exhaustive" means exhaustive for the **supported Hathor surface**, not for all upstream
       Strudel/Tidal/Chuck. | Drives AI-3, AI-5 |
| 19 | **LSP/deterministic intelligence and AI ghost-writing are separate, complementary systems —
       must not be collapsed into "LSP support."** The AI authoring layer owns *probabilistic*
       Cursor-style generation (inline ghost text, Fill-in-the-Middle, multi-token/multi-line
       continuation, musical transformation, ChucK synthesis continuation, AI repair after
       diagnostics) on top of the deterministic layer (Strudel LSP + ChucK compiler + versioned
       metadata). Ghost text is UI state, never document state until explicitly accepted. | Drives Track B, AI-G1…AI-G9 |
| 20 | **Reuse an existing generic LLM-LSP/FIM server (`llm-ls` or equivalent); build no custom LLM
       completion server from scratch.** Verify the selected version's real capabilities/API before
       coding; no hard-coding unverified config/protocol from secondary docs. Fill-in-the-Middle
       (prefix/suffix/middle) is a first-class requirement, not whole-document continuation. | Drives AI-G1, AI-G2 |
| 21 | **ChucK needs the full authoring stack, not just diagnostics.** Reuse a mature existing ChucK
       LSP if one is good enough; otherwise the real libchuck compiler is the authoritative
       diagnostic source and Hathor builds only the minimal deterministic-completion/metadata layer
       it needs — **no bespoke full ChucK language server unless the reuse investigation proves it
       necessary.** | Drives AI-G7 |
| 22 | **Timestamped cross-process events; delivery timing ≠ execution timing.** The Tidal/Hathor
       master clock emits explicitly timestamped musical events over IPC; the ChucK worker schedules
       by the event's authoritative timestamp, never by IPC-arrival instant. "Sample-accurate" is
       defined operationally and *demonstrated, or its measured limit documented* — never silently
       downgraded to "within one buffer" solely because IPC is involved. | Drives B4-K6, B4-K0.6 |
| 23 | **Cross-process audio transport is real only when proven.** A shared-memory ring buffer proves
       nothing by itself; require the pre-implementation two-process IPC spike (B4-K0.6) to
       demonstrate RT-safe read/write, under/overrun behaviour, worker crash/death mid-write, stale
       shared-memory detection, and recovery without hanging the main process — before B4-K2 treats
       the transport as real-time-safe. Control plane and audio plane are separate concerns
       (control = socket/lifecycle; audio = shared-memory ring). | Drives B4-K0.6, B4-K2 |
| 24 | **Per-tab VM resources are bounded and policy-driven, not auto-created per open file.**
       Defining the live-vs-suspended VM policy, "active" meaning, measured CPU/RAM cost per VM, a
       resource-safe ceiling, LRU suspension, resume-without-state-loss, and watchdog handling for
       suspended VMs is a required decision — and the policy must be changeable without rewriting
       core per-tab isolation. | Drives B4-K2, B4-K3 |

Note (decision 1): after my audit, **neither missing doc exists on disk**; `future-chuck-
integration.md` is the only real gap. `deviations-from-strudel.md` is intentionally absent.

### §Verification-first — verify external language-intelligence dependencies before locking them in

> Referenced by decisions #16/#17 and Phase H (AI-3, AI-4, AI-5). **An external dependency is not
> adopted/locked merely because it exists; it must first be verified against the specific Hathor
> surface.** This gate precedes any Commitment to reuse a library/bridge/API and any coding layer
> that hard-depends on it. De-risks assuming capability that the real artifacts don't provide.

Perform as a small spike during Phase H before AI-3/AI-4/AI-5 are built and before the relevant
dependencies are pinned:
- **Strudel LSP compatibility (AI-3/AI-4).** Confirm `strudel-lsp-server` actually accepts
  `.hathor` mini-notation source *as authored by Hathor's C++ reimplementation* — i.e. whether it
  parses the mini-notation directly or expects a different entry point — and what subset it covers.
  Then confirm how JUCE launches/communicates with it (child process, stdio LSP, or stdio/spawn+args),
  its built-in capabilities (completion/hover/diagnostics), and exactly what Hathor must layer on.
- **LSP→MCP bridge (AI-8).** Verify `isaacphi/mcp-language-server` (or the chosen mature
  equivalent) works with the integrated Strudel LSP and exposes the needed language data to the AI;
  otherwise choose a verified alternative rather than writing a custom bridge.
- **Real ChucK compiler API (AI-5/decision #18).** Inspect the **actual vendored libchuck/ChucK
  compiler API in the repo** and use its real supported mechanism for syntax validation/diagnostics;
  do not hard-code an assumed API such as `checkSyntax()`.
- **Editor LSP client threading.** Decide the editor-side threading/lifecycle for running the LSP
  in the JUCE host (where it runs, how it's tied to a session/tab, shutdown semantics) so the
  integration is real, not a stub.
- **LLM ghost-writing server (AI-G1/decision #20).** Verify the selected `llm-ls` (or equivalent)
  version's real capabilities/API — launch, communication, FIM prefix/suffix/middle support,
  cancellation, provider config, latency controls — against the *installed* version before coding;
  do not hard-code unverified fields from secondary docs.
- **ChucK LSP reuse investigation (AI-G7/decision #21).** Investigate whether a mature existing
  ChucK language server is good enough before building any ChucK intelligence; if not, use the real
  libchuck compiler for diagnostics + the minimal metadata/completion layer.
- **Validate, then decide.** If verification shows the chosen dependency cannot cover the supported
  surface, reassess the choice (fall back to Hathor's own metadata-driven layer + incremental
  real-compiler diagnostics) **before** spreading it across AI-3/AI-4/AI-8/AI-G*. Reuse existing
  MCP abstractions (e.g. `hathor-mcp`) wherever possible.

## Phase 0 — Critical fixes (block everything; run alone, then verify by a human)

> These are pre-existing Phase-2 gaps, not new features. Nothing in Phase A may start until both
> are fixed **and manually verified by a human** (a compile-clean build is not evidence).

### H0 — Wire the `hathor-mcp` socket accept loop
- **Status:** `[~]` **Size:** small **Subsystem:** `control`, `ui` **Depends:** none **Own pass:** no
  (Implementation landed: accept loop in `control/SocketServer.cpp`, wired via `AcpAgentSession::mcpServerLoop`,
  commands routed to `ControlInterface::dispatchWithCallback`. Console/stdout + socket tests added under
  `tests/test_mcp_socket.cpp`. Awaiting the **human gate** — manual verification with a real agent calling
  `set_pattern`.)
- **Goal:** agent MCP tools (`set_pattern`, `play`, `stop`, `bpm`, `set_gain`) actually reach
  `ControlInterface`.
- **Acceptance (SHALL):**
  1. The app runs an `accept()`/read loop on the Unix listener created by `AcpAgentSession`
     (`$TMPDIR/hathor-<pid>-<seq>.sock`) and forwards each command to
     `ControlInterface::dispatch()` on the worker thread.
  2. The loop never runs on the JUCE message thread or audio thread (matching the offline Req for
     the worker-thread boundary) and serialises concurrent connections (per-command mutex already
     exists at `control/Commands.hpp:29-48`).
  3. Socket cleanup on teardown is retained (`ui/AcpAgentSession.cpp:153-156`).
- **Manual verification (human, gate):** with a real running agent (Claude Code or Gemini CLI),
  send a chat message causing it to call `set_pattern`; confirm an actual pattern changes the live
  audio/visualizer.

### H1 — Wire `ExplorerPanel` and `ActivityRibbon` (alongside H0)
- **Status:** `[ ]` **Size:** small **Subsystem:** `ui` **Depends:** none **Own pass:** no
- **Acceptance (SHALL):**
  1. `MainWindow` instantiates an `ExplorerPanel` member, adds it, and lays it out in `resized()`.
  2. `ActivityRibbon::onPanelToggled` is assigned to open/close the explorer (and future panels);
     the active-button accent highlight works (`ActivityRibbon.hpp:55-58`).
  3. Clicking a `.hathor` file opens/recovers a tab (reuse `EditorArea::openFile`).
- **Human verification (gate):** launch, click Explorer, the panel appears and lists files; click
  a file → a tab opens.

---

## Phase A — Foundation (parallel after Phase 0 clears)

| ID | Item | Size | Subsystem | Depends | Own-pass |
|----|------|------|-----------|---------|----------|
| A1 | Theme/design-token engine | medium | `ui` | Phase 0 | no (design in §4) |
| A2 | Settings tab (not a window): Appearance / Agent / Petdex + Apply/Reset semantics | medium | `ui` | Phase 0 | no (except EQ — see B7) |
| A3 | Per-slot play/stop (engine + control) | medium | `engine audio control` | Phase 0 | no (design below is complete) |
| A4 | Recursive Explorer folder tree | large | `ui` | H1 | no |
| A5 | ChucK grammar → tokeniser (recognition+highlight) | medium | `ui` | Phase 0 | no |
| A6 | `docs/future-chuck-integration.md` | tiny | `docs` | none | no |

### A1 — Theme engine
- **Goal:** replace the duplexed `static constexpr` colour sites with a single runtime palette
  object that every themed component reads, so B3 can switch palettes at runtime.
- **Note:** the colours live in `HathorLookAndFeel::Colours` (`ui/HathorLookAndFeel.hpp:100-136`)
  and are duplicated per component at `ui/EditorArea.hpp:113-119`,
  `ui/ExplorePanel.hpp:70-75`, `ui/ActivityRibbon.hpp:76-79`, `ui/VisualizerPanel.hpp:132-136`,
  `ui/ChatSidebar.cpp:37-47`. Introduce a full `Palette` value-type + a current-palette holder on
  the LookAndFeel; components read **only** through it.
- **Acceptance:** a grep for `0xff…`/`Colours::` outside the palette definition returns **no**
  per-component uses; switching the active palette instantly reflects across all five zones.

### A2 — Settings tab (Appearance / Agent / Petdex) + Apply/Reset persistence
- **Goal:** settings open as a **tab** in the same Tab_Bar as `.hathor`/`.ck` file tabs (decision
  #7), reachable from the Ribbon Settings/Profile button (wired in H1). Content is a dedicated
  settings component, not a `CodeEditorComponent`.
- **Mechanics (tab behaviour):** the Settings tab appears in the Tab_Bar, can be focused and
  closed like any tab, and shows an Unsaved_Dot-equivalent indicator when it has pending edits.
  Clicking the Ribbon Settings button opens the tab if closed, or focuses it if already open.
  Switching away never closes it; closing it applies the close semantics below.
- **Sections inside the tab:**
  1. **Appearance** — theme picker (the 5 themes from A1/B3); opacity slider (B5, default 70%
     mac/win, opaque on Linux).
  2. **Agent / ACP** — agent executable path (made user-editable here; currently CLI/env-only per
     audit `ui/HathorApplication.cpp:59-73`), plus any other ACP config surfaced by
     `AcpAgentSession`. Persisted via `ApplicationProperties`.
  3. **Petdex** — browse/select a mascot (reuses D1–D4), opt-in only, no default; shows
     licensing/attribution per D4.
  4. **Placeholders** for the ChucK and EQ sections, which land in §Phase C (B4) / §Phase D (B7) —
     present but inert until those items ship.
- **Apply / Reset / close semantics (explicit, unambiguous):**
  - The tab holds **two states**: the *applied/persisted* settings (source of truth; loaded from
    `juce::ApplicationProperties` at startup) and the *edit buffer* (whatever the controls
    currently show).
  - **Apply** commits the edit buffer to live app state (theme/opacity/agent apply instantly) and
    persists via `ApplicationProperties`. After Apply, edit buffer == applied state (no pending
    changes); Unsaved_Dot cleared.
  - **Reset** reverts the edit buffer to the last-applied/persisted values — it is **"discard my
    unsaved edits", NOT factory defaults.** A factory-defaults option is out of scope unless
    explicitly requested as a separate, clearly labeled third action; do not overload Reset.
  - **Close without Apply**: in-progress edits are discarded (same as Reset); the last-Applied state
    remains in effect and persisted. Closing never reverts already-applied settings.
  - **Reopen** (same session or after restart): shows applied/persisted values, never stale edit
    buffer from a discarded session. No Save-on-close surprises.
- **Persistence scope:** theme id, opacity %, agent exe path, project dir, pet selection all via
  `juce::ApplicationProperties` (pattern at `ui/MainWindow.cpp:193-228`).
- **Acceptance:** Settings is a focusable/closable tab reachable from the Ribbon; Appearance,
  Agent, Petdex sections render; Apply/Reset/close behave exactly as specified above with no
guessing; values survive restart; reopening shows persisted state. (ChucK is not implemented
   here — see §Phase C (B4). EQ is not implemented here — it is §Phase D (B7).)

### A3 — Per-slot Play/Stop
- **Design:** add an armed/per-slot running bit to `SlotState` (`app/SlotState.hpp:32-38`) rather
  than the single global `running_` (`app/AudioEngine.cpp:137-147`). Add cmd
  `slot-play <slot>`/`slot-stop <slot>` to `ControlInterface` (existing set:
  `ping/play/stop/quit/list-patterns/bpm/set-gain/clear-pattern/set-pattern` per
  `ControlInterface.cpp:111-149`) and to `AudioEngineFacade`. The bit is `std::atomic`, read/written
  alloc-free on the audio thread.
- **Acceptance:** `slot-stop d1` silences only `d1`; other slots continue; `slot-play d1` resumes;
  visualizer reflects the slot idle/armed state; zero alloc + no mutex in the callback.

### A4 — Recursive Explorer tree
- **Design:** replace flat scan (`ui/ExplorerPanel.cpp:53-74`) with a recursive walk producing a
  folder (album) / file (song) tree, each folder an expand/collapse, per-file-type icon (later on
  `samples` folder asset class) — `.hathor` and (after) `.ck`.
- **Acceptance:** folders expand/collapse; only songs shown as leaves; clicking a song opens a tab
  (default slot via existing `EditorArea`); last directory persisted.

### A5 — ChucK grammar → tokeniser
- **Note:** adapt an existing public ChucK grammar (`forrcaho/vscode-chuck`, itself a port of
  Atom `cjwilburn/language-chuck`), not write new from scratch. We only need a `juce::CodeTokeniser
  subclass mapping ChucK keywords/classes/strings/comments to the existing colour scheme; we will
  **not** track ChucK-grammar-engine semantics.
- **Acceptance:** a `.ck` file renders with correct ChucK highlighting distinct from mini-notation;
  per decisions #6/#9 the `.ck` tab is wired for real eval (B4), never a silent no-op or stub.

### A6 — `docs/future-chuck-integration.md`
- **Goal:** write the deferred doc — ChucK SWOT, the `libchuck` embedding precedent, guarding
  sandbox/clock-sync risks, and the real-time eval path B4 implements (per decision #9). No
  implementation.

See **§4 (cross-cutting) for design answers needed to code A1/A2 correctly**, and the opacity
matrixed in **§B5**.

---

## Phase B — Feature layer (after the relevant Phase-A items)

| ID | Item | Size | Subsystem | Depends | Own-pass |
|----|------|------|-----------|---------|----------|
| B1 | Per-tab Play/Stop button → Visualizer | small | `ui`/`audio` | A3 | no |
| B2 | Now-playing metadata pipeline | medium–large | `engine audio ui` | A3 | **YES — full spec pass (cross-layer)** |
| B3 | Theme picker UI in Settings | small | `ui` | A1, A2 | no |
| B4 | **Real ChucK** — now its own sub-phase; see §Phase C (B4) | large | `engine audio ui` | A5, A6, A3 | no (design in §Phase C (B4)) |
| B5 | Opacity slider (platform-guarded) | small | `ui` | A2 | no |
| B6 | Chat thread tabs (one subprocess each) | medium–large | `control ui` | H0, A2 | no (§B6) |
| B7 | **Real EQ** — now its own sub-phase; see §Phase D (B7) | medium | `engine audio ui` | A2 (for UI), engine infra | no (design in §Phase D (B7)) |

### B1 — Per-tab Play/Stop
- **Goal:** per-tab play/stop control that starts/stops *that tab's slot* (decision #2) and drives
  the existing Visualizer the same as live playback.
- **Acceptance:** the control wires to `slot-play <slot>` / `slot-stop <slot>`; its state reflects
  the slot's armed state (play icon vs stop); clicking drives the visualizer; present on
  `.hathor` & `.ck` tabs (the latter governed by B4 — see that item for eval behaviour).

### B2 — Now-playing metadata pipeline
- **Goal:** carry per-atom source position from tokeniser → AST → `Event` → `VisualizerFrame` →
  `UITimer` → C1's editor highlight, without violating the audio-thread zero-allocation rule.
- **Engine:** add `std::size_t sourceOffset` to `MiniAtom` (`engine/src/MiniAst.hpp:32`), set where
  `Token.pos` is available (`engine/src/MiniParser.cpp:180/184/239/248/256/264/273`); thread
  through `lowerNode` into `Event` as a **small trivially-copyable integer** (never a
  `string_view`/pointer into ephemeral input — lifetime + SBO). Since combinators copy `Event`
  whole, the field propagates freely.
- **Audio→UI:** widen the by-value `Event<ParamMap>` that flows through `VisualizerFrame` + the
  `SpscRingBuffer` value-copy (`app/VisualizerFrame.hpp:166-167,221-222`), **and add a per-frame
  slot id** (the current frame has no slot; the audio loop at `AudioEngine.cpp:366-402` holds the
  slot `i` and event `j` but drops it). All three fixed inline stores
  (`PatternCompiler` InnerBuffer, `AudioEngine` `firedEventStorage` `:359-364`,
  `VisualizerFrame` `eventStorage`) re-size via `sizeof(Event)` automatically — unchanged budget,
  still no heap.
- **Acceptance:** an event's byte offset survives to the UI and maps to the correct glyph (C1);
  the ring remains allocation-free / ≤ ≈8 bytes per event; no allocation in stable-state render
  loop. **This item must have its own full pass before coding** (decision).

### B3 — Theme picker
- **Acceptance:** user selects among the 5 themes (current, Purple/Neon, Capuchin, Sand, Light);
  applies instantly via A1; persists via A2. No rebuild required.

### B4 — Real ChucK (see §Phase C (B4))
- Fully decided design in §Phase C (B4) below. Each sub-item there is a single-agent task. No further
  spec pass. Promotion recorded as decision #11.

### B5 — Opacity slider
- **Platform matrix (JUCE researched):**
  | OS | SUPPORT | Hathor action |
  |----|---------|---------------|
  | macOS | reliable full-window alpha | enable |
  | Windows | reliable via `WS_EX_LAYERED` | enable (use software renderer; avoid D2D/OpenGL alpha bugs) |
  | Linux | unreliable; varies by compositor (X11 w/o compositing; Wayland varies) | feature-detect; best-effort; degrade gracefully, warn |
- **Note (explicit decision):** the default is **70% opacity**, per the product owner's stated
  preference — on **macOS and Windows**, where window transparency is reliably supported via `setAlpha`.
  On **Linux**, where transparency is unreliable per-compositor (X11 without compositing, Wayland
  varies), fall back to **100% opaque** and warn, so the app still launches correctly. The 70%
  default is deliberate, not a compromise for engineering convenience. Implement with
  `TopLevelWindow::setAlpha` + `setOpaque(false)` on mac/win; keep an opaque fallback on Linux.
- **Acceptance:** slider in Settings; default **70%** on macOS/Windows, **100% opaque** on Linux;
  no crash on a compositor-less Linux (graceful message).

### B7 — Real EQ (see §Phase D (B7))
- Fully decided design in §Phase D (B7) below. Each sub-item there is a single-agent task. No further
  spec pass. Promotion recorded as decision #11.

---

## V2 Architecture — ChucK isolation, synchronization & asset lifecycle

> This section is the authoritative design for the "Cursor for DJs" trust model: executing
> arbitrary AI-generated code inside a live music app demands stronger isolation at every
> dangerous boundary than a conventional (trusted-human) ChucK host. Decision #12. Supersedes the
> earlier in-process, shared-VM ChucK picture in decision #11. **Where this section and Phase C (B4)
> disagree, this section wins.**

**Threading / execution:**
```text
Hathor Main Process                              hathor-audio-worker (companion process)
┌──────────────────────────────────────┐        ┌──────────────────────────────────────────┐
│ Tidal / Strudel Pattern Scheduler    │        │   per-tab Chuck_VM (A)   per-tab Chuck_VM (B) │
│ Project / UI / SampleBank           │        │   own thread · own watchdog · own lifecycle  │
└───────────────┬──────────────────────┘        └────────────┬───────────┬───────────────[mon]──┘
                │  timestamped musical event queue (SAMPLE_TS)│           │
                └───────────────▶ Hathor_aacWorker ◀──────────┴───────────┘
                                          │  lock-free SPSC audio/event transport
                                          │  + worker liveness / generation (control plane)
                                          ▼
                                   Hathor Audio (mix engine)
```

1. **Per-tab `Chuck_VM` isolation (decision #12).** Each active `.ck` tab gets its own VM, its own
   dedicated OS thread, its own watchdog, and its own restart/terminate lifecycle. A hung/crashed
   instrument in one tab can never silence a healthy instrument in another — matching the
   independent-stop model Hathor already uses for individual Tidal slots.
2. **Out-of-process `hathor-audio-worker`.** All ChucK VMs run inside a small companion process so
   a native libchuck crash (segment fault, overrun, corruption) never takes down the main process.
   If the worker dies: main detects it, UI reports engine failure, worker restarts, existing Hathor
   state stays intact, and affected ChucK instruments restart independently. The audio transport
   stays lock-free and real-time-safe regardless of the concrete IPC mechanism (validated against
   real JUCE constraints, not assumed prematurely).
3. **B4-K0.5 spike.** libchuck's `compileCode()`/`run()` thread-safety is an unverified assumption
   (B4 billing it as safe). Spike it first (B4-K0.5) before K2+ builds on it; if unsafe, force a
   serialized command path / locking instead of relying on undefined behaviour.
4. **Tidal = master conductor.** The Phase-1 pattern scheduler is the authoritative musical clock.
   It emits **explicitly timestamped** ChucK events (Note On/Off, parameter changes, trigger
   instrument/control) into a ChucK event queue; ChucK does not infer timing by polling BPM. The
   worker schedules each event **by its authoritative sample/frame timestamp**, never by the instant
   an IPC message arrives (decision #22 — delivery timing ≠ execution timing). Keeps clean
   separation: musical scheduling / ChucK execution / real-time audio / UI.
5. **ChucK is an instrument workshop, not a permanent residency.** A live VM only exists while the
   producer writes/prompts/tunes/auditions an instrument. When an instrument is accepted, **Bake to
   Song** renders it to a permanent `.wav` asset and shuts the VM down. Most instruments therefore
   don't linger as live processes.
6. **Worker crash / shared-memory recovery (PART C).** If the ChucK worker dies mid-audio-write, the
   main process must detect *death* (worker generation/session identity + liveness), invalidate the
   shared-memory audio transport, never wait indefinitely on the audio thread, fall back to silence,
   reinitialize/replace shared memory, and restart the worker if policy permits — all RT-safe
   (decision #23). The audio transport carries a generation token so stale/corrupt shared-memory
   state is never interpreted as valid audio.
7. **Bounded per-tab VM resources (decision #24).** No permanent OS thread/VM is auto-created just
   because a `.ck` file is open. A VM is created on Play/Activate and suspended/destroyed on
   Stop/Deactivate per an explicit resource policy (live vs suspended, active definition, measured
   CPU/RAM per VM, ceiling, LRU, resume-without-state-loss, suspended-VM watchdog) that is
   changeable without rewriting per-tab isolation.

**Instrument lifecycle**
```text
write/prompt .ck  →  live ChucK sandbox  →  audition/iterate  →  "Bake to Song"  →  permanent .wav  →  VM terminates  →  song uses ordinary sample
```

**Workspace / asset layout** — `.hathor` = song/pattern logic, `.ck` = instrument source, `.wav` =
rendered asset, `.hathor_assets` = project-managed assets:
```text
My_New_Album/
├── intro_track.hathor
├── main_groove.hathor
└── .hathor_assets/
      ├── external_imports/808_kick.wav
      └── chuck_instruments/acid_bass.ck, acid_bass.wav
```
The raw `.ck` stays (to regenerate/modify later); the `.wav` is what opening the song depends on —
the song never recompiles arbitrary native synthesis on open. The FileTreeDataModel treats
`.hathor_assets` as a managed dir (hidden/collapsed internals, meaningful assets exposed, `.wav`
wired to `.hathor` autocomplete), while the real filesystem layout stays deterministic underneath.

**Studio (default) vs Live Jam (optional):** baked instruments persist as project assets (default
production workflow, survives reopening months later). Live Jam option writes to a
temporary/session location for disposable four-bar risers / transitions / textures, cleaned at
session end.

**Bake to Song (`Ctrl+Shift+B`)** — three stages: (1) render the active ChucK instrument to
`.hathor_assets/chuck_instruments/<name>.wav` via a background audio writer; (2) shut the VM down,
terminate its thread, release runtime resources (no manual process management); (3) bind the asset
to the project's SampleBank and expose it to the `.hathor` editor/autocomplete, so finished song
code is as simple as ``d1 $ s "acid_bass"``.

---

## Phase C — Real ChucK integration (V2, out-of-process, per-tab isolated)

> **Direct implementation.** Design is final (decisions #11, #12; V2 Architecture section). Each
> sub-item `B4-Kn` is a **single-agent task** — one agent runs one sub-item, in order. No separate
> own-pass. This is the **highest safety-risk work in the project** (arbitrary, often AI-generated
> code executes near real-time audio). Hard gate (DoD §6.2 / B4-K8): a hung **or natively-crashing**
> `.ck`/worker must never hang or crash the JUCE audio thread or the rest of Hathor. Dependencies:
> A5 (tokeniser), A6 (libchuck embedding doc), A3 (per-slot play/stop), V2 Architecture, and
> infra for the out-of-process worker.

| ID | Item | Size | Depends |
|----|------|------|---------|
| B4-K0.5 | **libchuck concurrency spike** (compile/run thread-safety) — gates K2+ | small | libchuck vendored |
| B4-K0.6 | **Cross-process audio IPC spike** (shared-memory ring: RT-safety, under/overrun, crash/death, stale-state, recovery) — gates B4-K2 | medium | B4-K0.5 |
| B4-K1 | Audio/event transport: lock-free SPSC sample ring (reuse `VisualizerFrame` pattern) | medium | — |
| B4-K2 | Out-of-process worker: `hathor-audio-worker` process + IPC + survivor/restart + liveness/generation + resource policy | large | B4-K0.5, B4-K0.6 |
| B4-K3 | Per-tab `Chuck_VM` isolation: own thread + per-VM watchdog + lifecycle maps tabs↔VMs; bounded live-VM resource policy | large | B4-K1, B4-K2 |
| B4-K4 | Compile/load `.ck` on the worker (safe/economy thread); handoff to the per-tab VM | medium | B4-K0.5, B4-K2 |
| B4-K5 | Per-VM hang detection / watchdog → per-tab restart | medium | B4-K3 |
| B4-K6 | **Tidal-as-master timestamped event queue** (Note On/Off, params, triggers, seed advance; schedule by timestamp, not IPC arrival) | large | B4-K3, A3 |
| B4-K7 | `.ck` tab eval (`Ctrl+Enter`), status-bar errors, stop/replace shred | medium | B4-K4 |
| B4-K8 | Hard gate: hung-shred + native-crash/worker-death + shared-memory recovery tests | medium | B4-K5, B4-K2 |

### B4-K0.5 — libchuck concurrency spike (thread-safety)
- Minimal isolated test before K2+ builds on one dangerous assumption. Steps: create a VM, `run()` it
  continuously on one thread, call `compileCode()` repeatedly from another thread, exercise the
  exact handoff pattern B4 expects, run under sanitizers where practical, and **record whether the
  API is actually thread-safe** as an architectural decision.
- **If unsafe**, production must use a serialized message/command path (or locking) rather than
  relying on undefined behaviour.
- **Acceptance:** a documented GO/NO-GO recorded in the decisions, plus (if NO-GO) the serialization
  mechanism selected.

### B4-K0.6 — Cross-process audio IPC spike (PRE-implementation; gates B4-K2)
> Decision #23. **A shared-memory ring buffer proves nothing by itself.** The out-of-process
> transport is *not* treated as real-time-safe until this two-process spike demonstrates it on a
> representative supported target. Control plane and audio plane are separate concerns (PART B):
> control = socket/IPC (commands, status, worker start/detect-death/restart); audio = shared-memory
> ring (continuous samples).
- Build a minimal working two-process prototype:
  ```text
  Main Hathor process ── shared memory ──> Audio worker process → audio samples
  ```
  `hathor-audio-worker` (companion) owns the ChucK VM(s); the main process consumes shared-memory
  audio.
- **What the spike must test explicitly (not assume):**
  1. shared-memory audio ring buffer;
  2. cross-process atomic coordination (seq/publish counters);
  3. producer/consumer behaviour;
  4. real-time-safe **reader** behaviour (never blocks, never allocates);
  5. real-time-safe **writer** behaviour;
  6. underrun behaviour;
  7. overrun behaviour;
  8. worker restart;
  9. worker crash during streaming;
  10. worker death mid-write;
  11. stale shared-memory state (must be detected, never read as valid audio);
  12. recovery without hanging the main process;
  13. cleanup/reinitialization/restart of shared memory;
  14. platform assumptions relevant to supported Hathor targets.
- **Do not** use a normal pipe/socket as the audio sample transport merely because it is easier —
  audio goes over shared memory (real-time-safe); control goes over socket/IPC.
- **Control plane vs audio plane (PART B):** they are different concerns and must not be conflated.
- **Acceptance:** a documented PASS/FAIL per the numbered items; only on PASS does B4-K2 treat the
  transport as real-time-safe; any measured limitation is recorded, not hidden.

### B4-K1 — Audio/event transport: lock-free SPSC sample ring
- Lock-free SPSC ring of raw `float` samples, reusing the existing `SpscRingBuffer` template/pattern
  from `app/VisualizerFrame.hpp` rather than inventing a parallel mechanism. Sized ≥2× the audio
  callback size with room for short bursts; overflow drops oldest (ChucK audio is best-effort glue,
  not the master clock). The same ring-family pattern is used for the musical event queue (B4-K6).
- **Acceptance:** push/pop lock-free + allocation-free; producer/consumer on separate threads; the
  consumer (JUCE audio callback) never blocks — missing/underrun samples are silence.

### B4-K2 — Out-of-process worker (`hathor-audio-worker`)
- Establish the companion process that owns all ChucK VM(s), their watchdogs, synth/event input,
  and audio rendering. Main process ↔ worker via two planes: **control plane** (socket/IPC for
  lifecycle, commands, status) and **audio plane** (shared-memory ring, validated RT-safe by
  B4-K0.6). Do not assume the transport is real-time-safe until B4-K0.6 passes.
- **Liveness / generation (PART C).** Give the worker and each audio transport a **generation /
  session identity** so the main process can distinguish a *dead* worker from *stale shared-memory*
  left over from a previous lifecycle. On worker death:
  ```text
  worker crashes ─► main detects death (generation/liveness) ─► audio transport invalidated
       ─► no indefinite read/wait on the audio thread ─► safe silence/fallback
       ─► shared memory reinitialized or replaced ─► worker restarted if policy permits
  ```
  The main audio thread never waits indefinitely for a dead worker. This is a distinct failure mode
  from an in-process thread hang (which B4-K5's watchdog guards); PART C is the process-death case.
  Recovery remains real-time-safe.
- If/when the worker dies: main process detects it, UI reports engine failure, worker restarts,
  existing Hathor state stays intact, affected ChucK instruments restart independently. Same
  "dangerous execution lives outside the primary process" principle used elsewhere.
- **Resource-policy hook (decision #24):** the worker enforces B4-K3's configurable VM/thread
  budget; it does not auto-create a live VM per open file.
- **Acceptance:** worker lifecycle (start/detect-death/generation-track/restart) works; a killed,
  crashed, or mid-write-dead worker never hangs the main JUCE thread; stale shared-memory is never
  rendered as valid audio.

### B4-K3 — Per-tab `Chuck_VM` isolation + bounded resource policy
- Each active `.ck` tab owns one `Chuck_VM`, one dedicated light OS thread (the ChucK thread,
  separate from JUCE audio and worker threads), one watchdog, and one lifecycle; a failure in one
  tab never silences another. Tab↔VM/thread/watchdog mapping is explicit and re-buildable after a
  worker restart.
- **Resource-policy (decision #24) — explicit, not unbounded:** a live VM is **not** auto-created
  merely because a `.ck` file is open. Establish:
  1. Only actively-playing/reval'd tabs get a live VM; idle open tabs get none.
  2. Inactive tabs **suspend** or destroy their VM per policy (prefer deterministic suspend).
  3. Define what "active" means (slot playing / being eval'd).
  4. Measure and record expected CPU/RAM cost per live VM.
  5. Define the expected max concurrent live VMs (resource-safe ceiling).
  6. Define what happens at the ceiling (LRU-suspend idle, or reject with a message — configurable).
  7. LRU suspension is permitted; a suspended tab **resumes without losing state**.
  8. Suspended/destroyed tabs can be re-activated (recreate/resume) safely.
  9. Watchdogs are managed per state (live VM has a watchdog; suspended VM does not need one).
  Choose this lifecycle:
  ```text
  Open .ck tab ─► no VM required necessarily
       Play/Activate ─► create isolated VM + worker resources
       Stop/Deactivate ─► suspend or destroy per resource policy
       Reactivate ─► recreate/resume safely
  ```
  Design so the policy can change **without rewriting core per-tab isolation** (config + measured
  data, not hard-coded per-file threads).
- **Acceptance:** two simultaneously active `.ck` tabs run on independent VMs/threads/watchdogs;
  hanging or stopping one never affects the other; the mapping survives worker restart; the resource
  policy + measured cost/ceiling is documented and enforced — no per-file OS thread for idle open
  tabs.

### B4-K4 — Compile/load `.ck` on the worker + handoff
- Compiling/loading new `.ck` (worker thread where allocation is allowed) per the validated
  B4-K0.5 result; the compiled shred hands over to the target per-tab VM to begin rendering via the
  same atomic-handoff discipline used for pattern hot-swapping
  (`std::atomic_store_explicit`/`std::atomic_load_explicit`, Apple-Clang-compatible as used
  throughout the codebase).
- **Acceptance:** no allocation on the audio/render threads; a loaded shred starts on the next
  loop iteration of that tab's VM.

### B4-K5 — Per-VM hang detection / watchdog
- The per-tab thread increments `std::atomic<uint64_t> chuckHeartbeat_` once per rendered block. A
  low-frequency checker (a `juce::Timer` or small watchdog thread on the worker) flags a tab whose
  heartbeat stalls ~2 s (a shred looping without `now +=>` — a real, expected failure for
  arbitrary/AI-generated ChucK). On detection, tear down + restart **that tab's** VM on a fresh
  thread (blunt, simple, bounded; no surgical single-shred kill). Surface a clear UI message
  ("ChucK engine restarted — a shred stopped responding"), never silently.
- **Acceptance:** a hung shred in one tab is detected in ~2 s and only that tab restarts; the JUCE
  audio thread never hangs.

### B4-K6 — Tidal-as-master timestamped musical event queue (deep sync) (PART D)
- The Phase-1 pattern scheduler is the authoritative clock (Tidal master). Tidal events produce
  explicit ChucK events: Note On / Note Off / parameter changes / instrument triggers / scheduled
  control changes. These flow into a ChucK event queue as **timestamped** musical events; ChucK
  does not infer timing by polling BPM. It delivers "a riser rising over the next 16 bars aligned to
  BPM" natively. Respect the clean separation: musical scheduling | ChucK execution | real-time
  audio | UI/control.
- **Timestamped events; do not depend on IPC arrival timing (decision #22).** Events carry an
  explicit audio/sample timeline timestamp:
  ```text
  Event
  ├── type
  ├── payload
  ├── musical timestamp
  └── sample/frame timestamp
  ```
  Structure follows the engine's clock model. Critical requirement: **the worker schedules an event
  by its authoritative timestamp, not by the instant the IPC message arrives.**
  ```text
  Tidal / Hathor master clock → timestamped event → IPC transport → ChucK worker
                    → schedule against timestamp → audio output
  ```
  This makes IPC latency a *transport* problem, not a musical-timing reference; the worker keeps
  enough shared-timeline info to compensate. B4-K6 documents:
  - what clock owns musical time (Tidal/Hathor master clock);
  - how sample/frame timestamps are generated;
  - how timestamps are transferred across the process boundary (in-band, event metadata);
  - how the worker maps timestamps to its own audio timeline (shared clock / offset compensation);
  - what happens if an event arrives **too late** (drop + report, or schedule aligned to next
    boundary — define which);
  - what happens if an event arrives **early** (held until its timestamp — the normal case);
  - how buffer boundaries are handled (defer to boundary for sample-alignment);
  - how drift is detected (running sample-clock offset between master and worker);
  - how clock synchronization is maintained (continuous correction / shared timebase);
  - what "sample-accurate" means **operationally** in this implementation.
- **Do not downgrade the requirement to "within one buffer" merely because IPC is involved.** Design
  so delivery timing and event execution timing are explicitly separated. If the spike (B4-K0.6) or
  the K6 implementation shows the chosen architecture cannot honestly satisfy the intended sample-
  accurate requirement, **document the measured limitation and revise the claim based on evidence** —
  do not leave the contradiction implicit.
- **Acceptance:** a `.ck` instrument tracks pattern-driven Note On/Off and parameter events by
  timestamp (not IPC arrival) in sample-accurate time without a shared-global BPM poll; ordering is
  deterministic; late/early/buffer-boundary/drift behaviour is explicitly defined and demonstrated;
  audio stays lock-free.

### B4-K7 — `.ck` tab eval
- `Ctrl+Enter`/`Ctrl+Alt+Enter` on a `.ck` tab compiles via the worker and hands the shred to that
  tab's VM (analogous to `.hathor` `set-pattern`). Compile errors surface in the same status bar.
  A fresh eval replaces the prior shred; a stop path is available (per-tab via B4-K5 restart or an
  explicit stop). Per decision #6/#9 eval can fail loudly but is never a silent no-op.
- **Acceptance:** eval produces live, audible, per-tab-isolated ChucK; compile errors reported;

### B4-K8 — Hard gate: hung-shred + native-crash/worker-death + shared-memory recovery tests
- Hard gate (DoD §6.2), not optional. Tests:
  1. **Deliberately hung shred** — `while(true){}` with no `now +=>`: the tab's VM restarts in ~2 s
     and the JUCE audio thread + rest of app stay responsive.
  2. **Native crash / worker death** — a crashing `.ck` (or killed worker) does not crash the main
     process; worker restarts; affected tab re-inits; other tabs continue.
  3. **Shared-memory recovery (PART C)** — kill the worker *mid-write*; confirm the main process
     detects death (generation mismatch), stops reading stale memory, emits silence, reinitializes
     shared memory, and restarts — all without hanging the audio thread (evidence carried from
     B4-K0.6).
- Unit/ctest the heartbeat+watchdog, worker-restart, and shared-memory-recovery paths.
- **Acceptance:** a malicious/hung/crashing `.ck` never hangs or crashes the JUCE audio thread or
  Hathor; explain then recover with a UI message, never silently.

---

## Phase D — Real EQ

> **Direct implementation.** The design below is final (decision #11); each sub-item `B7-Kn` is a
> **single-agent task** — one agent runs one sub-item, in order. Do not run a separate own-pass;
> implement from this spec. This is real-time DSP (filter math correctness matters — getting it
> wrong causes audible artifacts/clipping). Depends on A2 (UI) and the existing audio engine +
> `masterGain_` pattern.

| ID | Item | Size | Depends |
|----|------|------|---------|
| B7-K1 | Per-voice biquad low-pass: `cutoff`/`resonance` `ParamMap` keys + filter state in `Voice` | medium | — |
| B7-K2 | Master-bus preset EQ: Flat / Bass Boost / Vocal / Bright 2–3 band chain + atomic swap | medium | — |
| B7-K3 | Settings Appearance: Audio EQ preset selector (via A2's tab) | small | B7-K2, A2 |
| B7-K4 | Verification: per-event filtering ear-test + preset-sweep click/pop check | small | B7-K1, B7-K2 |

### B7-K1 — Per-sound filtering (matches Strudel's `.lpf()`/`.cutoff()`)
- Add two new `ParamMap` keys: `"cutoff"` (double, Hz) and `"resonance"` (double, Q factor). These
  are populated the same way `"gain"`/`"speed"`/`"pan"` already are — set via mini-notation
  parameter syntax if/when extended, or left at sensible defaults (cutoff ≈ 20000 Hz / effectively
  off, resonance ≈ 0.707) if unset.
- Add a per-voice biquad low-pass filter to `Voice` (in `VoicePool`): compute filter coefficients
  **once, at trigger time**, from the `cutoff`/`resonance` params (standard RBJ Audio EQ Cookbook
  low-pass formulas — well-established, documented DSP math, not designed from scratch).
- Unlike gain/pan (a scalar), the filter must process **every sample** of the voice's playback —
  implement as per-voice filter state (e.g. two delay-line floats for a direct-form biquad) updated
  in `VoicePool::mix()` alongside the existing per-sample resampling/mixing loop. No heap
  allocation, no per-sample coefficient recomputation — coefficients are fixed for the voice's
  lifetime, matching how gain/speed/pan already work (Req 10.6's "set at trigger time, not changed
  mid-voice" — the same rule now applies to filter coefficients).
- **Acceptance:** varied `cutoff`/`resonance` per event produce audible per-event filtering (not a
  global effect); the audio callback stays allocation-free.

### B7-K2 — Master-bus preset EQ
- A small fixed set of presets (Flat / Bass Boost / Vocal / Bright) applied as a 2–3 band
  fixed-coefficient filter chain at the master mix stage — same location as `masterGain_`, applied
  **after** per-voice filtering and **after** ChucK audio is mixed in.
- **Chain order is fixed (decision #13): `Master EQ → Final Master Gain → Output`.** The B7 master
  filters are the last signal-shaping stage; master gain is applied after them. This is an
  architectural constant, not left to individual implementers.
- Changing presets recomputes filter coefficients on the worker/control thread and hands them to
  the audio thread via the same atomic swap pattern used everywhere in this codebase — never mutate
  filter state the audio thread is concurrently reading.
- Ship exactly these 4 presets for v1; do **not** build a free-form multi-band slider UI yet
  (that's a later extension once the fixed-preset version is proven stable).
- **Acceptance:** selecting a preset applies it at the mix stage, then master gain, then output;
  hot-swappable with no audio-thread allocation/mutex; no audible artifacts/clicks at the tested
  gains.

### B7-K3 — Settings UI: Audio EQ preset selector
- Lives in the Settings tab's Appearance/Audio section (A2). Preset selection drives the B7-K2
  swap. Uses A2's Apply/Reset persistence semantics (decision #8).
- **Acceptance:** the 4 presets are selectable and persisted; changing them applies instantly
  without a rebuild.

### B7-K4 — Verification
- Play a pattern using varied `cutoff`/`resonance` per event; confirm audible per-event filtering
  (not a global effect).
- Sweep master EQ presets while a pattern plays; confirm no clicks/pops at transitions (same
  ear-test bar as every other hot-swap check in this project).
- **Acceptance:** both checks pass with no audible artifacts.

---

## Phase E — Bake to Song & audio-asset lifecycle

> **Direct implementation.** Design is final (decisions #12; the asset workflow in the "V2
> Architecture" section). Each sub-item `B8-Kn` is a **single-agent task**, one agent per sub-item,
> in order. No separate own-pass. Converts live ChucK instruments into permanent project `.wav`
> assets so songs never depend on recompiling arbitrary native synthesis on open. Depends on A2
> (Settings), A4 (recursive file tree), the B4 worker, and the existing SampleBank.

| ID | Item | Size | Depends |
|----|------|------|---------|
| B8-K1 | Live-Jam vs Studio mode plumbing (per-session temp vs permanent target) | medium | A2 |
| B8-K2 | Background render writer: render active instrument → `.wav` (Studio/Live target) | large | B4-K6, B4-K2 |
| B8-K3 | Clean shutdown of the baked tab's VM/thread + resource release | medium | B8-K2 |
| B8-K4 | Bind `.wav` to the project SampleBank + `.hathor` editor/autocomplete exposure | medium | B8-K2, B8-K3 |
| B8-K5 | `.hathor_assets` managed-dir handling in the FileTreeDataModel (A4) | medium | B8-K4, A4 |
| B8-K6 | Bake to Song UX: `Ctrl+Shift+B`, status, error path | small | B8-K2–B8-K4 |

### B8-K1 — Studio vs Live Jam asset target
- Two rendering targets, selected per bake: **Studio (default)** writes to the permanent
  `.hathor_assets/chuck_instruments/<name>.wav` so the asset survives reopening the project months
  later (production workflow); **Live Jam (optional)** writes to a temporary/session location,
  usable for disposable four-bar risers / one-off transitions / textures, cleaned up at session
  end. Default is Studio.
- **Acceptance:** both targets write a valid `.wav`; Studio persists across sessions; Live-Jam assets
  are offered and cleaned up at session end.

### B8-K2 — Background render → `.wav`
- Render the active ChucK instrument through a background audio writer into the chosen target path.
  The output is a normal uncompressed audio asset suitable for the project's SampleBank.
- **Acceptance:** a fully non-real-time buffer render produces a valid `.wav` at the target path; the
  main process stays responsive throughout.

### B8-K3 — Shut down after render
- Once rendering fully succeeds, shut that tab's ChucK VM down, terminate its dedicated thread
  cleanly, and release its runtime resources — no manual process juggling by the user.
- **Acceptance:** after a successful bake, the tab's VM and thread are gone; resources released; no
  lingering background process.

### B8-K4 — Bind to SampleBank + expose to `.hathor`
- Register the resulting asset with the project's SampleBank and expose it to the `.hathor`
  editor/autocomplete, so finished song code is literally ``d1 $ s "acid_bass"``. The `s` sample
  resolves to the ordinary project audio asset — no live VM required.
- **Implementation:**
  - `SampleBank::addEntry(name, index, data, channels, rate, path)` — registers a baked WAV
    as `name=<stem>, index=0` in the existing SampleBank.  Thread-safe via a registration
    mutex; `find()` remains lock-free for the audio thread.
  - `SampleBank::listNames()` — returns sorted unique sample names for editor autocomplete
    and the `list-samples` control command.
  - `SampleBank::reloadStudioAssets(dir, formats, rate)` — on restart, scans
    `<project>/.hathor_assets/chuck_instruments/` for flat `.wav` files and registers
    them, preserving baked instruments across application restarts.
  - `AudioEngine::startBakeRender()` wraps the B8-K2 completion callback: on success,
    it derives the sample name from the output path stem and calls
    `registerBakedAsset()` → `SampleBank::addEntry()`, making the instrument
    immediately playable via `s "name"` without re-baking.
  - `ControlInterface` adds a `list-samples` command returning all registered
    sample names as JSON.
  - LiveJam assets follow B8-K1 session lifetime (temp dir cleaned at shutdown);
    Studio assets persist in `.hathor_assets/` and are never removed by cleanup.
- **Acceptance:** the baked sample is selectable in the SampleBank and resolves from `.hathor`
  notation with no ChucK runtime.  `list-samples` returns the baked instrument name.  Restart
  preserves Studio-baked instruments.

### B8-K5 — `.hathor_assets` in the file tree
- The recursive `FileTreeDataModel` (from A4) treats `.hathor_assets` as a **managed** project
  directory: hide/collapse its implementation details from the normal project view, expose the
  meaningful assets (e.g. the instrument), connect the rendered `.wav`/source `.ck` to
  autocomplete, and
  keep the real filesystem layout intact underneath. User sees `Instruments → acid_bass`, not the
  internal dir.
- **Acceptance:** the explorer shows a clean managed view (e.g. `Instruments/acid_bass`) while the
  `.hathor_assets/chuck_instruments/{acid_bass.ck, acid_bass.wav}` layout stays on disk; `.ck`
  source remains accessible for later regen.

### B8-K6 — Bake to Song UX
- **`Bake to Song`** (or `Ctrl+Shift+B`) triggers B8-K2→K4 with clear in-progress / success / error
  UI (target chosen between Studio/Live). No silent failures (decision #6).
- **Acceptance:** a user can select an active ChucK instrument, invoke Bake, and end with a
  permanent `.wav` bound into the project, with legible errors otherwise.

---

## Phase F — Integration & the genuinely hard piece

| ID | Item | Size | Subsystem | Depends | Own-pass |
|----|------|------|-----------|---------|----------|
| C1 | Editor now-playing highlight | small–medium | `ui` | B2 | no |
| C2 | Chat reconnect / thread UX polish | small | B6 | no |

### C1 — Editor now-playing highlight
- A live overlay on `HathorTab`/`EditorArea` that draws a glyph-box over the currently-sounding atom
  for the tab's slot, using the B2 `(slot, offset)` per-event updates. Must not fight the existing
  static syntax coloring; repaint only from the UITimer pathway.

### C2 — Chat reconnect / thread UX
- Reuse the existing disconnect → `restart()` path (`ui/ChatSidebar.cpp:320-332,385-397`) per
  thread tab; add thread-scoped reconnect prompts (per decision #3).

---

## Phase G — Petdex mascot (its own sub-project; sequential inside)

> Treat D1–D4 as **their own requirements→design→tasks pass** — external network dependency,
> spritesheet decoding (WebP vs zipUrl), licensing, and animation-state mapping.

| ID | Item | Size | Depends | Own-pass |
|----|------|------|---------|----------|
| D1 | Manifest fetch/cache + opt-in picker UI | small–medium | A2 | yes (all D falls under one own-pass) |
| D2 | Spritesheet fetch + decode | medium | D1 | same pass |
| D3 | Frame-slice 8×9 192×208 + animate + state map | medium | D2 | same pass |
| D4 | Per-selection license/attribution check & display | small, but blocking before any pet is shown | D1 | same pass |

- **Decision #5:** no default mascot; fetched/cached only after explicit selection. D4 still shows
  licensing/attribution for any pet actually displayed.
- **Audited facts:** manifest (live) = 4425 pets, 7 fields each
  (`slug displayName kind submittedBy spritesheetUrl petJsonUrl zipUrl`); `spritesheetUrl` is a
  **WebP** sheet (JUCE core loads PNG/JPEG/GIF, not WebP — prefer the `zipUrl` package, or add WebP
  decode); **no license field** in the manifest (attribution only via `submittedBy`); the 8×9/192×
  208 frame grid and animation-state names (idle/working/… ) are **not** encoded in the manifest,
  so resolve via the zip or a convention.

---

## Phase H — AI Authoring & Hathor MCP v2

> **Direct implementation.** Design is final (decisions #14–#21; two tracks below; plus an explicit
> §"Verification-first" gate before any external language-intelligence dependency is adopted). Each
> item `AI-1`…`AI-8` (Phase H) and `AI-9`+`AI-G1`…`AI-G9` (Phase I) is a **single-agent task**, one
> agent per sub-item, **in
> order** (AI-1 first — it is the canonical contract everything else sits on). Do not implement all
> MCP tools at once; follow the waves. **One non-negotiable rule (decision #14): the MCP must not
> become the place where Hathor's intelligence lives.** And (decisions #16–#17): **language
> intelligence comes from an existing Strudel LSP, not a bespoke re-parser built inside Hathor.**
> LSP + diagnostics is *not* the whole authoring experience (decisions #19–#21): **AI ghost-writing
> (llm-ls/FIM, project-aware context, few-shot, inline ghost text) is a separate, complementary
> system** — see §AI-G1…AI-G9. The canonical application API/command layer and the versioned language
> service are the two most important deliverables — they keep JUCE, MCP, the editor, and AI prompts
> from each knowing a *different* Hathor. Depends on H0 (socket accept loop), Phase A, and the B4/B8
> worker + asset architecture it surfaces.

```text
                          AI Agent
                             │
                            MCP
                 ┌───────────┴───────────┐
                 │                       │
                 ▼                       ▼
           Hathor MCP              LSP → MCP bridge
                 │                       │
                 ▼                       ▼
           Hathor App           (Language Intelligence) Strudel LSP
                                         │
                                         ▼
                                  llm-ls / FIM (AI ghost)
```
The Hathor editor may also consume the LSP directly (editor → LSP → Strudel LSP). **LSP =
language-intelligence service; MCP = agent capability/interface layer** — they are separate
concerns (decision #17); an existing LSP→MCP bridge is used (e.g. `isaacphi/mcp-language-server` or
equivalent, subject to §Verification-first) rather than a custom protocol. The AI ghost layer sits
on top via **llm-ls (or equivalent FIM server)** — decision #20 (see §AI-G1).

**Track A — Agent Interface (MCP v2).** The external AI↔Hathor surface: project inspection,
diagnostics, ChucK lifecycle, rendering, asset management, safe song editing. Replaces the
current "MCP → text command → Hathor" 5-tool playback bridge.

**Track B — Authoring Intelligence.** The in-app authoring layer splits into two complementary
systems (decision #19) that **must not be collapsed into "LSP support"**:
- **Deterministic language intelligence** (exact, predictable): Strudel/Tidal + mini-notation
  completion, hover/docs, supported-function discovery, sample completion, deterministic ChucK
  completion where supported, ChucK compiler diagnostics, exact supported-API/symbol info. Source:
  an **existing Strudel LSP** (`strudel-lsp-server`) for `.hathor` + versioned supported-surface
  metadata + the real libchuck compiler for ChucK diagnostics.
- **AI authoring intelligence** (Cursor-style, probabilistic): inline ghost text, Fill-in-the-
  Middle completion, multi-token/multi-line continuation, pattern continuation, musical
  transformation, ChucK synthesis continuation, context-aware generation, AI repair after
  diagnostics — via an existing generic LLM-LSP/FIM server (`llm-ls` or equivalent) + project-aware
  context + few-shot (AI-G1…AI-G9).
Shared source for editor, AI context, docs, validation — one authoritative language-intelligence
source per language (decision #18, §AI-9 rule).

### Phase H table

| ID | Track | Item | Size | Depends |
|----|-------|------|------|---------|
| AI-1 | A+B | Canonical AI/application contract + authorization/safety capability model | large | H0 |
| AI-2 | A | Read-only project intelligence (inspect_project, get_current_song, list_assets, list_samples, list_chuck_instruments, get_diagnostics, get_audio_status) | large | AI-1 |
| AI-3 | B | Language intelligence foundation: existing Strudel LSP + **versioned** supported-surface metadata | large | AI-1 |
| AI-4 | B | LSP-powered editor language integration (completion L1–L3, hover, diagnostics) | medium | AI-3 |
| AI-5 | A+B | ChucK lifecycle + **real-compiler diagnostics** (async jobs) | large | AI-2, B4 |
| AI-6 | A | Rendering / asset lifecycle with **commit/overwrite safety** | medium | AI-5, B8 |
| AI-7 | A | Safe song mutation (edit_song, transactional + validated) | medium | AI-2 |
| AI-8 | B | Project-aware AI context provider (targeted, not full-project; LSP→MCP bridge) | medium | AI-3, AI-2 |

> Phase H is the **canonical contract + authoring foundation + MCP tools**. The rest of the AI layer
> continues in later phases: **Phase I** (AI-9 + AI-G1…AI-G9 ghost writing/completion system),
> **Phase J** (inline-completion UX polish on top of Phase I), and **Phase K** (AI-10 agent session
> + agentic workflow).

**MCP namespace organization (target, decision #14):** group tools into namespaces rather than
~30 random ones — `Project` (inspect_project, get_current_song, list_assets, get_diagnostics),
`Music` (get_current_pattern, set_pattern, set_bpm, list_samples, list_scales, get_scale),
`ChucK` (list_chuck_instruments, create_chuck_session, get_chuck_session, compile_chuck,
audition_chuck, stop_chuck), `Rendering` (render_chuck, get_job_status, commit_rendered_asset),
`Playback` (play, stop, set_gain, get_audio_status), `Editing` (edit_song).

### AI-1 — Canonical AI/Application Contract + Authorization/Safety Model
- **Architectural first task; no feature work yet.** Audit the existing MCP/socket bridge and
  define, once, the canonical internal command/service boundaries (Project / Pattern / ChucK /
  Assets / Playback / Diagnostics): request/response schemas, errors, **async jobs**, permissions,
  resource IDs, lifecycle semantics. A shared layer callable by **UI, MCP, AI authoring, tests**.
  Replace MCP→text-command translation with direct calls into this layer. Do **not** invent a
  separate permission system that diverges from existing Hathor conventions; follow them.
- **Authorization & safety capability model (mandatory).** Because later items can modify songs,
  project state, compile code, start/stop sessions, render audio, create persistent assets, and
  overwrite state, AI-1 defines an explicit capability model — every operation belongs to exactly
  one class:
  - **Read-only** (safe, no mutation): `inspect_project`, `get_current_song`, `list_assets`,
    `list_samples`, `list_chuck_instruments`, `get_diagnostics`, `get_audio_status`.
  - **Non-destructive execution** (mutates runtime/audio state only, never persistent files):
    `compile_chuck`, `audition_chuck`, `play`, `stop`.
  - **Persistent mutation** (requires stronger safeguards): `edit_song`, create/modify/render-and-
    commit assets, overwrite project content.
- For each operation AI-1 records: is it read-only? does it mutate runtime vs persistent state?
  does it need user confirmation? can it overwrite an existing asset? how does cancellation work?
  how do failed ops roll back? how are destructive ops audited? how are MCP clients
  authenticated/authorized? how future agents are prevented from bypassing these boundaries.
- **Acceptance:** one documented service interface is consumed by all four callers; every tool is
  tagged with its capability class; read-only tools are guaranteed mutation-free; persistent
  mutations carry confirmation + rollback + audit; MCP no longer maintains its own model of Hathor;
  the existing playback tools route through it without regression.

### AI-2 — Read-Only Project Intelligence
- Give the AI eyes first — **no destructive tools yet.** `inspect_project` (semantic project
  representation: name, songs, current song, bpm, active slots, chuck_instruments, samples — not a
  raw filesystem dump), `get_current_song` (source, active patterns, tempo, selection, diagnostics,
  referenced assets), `list_samples` (name/path/duration, from SampleBank), `list_chuck_instruments`
  (name/source/rendered/audio_asset), `get_diagnostics` (lang + chuck errors straight from the real
  parser/compiler, not inferred), `get_audio_status`.
- **Acceptance:** each returns its documented schema; the AI can fully describe a project without
  the filesystem; diagnostics become the repair loop's foundation.

### AI-3 — Language Intelligence Foundation (existing Strudel LSP + versioned supported-surface metadata)
- **`.hathor` is standard Strudel mini-notation** (reimplemented + differential-tested in Hathor's
  C++ engine against Strudel golden fixtures), **not** a custom dialect or superset (decision #18).
  So do NOT design a Hathor-specific grammar; the `.hathor` extension is merely the Hathor editor/
  project representation of standard Strudel mini-notation source. The upstream surface (all of
  Tidal/Strudel/Chuck) is distinct from the **Hathor supported surface** (what Hathor intentionally
  implements, validates, and promises the user) — only the latter must be exhaustively represented.
- **Primary language intelligence = an existing Strudel LSP**, `strudel-lsp-server`, integrated
  (subject to §Verification-first), reused rather than re-implemented: completion, hover,
  function/pattern/sample discovery, diagnostics. Do NOT fork/re-parse it; do NOT duplicate its
  intelligence in Hathor. Only add Hathor-specific integration where Hathor's actual supported
  surface / project model / editor behavior requires it. LSP is language intelligence; MCP is the
  agent layer (decision #17) — never collapse the two.
- **Versioned supported-surface metadata (mandatory, decision #18).** The part of AI-3 Hathor owns
  is a **versioned** model identifying exactly which language/runtime surface it describes (e.g.
  `LanguageMetadata { schemaVersion, hathorEngineCompat/version, strudelMiniNotationCompat,
  chuckLibVersion, definitions[] }` — exact field names are implementation choices). This prevents
  stale/partially-updated definitions driving editor/AI/validation after the parser, Strudel
  surface, or vendored libchuck version changes. AI-3 defines: (1) the schema/version scheme,
  (2) compatibility/version identification, (3) how metadata changes are detected + reviewed,
  (4) how consumers report/identity which metadata version they use, (5) how incompatible
  metadata+runtime combinations are prevented/surfaced. Keep it to traceability + compatibility,
  not heavyweight package infra.
- For the intended ChucK/`.ck` surface, define the ChucK API metadata (oscillators, envelopes,
  Hathor-specific APIs) so the AI/editor understands **your** vendored version + Hathor
  integration constraints — but the authoritative ChucK *diagnostics* come from the real compiler
  (AI-5), not from metadata or an approximate parser.
- **Acceptance:** the Strudel LSP is confirmed compatible with `.hathor` mini-notation source
  (§Verification-first) and integrated into the editor + AI (via LSP→MCP bridge); a versioned
  supported-surface metadata model exists, is exhaustive only for the supported surface, is
  version-identified everywhere, and is the single source (no separate editor vs MCP lists).

### AI-4 — LSP-Powered Editor Language Integration (completion / hover / diagnostics)
- Non-AI editor interaction first (decision #15), driven primarily by the integrated Strudel LSP
  (decision #16): **L1** completion (samples `kic`→`kick/kick2/kick_heavy`; `scale "|`→minor/major/
  dorian/phrygian…), **L2** signature-aware (valid args for `fast`; structural for `slowcat 4 [`),
  **L3** context-aware ("inside d1, these samples exist") — plus hover docs and basic diagnostics
  from the LSP's native capabilities (verified in §Verification-first). Only where the LSP does
  not cover Hathor's *supported* surface does Hathor layer on top from the versioned metadata
  (AI-3). **L4 (inline AI) is AI-9, not here.**
- **Acceptance:** the editor drives completion/hover/diagnostics through the LSP (or its verified
  bridge), not through a bespoke re-parser; reliable for the supported surface; no LLM in the
  basic-syntax hot path.

### AI-5 — ChucK lifecycle + real-compiler diagnostics (async jobs)
- Abstract surface mapped onto the isolated per-tab ChucK architecture (B4-K3) rather than exposing
  low-level VM internals: `create_chuck_session`, `get_chuck_session` (id, source, state,
  diagnostics, runtime status), `compile_chuck` — **an asynchronous job, not a blocking MCP call**
  → `{job_id, status}`, `audition_chuck` (start), `stop_chuck` (that session only).
- **ChucK diagnostics authority = the actual vendored ChucK/libchuck compiler/runtime (decision
  #18).** A ChucK diagnostic service accepts `.ck` source, runs real compiler/syntax validation
  via the vendored libchuck API, and returns structured diagnostics (line/column where the compiler
  provides it), feeding the editor and the MCP/application API. Do **not** approximate with a
  re-parser for symmetry with the Strudel LSP. **Implementer must first inspect the actual libchuck
  compiler API in the repo and use the real supported mechanism** — do not hard-code an assumed API
  such as `checkSyntax()`.
- **Acceptance:** tools drive the isolated sessions from the abstract surface; compile is async via
  the shared job infra; per-session stop isolates one session; diagnostics originate from the real
  compiler.

### AI-6 — Rendering / Asset lifecycle (with commit & overwrite semantics)
- `render_chuck` (session_id, duration_bars, asset_name → a job), `get_job_status` (job_id →
  status/success/diagnostics), and `commit_rendered_asset` (or auto-commit on success) producing a
  B8-managed asset (`.hathor_assets/chuck_instruments/{name}.ck` + `{name}.wav`). Reuses the AI-5
  job infrastructure.
- **Persistent-mutation safety (decision #18 / AI-1 capability model).** Define explicitly: render
  itself is non-destructive (a job); the asset becomes persistent only on a confirmed commit;
  **overwriting an existing asset requires confirmation** (asset-name collision → confirm or unique
  name); define name resolution; what happens if render succeeds but registration fails (partial
  result cleaned up, no orphan half-committed asset); and how failed/partial renders are cleaned
  up. A commit is audited and rollback-capable.
- **Acceptance:** render is a real B8 bake with an observable job lifecycle; a completed render
  yields a project asset bound to the SampleBank; overwrite is gated by confirmation; failed/partial
  renders leave the project clean and audited.

### AI-7 — Safe Song Mutation
- `edit_song` structured editing, not arbitrary filesystem writes: e.g.
  `{operation: "replace_pattern", slot, pattern}` or `{operation:"insert", location, content}`.
  **Transactional + validated** — the AI must not silently break a working song; on failure the
  prior state is retained and diagnostics returned. Existing `set_pattern`/`set_bpm`/`play`/`stop`/
  `set_gain` are kept and eventually return validation/diagnostic feedback.
- **Acceptance:** edits are structured, validated, transactional; a partial/broken input can never
  leave the project in a broken state; persistent mutation is classified + confirmed + audited per
  the AI-1 capability model (overwrite/delete/replace cross the confirmation boundary).

### AI-8 — AI Context Injection (via LSP / LSP→MCP bridge; feeds ghost-writing too)
- Wire the authoring layer to the AI **dynamically**: current file, cursor region, current pattern,
  available samples/instruments/scales, current bpm, relevant API definitions, current diagnostics.
  Targeted, **not the whole project every time** — no giant static system prompt. Language
  intelligence reaches the AI through the LSP→MCP bridge (decision #17); the versioned supported-
  surface metadata (AI-3) supplies Hathor-specific facts the LSP doesn't carry. No duplicated
  language knowledge (decisions #16/#18). AI-8 supplies the *editor/language* context; the
  edit-location-specific authoring context for FIM is **AI-G3**.
- **Acceptance:** context is assembled per request from the shared models / LSP / metadata; it
  tracks project edits; it removes the need for huge static prompts.

## Phase I — AI Ghost-Writing & Inline Completion (ballooned from AI-9/AI-G)

> **Direct implementation.** This phase was originally part of AI-9 + AI-G1…AI-G9 inside the AI
> quadrant; it ballooned into its own phase (decision #19/#20). Item IDs (`AI-9`, `AI-G1…AI-G9`) are
> kept as-is. It is the Cursor-style **probabilistic** authoring layer on top of Phase H's
> deterministic foundation. Each item is a single-agent task, in order. Depends on Phase H (AI-1…
> AI-8), the §Verification-first spike (including llm-ls verification), and the B4/B8 worker/asset
> architecture.

| ID | Track | Item | Size | Depends |
|----|-------|------|------|---------|
| AI-9 | B | Inline AI completion (Cursor-like; on top of LSP+metadata+context) | large | AI-8, AI-4 |
| AI-G1 | B | Evaluate & integrate an existing generic LLM-LSP/FIM server (`llm-ls`) for ghost writing | large | AI-9, AI-3 |
| AI-G2 | B | Fill-in-the-Middle as a first-class design requirement (prefix/suffix/middle) | medium | AI-G1 |
| AI-G3 | B | Hathor-specific authoring-context provider on top of llm-ls (retrieval/relevance, not repo dump) | medium | AI-G1 |
| AI-G4 | B | Few-shot / domain-specific completion context (`.hathor` + ChucK, versioned) | medium | AI-G3 |
| AI-G5 | B | Deterministic completion and ghost completion coexist (explicit UI precedence/cancel/invalidate rules) | medium | AI-4, AI-G1 |
| AI-G6 | B | Ghost text is UI state, not document state (never inserted until accepted) | medium | AI-G1, AI-G5 |
| AI-G7 | B | ChucK authoring assistance (deterministic + AI; reuse-or-minimal; no bespoke full ChucK LSP without reuse study) | large | AI-G3, AI-5 |
| AI-G8 | B | Preserve the existing Strudel LSP decision; `.hathor` = standard Strudel mini-notation | ack | AI-3 |
| AI-G9 | B | Final Track B authoring stack (dependency graph + diagram) | small | AI-G1…AI-G8 |

### AI-9 — Inline AI Completion (ghost-writing entry point)
- Only now the Cursor-like layer, on the deterministic foundation (Strudel LSP + AI-3 versioned
  metadata + AI-8 context + AI-2 diagnostics/assets). Inline suggestions + high-level
  transformation ("make this bassline darker"). The LLM expands/transforms against facts, never
  paper over with invented syntax. **AI-9 starts the probabilistic ghost layer; its full system is
  §AI-G1…AI-G9** (llm-ls/FIM, context, few-shot, ghost-text semantics, coexistence, ChucK authoring).
- **Acceptance:** decisions map to currently-valid language/project facts, not generic filler;
  deterministic and AI completion coexist per §AI-G5 instead of fighting; quality justifies the
  agentic flow (AI-10).

### AI-G1 — Integrate `llm-ls` (or equivalent) for Ghost Writing (decision #20)
> Do **not** build a custom LLM completion server from scratch. Reuse an existing generic
> LLM-LSP/FIM implementation (e.g. Hugging Face's `llm-ls`). Prior to implementation, verify the
> selected version's real capabilities and API — do not hard-code unverified config/protocol from
> secondary docs (§Verification-first applies to llm-ls too).
- Conceptually:
  ```text
  Hathor Editor ─(inline completion request)→ llm-ls ─(FIM request + context)→ Configured LLM provider
     ─→ completion ─→ Hathor ghost-text renderer
  ```
- Establish: how `llm-ls` is launched; how Hathor communicates with it; how completion requests are
  **cancelled**; how **stale responses** are discarded; how requests are **debounced**; how
  **latency** is controlled; how **provider configuration** works; how **FIM prefix/suffix/middle**
  maps to Hathor; how `.hathor` and `.ck` files are identified (language id); how **custom context**
  is injected; how completion results are returned to the JUCE editor.
- The LLM completion path stays **off the JUCE real-time audio thread**.
- **Acceptance:** `llm-ls` is integrated against a verified API and returns Cursor-style ghost
  completions to the editor; full lifecycle (launch/communicate/cancel/stale-drop/debounce/latency/
  provider) is defined and functional; no custom LLM server is written.

### AI-G2 — Fill-in-the-Middle as a First-Class Design Requirement (decision part #20)
- Ghost-writing uses **FIM** (prefix/suffix/middle), not whole-document continuation. Conceptual
  request:
  ```text
  PREFIX    code before cursor
  SUFFIX    code after cursor
  CONTEXT   relevant project/language information
  LANGUAGE  .hathor or .hathor/.ck
  SUPPORTED SURFACE  relevant Hathor-supported APIs/constructs
  INSTRUCTIONS  brief completion-specific instructions
  ```
  Model generates the missing middle. This matters because users place the cursor *inside* an
  existing pattern/synthesis graph and expect completion of the missing portion, not a blind append.
- **Acceptance:** an express FIM path (prefix/suffix/middle) exists through llm-ls; completing
  inside a pattern/synthesis graph yields the missing middle, not end-of-file continuation.

### AI-G3 — Hathor-Specific AI Context provider on top of llm-ls
- llm-ls doesn't know Hathor by default; build a **Hathor authoring-context provider** that supplies
  **compact, relevant** context per editing location — retrieval/relevance selection, **not** a dump
  of the whole repository.
- For `.hathor` (current file, cursor position, surrounding pattern, current selection, nearby
  patterns, available project samples, rendered ChucK instruments, project BPM, project/song context,
  supported scales, relevant Strudel/mini-notation constructs, transformation functions, current
  diagnostics, relevant language metadata, a small number of high-quality examples).
- For `.hathor`/`.ck` (current ChucK file, cursor, surrounding ChucK code, selection, current
  instrument/session, relevant supported ChucK constructs, synthesis examples, available Hathor
  audio APIs, current compiler diagnostics, relevant ChucK metadata, project assets, concise valid
  audio-routing idioms).
- **Acceptance:** context is assembled per edit location via retrieval/relevance; it is bounded
  (not whole-repo for every request); it flows into the llm-ls completion request.

### AI-G4 — Few-Shot / Domain-Specific Completion Context
- Both domains are niche; support explicit **Hathor-specific few-shot examples**, versioned and tied
  to the supported surface where appropriate.
- `.hathor`: valid Strudel mini-notation, supported pattern structures, transformations, scales,
  rhythmic structures, valid project/sample conventions.
- ChucK: valid UGen routing, `=>` audio graph construction, time advancement (`,`/`now` etc.),
  synthesis patterns, envelopes, filters, oscillators, valid DAC routing, safe Hathor-supported
  usage patterns.
- **Acceptance:** examples reinforce correctness; the base model's training knowledge is not assumed
  sufficient; few-shots are versioned with the supported surface (AI-3).

### AI-G5 — Deterministic and Ghost Completion Coexist (concurrency/state rules)
- Standard completion (`fast|` → Strudel LSP deterministic candidates) vs ghost completion (user
  pauses / invokes AI action → possible multi-token continuation, e.g. `d1 $ s "bd sd hh"`).
  Explicit precedence rule; the two must not fight:
  ```text
  deterministic completion popup active → ghost is lower priority / temporarily hidden.
  no deterministic popup → ghost may render.
  user continues typing → cancel stale ghost completion.
  user accepts completion → commit only the generated text.
  document changes → invalidate stale completion results.
  ```
  Behavior follows JUCE's existing editor architecture, but the concurrency/state rules are explicit.
- **Acceptance:** the coexistence rules above are implemented/tested; deterministic and ghost never
  conflict simultaneously.

### AI-G6 — Ghost Text Is UI State, Not Document State
- AI completion is **never inserted into the underlying document until explicitly accepted**. Keep:
  ```text
  document text  +  temporary completion state (ghost text)
  ```
  as separate concepts. Ghost text must: not affect file contents; not affect undo; not affect
  compilation/diagnostics; disappear when cursor/context changes; be cancellable; be replaceable by a
  newer completion; become doc content only when accepted.
- Use the editor overlay/custom-rendering mechanism or appropriate approach; do not force a specific
  rendering implementation before inspecting the editor architecture.
- **Acceptance:** accepting a ghost comp materializes exactly the generated text into the document
  and undo; dismissing/typing/invalidating removes it; document, undo, compile, and diagnostics see
  only real document state before acceptance.

### AI-G7 — ChucK Must Also Have Authoring Assistance (decision part)
- Do **not** read "use the real ChucK compiler for diagnostics" as ChucK receiving only error
  reporting. Give ChucK an authoring experience comparable to `.hathor`, as supported by what
  available implementations provide: ChucK metadata + deterministic completion + real-compiler
  diagnostics + project-aware context + llm-ls/FIM ghost writing.
- **First investigate** whether a mature existing ChucK language server / reusable language-
  intelligence implementation exists. If none is good enough, use the real libchuck compiler as the
  authoritative diagnostic source and build *only the minimum deterministic-completion/metadata
  layer required by Hathor* — **no bespoke full ChucK LSP unless the reuse investigation proves it
  necessary** (decision #21).
- **Acceptance:** ChucK receives deterministic + AI authoring; no bespoke full ChucK server is built
  without a documented reuse investigation.

### AI-G8 — Preserve the Existing Strudel LSP Decision (declaration)
- Full decision record: `docs/design/ai-g8-strudel-lsp-decision.md`
- `.hathor` is **standard Strudel mini-notation**, not a custom Hathor dialect/superset. Architecture:
  ```text
  .hathor ─► standard Strudel mini-notation
       ├── Strudel LSP        → deterministic editor intelligence
       ├── Hathor metadata    → supported-surface/project knowledge
       └── llm-ls + FIM       → AI ghost writing
  ```
  No custom `.hathor` grammar merely because the extension is `.hathor`. Hathor's C++ parser remains
  the runtime source-of-truth implementation (validated against Strudel golden fixtures); the LSP is
  an authoring/intelligence service, **not** a replacement for the runtime parser.
- **Acceptance:** (ack, no code) the reuse decision stands; no custom grammar/LSP is introduced.

### AI-G9 — Final Authoring Intelligence Stack (diagram)
- Close Track B against accidental "LSP integration = ghost-write substitution". Final stack:
  ```text
  TRACK B — AUTHORING INTELLIGENCE
  Language Knowledge   : Strudel LSP · ChucK diagnostics · Hathor supported-surface metadata · versioning
  Deterministic Completion: Strudel/Tidal functions · mini-notation constructs · samples · scales ·
                 project assets · ChucK constructs · hover/documentation
  Project-Aware Context : current file · cursor/selection · surrounding code · project structure ·
              samples · instruments · patterns · BPM · scales · diagnostics
  AI Ghost Writing : llm-ls/FIM · multi-line/FIM continuation · Hathor context · few-shot ·
                 ChucK context · Strudel context · inline ghost rendering
  AI Repair        : generate → validate → diagnose → repair → validate again → audition
  ```
  Represent this whole stack in the program's dependency graph. This is decision #19's explicit
  guarantee that "LSP integration" is never a substitute for the ghost-writing system.

---

## Phase J — Inline-Completion UX Polish (on top of Phase I)

> **Direct implementation**, small-ish items. Now that ghost writing exists (Phase I), the inline
> completion *interaction* must not feel like a dumb autocomplete. Each item is a single-agent task;
> order roughly as listed. All items depend on AI-9 / AI-G1…AI-G9.

| ID | Item | Size | Depends |
|----|------|------|---------|
| J-1 | Triggering policy (when to ask the model at all) | medium | AI-9, AI-G1 |
| J-2 | Multiple candidates & cycling | small | J-1 |
| J-3 | Partial / multi-token acceptance | medium | J-1 |
| J-4 | Selection-aware & intent-aware completion (no new task — fold into J-1/AI-9/G3) | — | — |
| J-5 | Codebase / project retrieval for completion | medium | AI-8, AI-G3 |
| J-6 | Completion-quality feedback loop | medium | J-1 |

### J-1 — Completion Triggering Policy
- Don't call the LLM on every keystroke. Define *when* completion is offered: **not** inside
  strings/comments; not when the surrounding construct is invalid/incomplete; **not** when a
  deterministic popup/suggestion is already active; pause at a *meaningful* boundary (after `(`, `→`,
  `.`, space after a word, end of a token) rather than mid-word churn.
- **Acceptance:** the editor stops spamming ghost text; requests fire only at sensible boundaries;
  the LLM path stays off the audio thread.

### J-2 — Multiple Candidates & Cycling
- Offer up to a few candidates and cycle without recomputing: `Tab` accepts, `Esc` rejects,
  `Alt+→`/`Alt+←` move through alternatives.
- **Acceptance:** cycling works; accepting one candidate is a no-op on others; no re-request on
  cycle.

### J-3 — Partial / Multi-Token Acceptance
- User should be able to accept *part* of a suggestion (up to cursor) without accepting everything or
  nothing.
- **Acceptance:** partial accept inserts only the prefix up to the acceptance point and leaves the
  rest as pending/ghost.

### J-4 — Selection-Aware & Intent-Aware Completion (folded in)
- Already covered by AI-9 / AI-G3 (continue vs. transform vs. densify vs. repair). Do **not** add a
  separate task; keep it as an acceptance criterion of AI-9 and J-1 so there's one intent-aware path.

### J-5 — Codebase / Project Retrieval for Completion
- Make the completion context **project-aware**, not just current-file-aware. Index → retrieve →
  rank → inject: relevant instrument definitions, working ChucK idioms, examples, conventions.
- **Decision:** start with symbol/file/metadata + targeted search (no heavy vector DB / RAG yet);
  evolve to embeddings only if quality data demands it.
- **Acceptance:** completion can use a *relevant* snippet from elsewhere in the project; retrieval is
  bounded, cheap, and versioned.

### J-6 — Completion-Quality Feedback Loop
- Telemetry to improve the system over time: displayed / accepted / partially-accepted / rejected,
  time-to-accept, compile success after acceptance, introduced diagnostic, immediate deletion,
  heavy modification afterwards. Store per-language (e.g. "ChucK completions accepted 8%").
- **Acceptance:** the signal exists to tell us whether ghost writing is actually helping; surfaced in
  a readable report.

---

## Phase K — AI-10 Agentic Musical Workflow (end state, safety-carrying)

> **Direct implementation, large.** The whole thing is one phase (decision #18); item IDs (`AI-10`)
> kept as-is, with sub-specs below (AI-10.x agent-session specification). A human ("composer") asks
> the agent to build/modify music; the agent plans and executes a loop of read → edit → validate →
> audition → repair, gated at destructive steps. Depends on Phase H…J and the §Verification-first spike.

### AI-10 — Agentic Musical Workflow
- The full loop: `inspect_project → inspect song → inspect assets → generate pattern → validate →
  compile → audition → inspect diagnostics → repair → render → bind asset → update song`. Don't
  build `suggest_pattern` first; language/project reliability comes before generation quality.
- **Safety (decision #7, AI-1 capability model carried through).** The autonomous workflow has no
  unlimited authority. The read/compile/audition part may run automatically, but the destructive
  steps — overwrite song, delete asset, replace an existing instrument — **cross the same
  confirmation/authorization boundary as their corresponding AI-1/AI-6/AI-7 tools**. No bypassing.
  Defined here *before* AI-10 is implemented, not patched in after.

### AI-10.1 — Natural-Language Intent → Actionable Plan
- From a prompt ("make me a dark 8-bar acid bassline") the agent derives a concrete plan it will
  actually run: inspect song → inspect assets → decide reuse → create/modify ChucK → compile →
  audition → repair → render → ask permission → bind → report. The plan is shown before heavy steps.
- **Acceptance:** a plan (not a guess) is produced and is executable by the loop; reuse is decided
  *before* creating new assets.

### AI-10.2 — Conversational Memory / Working Set
- Distinct from AI-8 context injection. The agent remembers *this session*: "make it darker" refers
  to the bass just created; track filter movement, note simplicity, and how to revert.
- **Acceptance:** multi-turn references resolve against the working set, not against nothing.

### AI-10.3 — First-Class Diff / Preview / Undo for AI Changes
- Beyond AI-7's transactional safety: a human-readable AI change-set with checkpoints, and
  Accept / Reject / Undo at the change-set level.
- **Acceptance:** the composer can see what the agent changed and revert a whole change-set.

### AI-10.4 — Agent Explains What It's Doing
- The agent reports its step checklist + progress in the chat UI (reusing AI-5/AI-6 job infra):
  "I made the bass darker by lowering filter cutoff and simplifying notes…".
- **Acceptance:** progress/explanation events stream to the UI; the user always knows what step is
  running.

### AI-10.5 — Conversational Repair (beyond compiler repair)
- Broader than AI-5's compile-error repair. Two failure kinds: *technical* (compile/render errors)
  and *creative* ("too busy" → preserve instrument/sound, reduce density, re-audition — do not
  regenerate from zero). Reuse-first: check existing instruments/samples before creating
  `kick_1`/`kick_2`/`kick_final…`.
- **Acceptance:** creative feedback leads to targeted edits (not a full regen); existing assets are
  reused instead of duplicated.

### AI-10.6 — Agent Task Lifecycle
- A real lifecycle, distinct from MCP cancellation: `QUEUED → PLANNING → INSPECTING → EDITING →
  VALIDATING → AUDITIONING → WAITING_FOR_APPROVAL → COMMITTING → COMPLETED` (plus `FAILED` /
  `CANCELLED` / `WAITING_FOR_USER`). The user can interrupt and replan mid-chain.
- **Acceptance:** every step maps to a state; the chain can be cancelled/re-planned safely; nothing
  destructive commits without passing `WAITING_FOR_APPROVAL`.

### AI-10 Acceptance (whole phase)
- An agent takes a high-level request through the loop to a baked, bound, validated musical result;
  every persistent/destructive step is gated + audited; nothing bypasses the capability model.

---

## Phase L — Traditional IDE Foundation

> **Purpose:** Complete the non-AI IDE foundation around Hathor's native JUCE editor: workspace/editor
> ergonomics, navigation/search, diagnostics, tasks/terminal, Git/source control, debugging/runtime
> inspection, workspace/session persistence, and unified contextual actions.
>
> **Design principle:** Hathor should provide the core ergonomics users expect from a modern
> professional IDE while remaining deliberately domain-specific. Do **not** attempt to clone VS
> Code/Cursor or create an extension ecosystem. Hathor is a native C++/JUCE music IDE centered on
> `.hathor`/Strudel mini-notation, ChucK, audio assets, and the Hathor engine.
>
> **UI principle:** Build the visual UI natively with JUCE components. Reuse mature libraries for
> backend capabilities where appropriate; do not reimplement Git, language intelligence, compilers,
> or other mature infrastructure unnecessarily.
>
> This phase is **not another AI phase**. Phases H–K already define Hathor's canonical
> AI/application contract, MCP, language intelligence, deterministic completion, LLM/FIM ghost
> writing, completion UX, project-aware retrieval, and the conversational/agentic workflow. Phase L
> provides the traditional IDE capabilities that make the application feel like a complete
> professional IDE rather than an AI system attached to an editor. It does not duplicate H–K; where
> it exposes AI capabilities it consumes the existing canonical contract and contextual AI actions
> rather than creating a second AI subsystem.

### Phase L table

| ID  | Item                                   | Size   | Depends              |
| --- | -------------------------------------- | ------ | -------------------- |
| L-1 | Editor & workspace ergonomics          | large  | existing editor      |
| L-2 | Navigation & workspace search          | medium | L-1, existing LSP    |
| L-3 | Unified Problems / diagnostics surface | medium | L-2, AI-4, AI-5      |
| L-4 | Tasks + simple integrated terminal     | medium | L-1                  |
| L-5 | Git source control + history + graph   | large  | L-1                  |
| L-6 | Debugging + Hathor runtime inspection  | large  | L-3, L-4             |
| L-7 | Workspace/session persistence          | medium | L-1                  |
| L-8 | Unified contextual IDE actions         | medium | L-2, L-3, L-5, AI-10 |

> Every L item is a single-agent task unless an implementation dependency requires sequencing. Do
> not create additional phases for these capabilities.

### L-1 — Editor & Workspace Ergonomics

Complete the core editor experience so Hathor behaves like a serious IDE rather than a
single-document code editor.

Implement/audit:

* multi-tab editing
* tab selection, closing, reopening and pinning where appropriate
* split editor panes
* multiple editor groups where justified
* drag/rearrange tabs
* active-editor state
* cursor/selection preservation
* multi-cursor editing
* column/block selection where practical
* code folding
* bracket matching
* automatic indentation
* find/replace within the current document
* regex search where appropriate
* breadcrumbs/current-file context
* command palette
* keyboard shortcut/action registry
* quick actions
* editor context menus
* unsaved-change indicators
* dirty-state handling
* close/reload protection for unsaved changes
* basic formatting/indentation integration for supported languages
* efficient operation with large source files

Reuse the existing editor/document architecture. Do not replace the editor with another UI framework.

**Acceptance:** a user can work across multiple `.hathor`, `.ck`, and relevant project files without
repeatedly losing editor state or fighting the UI. Common editing/navigation actions are available
through keyboard shortcuts, menus, context menus, and the command palette.

### L-2 — Navigation & Workspace Search
Implement/audit:

- quick-open file search
- workspace-wide text search
- search across project files
- replace across project
- symbol search
- go to definition
- find references
- rename where the underlying language service supports it
- navigation history / back / forward
- line/column navigation
- source-location links from diagnostics
- integration with the existing Strudel LSP and ChucK/compiler intelligence

For `.hathor`, consume the existing Strudel LSP and Hathor supported-surface metadata already
defined by Phase H. Do not create a second language-navigation implementation. For ChucK, use
authoritative compiler/runtime information where available rather than inventing a parser solely for
navigation.

**Acceptance:** a user can move through a Hathor project using normal IDE navigation workflows
without needing the AI chat to locate files, definitions, references, or diagnostics.

### L-3 — Unified Problems / Diagnostics Surface
Create one coherent IDE-level Problems/Diagnostics experience that aggregates diagnostics from the
systems already defined elsewhere in the program. Sources include:

- Strudel LSP diagnostics
- Hathor-supported-surface validation
- real ChucK/libchuck compiler diagnostics
- C++/JUCE build diagnostics where available
- task/build/test failures
- relevant runtime errors
- ChucK worker/session failures
- other existing Hathor engine diagnostics

The Problems surface should support: error/warning counts, grouping by severity, grouping by file,
line/column locations, clickable navigation to source, filtering, persistent/current diagnostic
distinction where appropriate, refresh/revalidation, clear indication of diagnostic source, and
useful empty states. The diagnostic system must remain deterministic. Do not replace compiler/LSP
diagnostics with LLM-generated guesses.

**Bottom-ribbon integration.** Expose important global state through a compact bottom ribbon/status
area: at minimum icon-based indicators/actions for problems/errors/warnings count, source control
state, terminal/tasks where appropriate, relevant runtime/audio state, and other high-value IDE
status indicators discovered during implementation. Keep the ribbon compact and unobtrusive.

**Acceptance:** a user can immediately see whether the project has errors/warnings and click
directly into the relevant source. Compiler/LSP/runtime failures are not hidden inside separate
disconnected systems.

### L-4 — Tasks + Simple Integrated Terminal
Add a **simple integrated terminal**, treated as an advanced/developer escape hatch rather than a
primary musician workflow. The terminal exists for: project/build commands, tests, CMake/tooling,
Git operations not yet surfaced by the UI, developer troubleshooting, advanced ChucK/engine
workflows, diagnostics and recovery, and future development tooling.

**Architecture.**

- Build the terminal **UI natively with JUCE**; do not introduce FTXUI, cpp-terminal, Electron, or
  another UI framework.
- Do not implement a shell; use appropriate platform process/PTY facilities underneath the JUCE
  presentation layer.
- Keep terminal execution off the JUCE message/audio threads; capture stdout/stderr asynchronously.
- Support process cancellation and lifecycle cleanup.
- Terminal failure must not block or destabilize audio processing.
- Also provide a lightweight **task runner** for common project actions such as
  build/test/check where appropriate.

**Acceptance:** a developer can open a terminal tab/panel, run ordinary commands, see streaming output,
cancel a process, and continue running the IDE. Terminal activity cannot block the real-time audio
path.

### L-5 — Git Source Control, History & Graph

> **Git backend/library + native JUCE UI.** Do not build repository logic from scratch and do not
> make an external Git GUI the primary interface. Use a mature Git implementation/library such as
> `libgit2` if compatible with the project's dependency/licensing constraints; verify the dependency
> before adoption.

**1. Changes / Commit view — default.** When the Git panel is opened, the user sees the normal
working view:

```text
SOURCE CONTROL

Changes
  M  main.hathor
  M  acid_bass.ck
  A  new_instrument.ck

Staged Changes
  M  ...

[ Commit message ]

[ Commit ]
```

Support: repository status; modified/untracked/deleted/renamed files; staged vs unstaged changes;
stage file; unstage file; stage selected changes where practical; discard/revert with
appropriate confirmation; commit; commit message; push; pull/fetch; remote status; checkout/switch
branch; create branch; merge; merge conflict detection; conflict resolution workflow; remote sync;
repository initialization/opening where appropriate; useful Git errors and recovery states. Do not
require the user to open a terminal for ordinary Git workflows.

**2. History tab.** The Git panel has a **History** tab at the top alongside the normal
Changes/Commit view, it should provide: commit history, commit hash/identifier, author/date where
appropriate, commit message, changed files, ability to inspect a commit, ability to open a commit's
diff, and branch/tag/ref info where appropriate.

**3. Git Graph.** The History view includes a visual **Git graph**, displayed as an editor tab when
opened rather than permanently consuming the main editor area. It should visually represent: commit
nodes, branch lines, merges, diverging branches, converging branches, HEAD/current branch,
branch/ref labels where appropriate, commit messages to the right of their corresponding nodes, and
commit identity/details on selection. For a linear history the graph naturally appears as a single
straight line; for branching/merging history, lines diverge/converge so the user understands
history visually. The graph is an **IDE visualization**, not a replacement for Git's repository
model.

**Diff view.** Opening a changed file or commit provides a proper diff view with native JUCE UI;
side-by-side preferred where practical:

```text
OLD / HEAD                 CURRENT
────────────               ────────────
removed line               added line
unchanged                  unchanged
```

Provide: added/removed/modified line visualization, file-level diff, commit-level diff, read-only
diff mode, navigation between changed regions, and a clear relationship between the diff and Git
status/history.

**Ribbon.** Represent Git status as a compact icon in the bottom ribbon; clicking it opens the
source-control panel. Do not duplicate the entire Git UI in the ribbon.

**Acceptance:** a user can perform the normal Git lifecycle without leaving Hathor
(edit → inspect changes → stage → commit → create/switch branch → merge → resolve conflicts →
inspect history → inspect graph → push/pull). The History/Git Graph opens as an editor tab while the
normal Changes + Commit experience remains the default source-control panel.

### L-6 — Debugging & Hathor Runtime Inspection
Add debugging appropriate to both the underlying native application and Hathor's music runtime; do
not recreate every feature of a general-purpose debugger where it provides little value.

**Native/C++ debugging.** Where supported by the platform/toolchain, provide an integration surface
for breakpoints, continue/pause, step over/step into/step out, call stack, locals/data, and watches.
Integrate with existing native debugging infrastructure rather than writing a custom C++ debugger.

**Hathor runtime inspection.** Provide a Hathor-specific runtime-inspection surface for: current
playback state, current BPM, current cycle/beat where available, active pattern slots, active
events/voices where available, ChucK session state, active ChucK VMs/shreds, worker process state,
worker restart/crash state, relevant audio-engine state, and current diagnostics. This complements
L-3 rather than replacing it.

**Important.** Do **not** add a giant "AI Repair" button to the debugger/problems UI. AI remains
available only through the existing Phase H–K contextual AI/chat/action architecture. The
traditional debugger remains a deterministic IDE tool.

**Acceptance:** a developer can inspect why a build/runtime/audio problem occurred using deterministic
diagnostic/debugging information, while a musician can inspect useful musical/runtime state without
needing a conventional C++ debugger.

### L-7 — Workspace & Session Persistence
Persist the user's IDE workspace so closing and reopening Hathor restores the working environment.
Persist, where applicable: active project, open files, tab order, pinned tabs, split-pane layout,
active editor group, active file, cursor positions, selections, scroll positions,
expanded/collapsed explorer state, active panels, Git panel state, terminal/task state where safely
possible, editor preferences, and relevant runtime-independent UI state. Do not persist transient
audio/real-time state in a way that can accidentally restart unsafe runtime activity.

On restart: restore workspace/editor state; do not automatically restart ChucK/audio sessions
unless an explicit existing product decision permits it; handle deleted/moved files gracefully;
recover from stale workspace state without blocking startup.

**Acceptance:** the user can close Hathor, later reopen, and continue approximately where they left
off without manually reconstructing their workspace.

### L-8 — Unified Contextual IDE Actions
Create a consistent action model connecting the traditional IDE surfaces with the AI capabilities
already defined in Phases H–K. This is **UX integration, not a new AI subsystem**. Important IDE
objects expose appropriate contextual actions, e.g.:

```text
Diagnostic        →  Go to source · Explain with AI · Open in Problems
Selection          →  Copy · Search · Go to definition · Find references · Ask AI
Pattern (.hathor)  →  Inspect · Search related project context · Ask AI
ChucK  instrument  →  Compile · Audition · Inspect diagnostics · Ask AI
Git change        →  Open path · Stage/unstage · Revert · Ask AI about change
Failure           →  Open source · Open Problems · Ask AI
```

The AI actions must use the canonical H–K AI/application contract and existing authorization/safety
model. Do not create an unrestricted "AI can do anything from this UI" shortcut. For destructive
actions, preserve the existing AI-1 authorization/approval boundaries.

**Acceptance:** the IDE feels coherent — users move naturally between deterministic tools and AI
assistance without needing to understand which subsystem owns the capability.

### Phase L dependency / execution order

```text
L-1 Editor & Workspace
        │
        ├──→ L-2 Navigation/Search
        │        │
        │        └──→ L-3 Problems/Diagnostics
        ├──→ L-4 Tasks/Terminal
        ├──→ L-5 Git
        └──→ L-7 Workspace Persistence

L-3 + L-4 ──→ L-6 Debugging/Runtime Inspection
L-2 + L-3 + L-5 + H–K ──→ L-8 Contextual IDE Actions
```

Where tasks are genuinely independent they may run in parallel; do not parallelize work that
modifies the same editor/workspace infrastructure without a clear ownership boundary.

### Phase L architectural constraints

1. **JUCE-native UI.** All visible IDE UI uses JUCE components; do not introduce a second
   framework for IDE panels.
2. **Reuse mature infrastructure.** Git → a mature library such as `libgit2` (after verification);
   language intelligence → the existing Strudel LSP (Phase H); ChucK diagnostics → the vendored
   compiler/runtime (Phase H); native debugging → existing toolchain/debugger facilities.
3. **No extension ecosystem.** No marketplace, plugin API, or VS Code compatibility layer; Hathor
   remains a focused music IDE.
4. **No duplicate AI.** Phase L consumes H–K; no second chat, agent, retrieval, or authorization
   system.
5. **No audio-thread contamination.** Git, terminal, indexing, diagnostics, persistence, and debug
   UI must not block the real-time audio thread (reuse the audio-worker/ownership model from Phase C/B7/B8).
6. **Deterministic tooling stays deterministic.** LSP/compiler/debugger/Git results are never
   replaced by probabilistic AI output.
7. **Musician-first complexity.** Advanced developer functionality exists but stays out of the
   primary music workflow; terminal/debugger complexity is progressively exposed.

### Phase L verification / Definition of Done

Phase L is complete only when the IDE can demonstrate all of the following:

- **Editor** — [ ] multiple files/tabs work; [ ] split editing works; [ ] common editing operations
  work without document corruption; [ ] unsaved changes are protected; [ ] shortcuts and command
  palette are reliable.
- **Navigation** — [ ] file search works workspace-wide; [ ] workspace text search works;
  [ ] symbol navigation works where supported; [ ] go-to-definition/reference integrates with
  language intelligence; [ ] diagnostics link directly to source.
- **Diagnostics** — [ ] Strudel/LSP diagnostics appear in Problems; [ ] ChucK real-compiler
  diagnostics appear in Problems; [ ] build/task/runtime failures appear in Problems;
  [ ] error/warning counts show in the bottom ribbon; [ ] clicking a problem opens the source.
- **Terminal/tasks** — [ ] terminal runs asynchronously; [ ] stdout/stderr stream correctly;
  [ ] processes can be cancelled; [ ] terminal cannot block the audio thread; [ ] common build/test
  tasks can be launched from the IDE.
- **Git** — [ ] status, stage/unstage, commit, branch create/switch, merge, conflict detection +
  resolution, fetch/pull/push all work; [ ] history is a dedicated tab; [ ] Git graph renders linear
  and branching/merging history; [ ] commit messages appear beside graph nodes; [ ] commit and
  working-tree diffs are inspectable; [ ] Git status is in the bottom ribbon.
- **Debugging/runtime** — [ ] native debugger integration works where supported; [ ] Hathor runtime
  state inspectable; [ ] ChucK session/worker state visible; [ ] debugging cannot destabilize the
  audio engine.
- **Persistence** — [ ] workspace restores after restart; [ ] tabs/layout/cursor restore;
  [ ] stale/missing files do not break startup; [ ] audio/ChucK runtime does not restart
  unexpectedly.

Then run a final **Traditional IDE Parity Audit** against the workflows a professional user
performs (`open → search → edit multiple files → navigate → encounter diagnostics → fix → build →
task/terminal → inspect Git → stage/commit → branch → merge → resolve → history → graph → debug →
inspect → persist → resume`), classifying each step as implemented / already existed / partially
implemented / not applicable / missing. Do not declare Phase L complete merely because components
exist — demonstrate the end-to-end workflows.

> **Success criterion.** The end state is not "Hathor becomes VS Code." It is: *Hathor is a
> professional native music IDE whose traditional editor/workspace is strong enough that users do
> not need a second general-purpose IDE for normal Hathor development, while its AI layer remains
> integrated through the existing H–K architecture.* A real IDE first, a music environment
> second, and an AI-native environment throughout — without becoming a generic developer platform.

---

## 4. Cross-cutting design decisions for items that touch shared code

These answers make A1/A2/B5 safe to implement without a full separate spec; read the relevant
block before implementing that item.

### §4.1 Theming (for A1, B3)
- Introduce a single flat `Theme` struct with a **complete token set identical in shape for all 5
  themes**, instantiated as values (not derived). Include at least:
  background/surface/surfaceLow/surfaceContainer/High/Highest/Bright; textPrimary/textSecondary/
  textMuted/Disabled; accent; error; warning; codeText/codeKeyword/codeType/codeString/
  codeFunction/codeMacro/codeBracket/codeLine; plus shared spacing/radius numbers.
- The LookAndFeel holds a `currentPalette`; components fetch colour values through the LookAndFeel
  (e.g. `lookAndFeel.getPalette().surface`).
- Switch = replace `currentPalette` object + `getLookAndFeel().sendLookAndFeelChange()`.

### §B2/§C1 — Now playing, zero-alloc contract (for B2)
- The new `Event` offset field **must** be a small trivial scalar (`int32/uint32` or `size_t`).
  This keeps `sizeof(Event<…>)` small, so the three inline stores and the SBO `std::function`
  captures stay unchanged in behaviour; the program keeps no `new` on the audio thread.
- Adding a second parallel inline array to `VisualizerFrame` is acceptable, but we prefer
  widening the by-value Event; either is allocation-free.

---

## 5. Parallel-execution / agent map (waves)

| Wave | Items (this wave) | Guidance |
|------|-------------------|----------|
| 1 (sequential) | H0, H1 | **do not over-partition** — these are gates, verify by human |
| 2 (parallel) | A1, A2, A3, A4, A5, A6 | A1+A2 share `ui` (separate files: LookAndFeel vs AppSettings; OK parallel); A3 engine; A5 grammar; A4 explorer |
| 3 (parallel) | B3, B5 (need A1+A2); B1 (need A3) | run in parallel, ~3 |
| 4 (parallel) | B6 (need H0); B2 (need A3, own pass) | ~2–3 |
| own sub-phases | **B4 (Real ChucK, V2)** → **B8 (Bake-to-Song / assets)** → **B7 (Real EQ)** — each a sequential sub-phase (K0/K1→Kn) after their deps land (B4 after A5/A6/A3 + worker infra + B4-K0.5/K0.6 spikes; B8 after B4 + A4; B7 after A2). Every sub-item is its own single-agent task (decisions #11, #12). | run the sub-phases in their own lane; B4-K0.5 (thread-safety) then **B4-K0.6 (audio IPC spike) precede B4-K2+**; B4-K2 gates B4-K3/K6 |
| 5 (sequential in-lane) | D1→D2→D3; D4 parallel to D2/D3 | 1–2 focused |
| Phase H (sequential) | AI-1 → AI-2…AI-8 (in order; AI-1 contract precedes all) | one agent per AI item; AI-1 first; **run the §Verification-first spike before AI-3/AI-4/AI-5** |
| Phase I (sequential) | AI-9 → AI-G1…AI-G9 (ghost-writing, after AI-9) | AI-G1 before AI-G2…G9; verify llm-ls before AI-G1 |
| Phase J (ordered) | J-1 → J-2 → J-3 → J-5/J-6 (small/parallel-able) | J-1 first; J-5 can run alongside J-2/J-3 |
| Phase K (sequential in-lane) | AI-10 → AI-10.1…AI-10.6 (agent loop first, spec sub-items in order) | AI-10 loop + safety before any sub-spec; nothing destructive without approval |
| Phase L (ordered, after H–K) | L-1 → L-2 → L-3 → L-6; L-4/L-5/L-7 parallel to L-2/L-3; L-8 last | L-1 first (everything depends on editor); L-8 after L-2/L-3/L-5 + H–K; do not parallelize work on the same editor/workspace surface |

Max useful parallelism ≈ **5–6**, matching Phase 1/2 layering ceiling. Never parallelize H0/H1
internally.

---

## 6. Definition of Done (DoD)

An item is **done** only when **all** hold:

1. Its acceptance criteria are met.
2. Any "human verification (gate)" step is actually performed and logged (H0/H1 are hard gates;
   every item marked "verify" applies to human logging). **B4-K8's ChucK safety test is also a
   hard gate, not optional:** a deliberately hung/runaway `.ck` file — and a natively-crashing
   `.ck`/worker — must neither hang nor crash the JUCE audio thread or the rest of Hathor.
3. Supporting unit / property additions land in `tests/` or `tests-ui/` and pass `ctest`. Additions
   specific to B4 (shred heartbeat/watchdog/crash-isolation/worker-restart + **shared-memory
   recovery**), B7 (filter coefficients), and B8 (bake render/bind) are mandatory, not advisory —
   see §Phase C (B4) / §Phase D (B7) / §Phase E (B8).
4. **Success is declared on evidence, not on code existing (PART H).** At minimum the program-level
   acceptance for these areas must be demonstrated, not assumed:
   - **Authoring intelligence:** the Strudel LSP actually provides deterministic `.hathor`
     completion; ChucK diagnostics originate from the real compiler; AI ghost completion works
     through FIM; Hathor-specific context reaches the completion model; project-aware context is
     relevant and bounded; ghost text does not mutate document state until accepted; stale
     completion requests are cancelled/discarded; deterministic and AI completion coexist correctly.
   - **Audio IPC (B4-K0.6/B4-K2):** two-process shared-memory audio transport demonstrated;
     real-time constraints tested; worker crash tested; **mid-write worker death** tested;
     shared-memory recovery tested; no indefinite main-process/audio-thread waits; restart
     demonstrated.
   - **Synchronization (B4-K6):** event timestamps explicitly defined; timestamped scheduling works
     across the process boundary; IPC arrival timing is not used as musical timing; late/early event
     behaviour defined; sample-accurate behaviour is demonstrated **or its measured limitation
     explicitly documented**.
   - **Per-tab resources (B4-K3):** concurrent-VM resource cost measured; inactive-tab policy
     defined; resource ceiling/policy documented; activation/deactivation behaviour tested.
4. Code compiles with `-Wall -Wextra -Werror`; no silent affordance regressions (decision #6) —
   if a control can't do its job, it says why (tooltip/text), never goes unresponsive.
5. Docs updated if behavior changed (esp. `ui/DESIGN_TOKENS.md` for A1/B3; new for opacity).
6. Any `file:line` citation this document relies on for an item is re-verified against current
   source before that item's implementation (audit citations drift once Phase 0 ships).

---

## 7. Tracking index

- [~] **H0** ACP accept loop · `control/ui` · gate (impl done; human verify pending)
- [ ] **H1** Explorer + Ribbon wiring · `ui` · gate
- [ ] **A1** theme engine · `ui`
- [ ] **A2** settings tab (Appearance/Agent/Petdex) + Apply/Reset · `ui`
- [ ] **A3** per-slot play/stop · `engine/audio/control`
- [ ] **A4** recursive Explorer tree · `ui`
- [ ] **A5** ChucK grammar → tokeniser · `ui`
- [ ] **A6** `docs/future-chuck-integration.md` · `docs`
- [ ] **B1** per-tab Play/Stop · `audio/ui/control`
- [ ] **B2** now-playing pipeline · `engine/audio/ui` · own pass
- [ ] **B3** theme picker · `ui`
- [ ] **B4** real ChucK (V2) · own sub-phase (B4-K0.5…K8) · `engine/audio/ui` + worker · **B4-K0.6 IPC spike gates B4-K2**
- [ ] **B5** opacity slider · `ui`
- [ ] **B6** chat thread tabs · `control/ui`
- [ ] **B7** real EQ · own sub-phase (B7-K1…K4) · `engine/audio/ui`
- [ ] **B8** Bake to Song / asset lifecycle · own sub-phase (B8-K1…K6) · `ui/audio`
- [ ] **C1** editor now-play highlight · `ui`
- [ ] **C2** chat reconnect/thread polish · `ui`
- [ ] **D1–D4** Petdex (own pass)
- [ ] **AI-1** canonical AI/agent contract · `engine`
- [ ] **AI-2** read-only project intelligence · `control`
- [ ] **AI-3** language-intelligence foundation (existing Strudel LSP + versioned surface metadata) · `engine`/`ui`
- [ ] **AI-4** LSP-powered editor completion/hover/diagnostics · `ui`
- [ ] **AI-5** ChucK lifecycle tools + real-compiler diagnostics (async jobs) · `control ui`
- [ ] **AI-6** rendering / asset lifecycle · `control ui audio`
- [ ] **AI-7** safe song mutation · `engine control`
- [ ] **AI-8** project-aware AI context · `ui engine`
- [ ] **AI-9** inline AI completion · `ui`
- [ ] **AI-10** agentic musical workflow · `all`
- [ ] **AI-G1** ghost-writing: integrate `llm-ls` (or equivalent FIM server) · `engine`/`ui`
- [ ] **AI-G2** Fill-in-the-Middle as a first-class requirement · `engine`
- [ ] **AI-G3** Hathor-specific AI-context provider on top of llm-ls · `engine`/`ui`
- [ ] **AI-G4** few-shot / domain-specific completion context · `engine`
- [ ] **AI-G5** deterministic + ghost completion coexist (UI precedence/cancel/invalidate) · `ui`
- [ ] **AI-G6** ghost text is UI state, not document state · `ui`
- [ ] **AI-G7** ChucK authoring assistance (reuse-or-minimize; no bespoke ChucK LSP) · `engine`/`ui`
- [ ] **AI-G8** preserve Strudel LSP decision (`.hathor` = standard Strudel mini-notation) · ack
- [ ] **AI-G9** final Track B authoring stack (diagram/dependency) · docs
- [ ] **J-1** completion triggering policy · `ui engine`
- [ ] **J-2** multiple candidates & cycling · `ui`
- [ ] **J-3** partial / multi-token acceptance · `ui`
- [ ] **J-4** selection-aware & intent-aware completion (folded into AI-9/J-1) · `ui` (no new task)
- [ ] **J-5** codebase / project retrieval for completion · `engine`
- [ ] **J-6** completion-quality feedback loop · `ui engine`
- [ ] **AI-10.1** agent: natural-language intent → actionable plan · `all`
- [ ] **AI-10.2** agent: conversational memory / working set · `engine`
- [ ] **AI-10.3** agent: first-class diff / preview / undo · `ui engine`
- [ ] **AI-10.4** agent: explains what it's doing (progress events) · `ui`
- [ ] **AI-10.5** agent: conversational repair (creative vs technical) · `engine`
- [ ] **AI-10.6** agent: agent-task lifecycle (states / cancel / approval) · `control engine`
- [ ] **L-1** editor & workspace ergonomics · `ui`
- [ ] **L-2** navigation & workspace search · `ui engine`
- [ ] **L-3** unified Problems / diagnostics surface (+ bottom ribbon) · `ui engine control`
- [ ] **L-4** tasks + simple integrated terminal · `ui control`
- [ ] **L-5** git source control + history + graph · `ui engine`
- [ ] **L-6** debugging + Hathor runtime inspection · `ui control engine`
- [ ] **L-7** workspace/session persistence · `ui engine`
- [ ] **L-8** unified contextual IDE actions · `ui`

---

_End — authoritative source for Phase 2.5. If source code contradicts a statement here, fix the
code and update this document, not the other way around._