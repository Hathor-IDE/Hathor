# AI-G9: Final Track B Authoring Intelligence Stack

## Status

**Implemented.** This document, together with the companion machine-readable
dependency graph at `docs/design/ai-g9-dependency-graph.json`, closes Track B by
representing the complete Hathor authoring-intelligence architecture and its
dependencies. The JSON graph is the single source of truth for implementation
order and architectural relationships; this document is the human-readable
companion that explains the layering, guarantees, and data flow.

---

## Goal

Close Track B against the common architectural mistake of treating "LSP
integration" as a replacement for "AI ghost writing." The Strudel LSP
provides **deterministic** language intelligence; `llm-ls`/FIM provides
**probabilistic** ghost generation. They are parallel, complementary systems.
This dependency graph makes that impossibility structural and visible.

---

## Canonical Stack — TRACK B — AUTHORING INTELLIGENCE

```
TRACK B — AUTHORING INTELLIGENCE

Language Knowledge
├── Strudel LSP
├── ChucK real-compiler diagnostics
├── Hathor supported-surface metadata
└── Metadata/schema/version compatibility

Deterministic Completion
├── Strudel/Tidal functions
├── mini-notation constructs
├── samples
├── scales
├── project assets
├── ChucK constructs
└── hover/documentation

Project-Aware Context
├── current file
├── cursor / selection
├── surrounding code
├── project structure
├── samples
├── rendered instruments
├── patterns
├── BPM
├── scales
└── diagnostics

AI Ghost Writing
├── llm-ls
├── FIM prefix/suffix/middle
├── Hathor authoring context
├── few-shot examples
├── ChucK context
├── Strudel context
└── inline ghost rendering

AI Repair
└── generate
      ↓
    validate
      ↓
    diagnose
      ↓
    repair
      ↓
    validate again
      ↓
    audition
```

Each layer has exactly one responsibility. No layer crosses into another's
territory. The **Language Knowledge** layer is the authority for language facts
(an external LSP + real compiler + versioned metadata). The **Deterministic
Completion** layer consumes that knowledge for exact, predictable editor
features (never an LLM). The **Project-Aware Context** layer assembles compact,
location-specific context for the AI. The **AI Ghost Writing** layer performs
probabilistic generation via `llm-ls`/FIM (UI state only). The **AI Repair**
layer closes the loop with real validation and audition.

---

## Architectural Separation — Data Flow

```
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
                                          │
                                          ▼
                                   Ghost text overlay (UI state)
                                          │
                    (on accept only) ───► document
                                          │
                                          ▼
                                   Real validation / diagnostics
                                          │
                                          ▼
                                   Repair loop
                                          │
                                          ▼
                                    Audition (ChucK session)
```

The machine-readable version of this separation — 10 ordered stages with their
serving nodes and explicit separation guarantees — is encoded in the
**`architectural_separation`** section of
`docs/design/ai-g9-dependency-graph.json`. The six separation guarantees
encoded there are:

1. Language intelligence (Strudel LSP + libchuck + metadata) is distinct from AI generation (llm-ls/FIM).
2. Deterministic authoring (exact completions) is distinct from AI ghost writing (probabilistic generation).
3. Project/context retrieval feeds AI generation, never replaces it.
4. The LSP does not produce ghost text; llm-ls does not produce deterministic completions.
5. Generated code is validated by the real compilers and runtime engine, not by metadata or an approximate parser.
6. Ghost text is UI state (AI-G6) — it never touches the CodeDocument, undo history, compiler input, or diagnostics until explicitly accepted.

### Layer responsibilities

| Layer | Component | Responsibility | Source of truth |
|---|---|---|---|
| **Language Knowledge** | Strudel LSP (`strudel-lsp-server.cjs`) | Deterministic `.hathor` completion, hover, diagnostics, discovery | Upstream Strudel/TypeScript |
| | Real libchuck compiler (`ChuckSessionService`) | Authoritative `.ck` diagnostics via `validateChuckSource()` | Vendored libchuck in `control/` |
| | Versioned `LanguageMetadata` | What Hathor supports; version compatibility manifest | `reference/language-metadata/HathorLanguageMetadata.json` |
| **Deterministic Completion** | `HathorLspClient` / `LspCompletionLogic` (UI) | L1–L3 completion, hover, diagnostics from the LSP (with metadata fallback) | Strudel LSP + `LanguageMetadata` |
| | `ChuckTokeniser` / `ChuckKeywords` (UI) | Deterministic `.ck` completion + syntax highlighting | `LanguageMetadata::chuckApi` |
| **Project-Aware Context** | `AuthoringContext` (control) | Assemble targeted context for MCP / AI requests | `EditorContextProvider` + `LspContextProvider` + `ProjectReadFacade` + AI-3 metadata |
| | `CompletionContextProvider` (control) | Location-specific context for llm-ls FIM requests (AI-G3) | Reuses AI-8 providers + AI-2 `ProjectReadFacade` + AI-3 metadata |
| | `FewShotCorpus` (engine) | Versioned few-shot examples, version-gated | `reference/language-metadata/HathorFewShotExamples.json` |
| **AI Ghost Writing** | `GhostLlmClient` / `GhostCompletionLogic` (UI) | llm-ls lifecycle: launch, communicate, cancel, debounce, stale-drop, latency | `llm-ls` + env-var `GhostProviderConfig` |
| | `GhostTextOverlay` (UI) | Inline ghost-text rendering as UI state only (never touches `CodeDocument`) | `GhostCompletionLogic` |
| | `CompletionCoordinator` (UI) | Deterministic + ghost coexistence (precedence / cancel / invalidate) | `GhostCompletionLogic` + `HathorLspClient` |
| **AI Repair** | `SongMutationService` (control) | Safe, transactional, validated `edit_song` (AI-7) | AI-1 capability model |
| | `RenderService` (control) | Render + commit + overwrite-safety for `.ck` assets (AI-6) | AI-5 ChucK sessions + B8 bake pipeline |
| | `ChuckSessionService` (control) | Compile / audition / stop isolated ChucK sessions (AI-5) | B4-K3 VM isolation |

**Runtime authority** (separate from authoring):
```
.hathor source ──► Hathor C++ parser/engine ──► runtime interpretation
                        │
                        └─── validated against ──► Strudel golden fixtures
```

The Strudel LSP, Hathor metadata, and AI ghost layer are **authoring-time**
services. They improve the editing experience but carry no runtime authority.
The C++ engine is the sole runtime source of truth. If the LSP or metadata
suggest a construct, the engine must still independently parse and interpret
it.

---

## Dependency Graph

### Full dependency graph (machine-readable)

The complete, machine-readable dependency graph — including all 18 AI nodes,
3 external nodes (H0, B4, B8), 48 edges (34 `depends_on`, 3
`external_dependency`, 11 `realizes`), 7 layer mappings, 8 architectural
guarantees, and an 18-step implementation order — is encoded in:

```
docs/design/ai-g9-dependency-graph.json
```

### Implementation order

| Order | Item | Phase | Rationale |
|---|---|---|---|
| 1 | AI-1 | H | Canonical contract — everything sits on this foundation |
| 2 | AI-2 | H | Give AI eyes first (read-only project intelligence) |
| 3 | AI-3 | H | Language intelligence foundation — versioned metadata + Strudel LSP confirmed |
| 4 | AI-4 | H | LSP-powered deterministic editor integration (depends on AI-3) |
| 5 | AI-5 | H | ChucK lifecycle + real-compiler diagnostics (depends on AI-2 + B4) |
| 6 | AI-8 | H | Project-aware AI context provider (depends on AI-3 + AI-2) |
| 7 | AI-7 | H | Safe song mutation (depends on AI-2) |
| 8 | AI-6 | H | Rendering/asset lifecycle (depends on AI-5 + B8) |
| 9 | AI-9 | I | Inline AI completion entry point (depends on AI-8 + AI-4) |
| 10 | AI-G1 | I | Integrate llm-ls/FIM for ghost writing (depends on AI-9 + AI-3) |
| 11 | AI-G2 | I | FIM prefix/suffix/middle (depends on AI-G1) |
| 12 | AI-G3 | I | Location-specific context provider (depends on AI-G1 + AI-3) |
| 13 | AI-G4 | I | Versioned few-shot examples (depends on AI-G3 + AI-3 + AI-G1) |
| 14 | AI-G5 | I | Deterministic + ghost coexistence (depends on AI-4 + AI-G1) |
| 15 | AI-G6 | I | Ghost text as UI state (depends on AI-G1 + AI-G5) |
| 16 | AI-G8 | I | Preserve Strudel LSP decision (depends on AI-3) — ack |
| 17 | AI-G7 | I | ChucK authoring assistance (depends on AI-G3 + AI-5 + AI-3) |
| 18 | AI-G9 | I | Final Track B architecture + dependency graph (depends on AI-G1…AI-G8) |

### Dependency edges (abbreviated)

The graph records these dependencies at minimum:

```
AI-3
  ├──► AI-4                              (metadata → LSP editor integration)
  ├──► AI-8                              (metadata → context provider)
  ├──► AI-G3                             (metadata → location-specific context)
  ├──► AI-G4                             (versioned metadata → version-gated few-shots)
  ├──► AI-G7                             (ChucK API metadata → ChucK authoring)
  └──► AI-G8                             (preserves Strudel LSP decision)

AI-4
  └──► deterministic editor intelligence  (realization: AI-4 IS the deterministic completion layer)

AI-5
  └──► authoritative ChucK diagnostics   (realization: AI-5 is the real-compiler diagnostic authority)

AI-7
  └──► safe structured mutation           (realization: AI-7 powers the repair loop's mutation step)

AI-8
  └──► shared editor/language context     (realization: AI-8 assembles the shared context)

AI-G1
  ├──► AI-G2                              (llm-ls → FIM protocol)
  ├──► AI-G3                              (llm-ls → context injection)
  ├──► AI-G4                              (llm-ls → few-shot flow)
  ├──► AI-G5                              (llm-ls → coexistence lifecycle)
  └──► AI-G6                              (llm-ls → ghost-text lifecycle)

AI-G3
  └──► location-specific Hathor context for ghost writing

AI-G4
  └──► versioned few-shot/domain examples

AI-G5 + AI-G6
  └──► safe deterministic/ghost coexistence and document-state separation

AI-G7
  └──► ChucK deterministic + AI authoring

AI-G8
  └──► preserves standard Strudel/LSP architecture

AI-G9
  └──► final Track B architecture
```

Full dependency chain (including Phase H foundation and Phase I ghost-writing):

```
H0
 └──► AI-1 (canonical contract)
        ├──► AI-2 (read-only project intelligence)
        │     ├──► AI-5 (ChucK lifecycle + real-compiler diagnostics) [+B4]
        │     │     └──► AI-6 (rendering/asset lifecycle) [+B8]
        │     └──► AI-7 (safe structured mutation)
        └──► AI-3 (language intelligence foundation)
              ├──► AI-4 (LSP-powered deterministic editor integration)
              │     └──► AI-9 (inline AI completion entry point) ──┐
              ├──► AI-8 (project-aware context provider)          │
              │     └──► AI-9 ─────────────────────────────────────┤
              ├──► AI-G3 (location-specific context)               │
              │     └──► AI-G4 (versioned few-shot examples)       │
              ├──► AI-G7 (ChucK deterministic + AI authoring)        │
              └──► AI-G8 (preserve Strudel LSP decision)            │
                                                                   │
              AI-9 ──► AI-G1 (integrate llm-ls/FIM) ───────────────┤
                          ├──► AI-G2 (FIM prefix/suffix/middle)     │
                          ├──► AI-G3 (reuses AI-G1 providers)        │
                          ├──► AI-G4 (few-shot flow)                 │
                          ├──► AI-G5 (deterministic/ghost coexist)  │
                          │     └──► AI-G6 (ghost text = UI state)   │
                          └──► AI-G7 (ChucK metadata + compiler)    │
                                                                   │
              All AI-G1…AI-G8 ──► AI-G9 (final Track B architecture)◄┘
```

### Layer-to-component mapping

| Canonical Layer | Nodes | Key Implementation |
|---|---|---|
| **Language Knowledge** | AI-3, AI-5, AI-G8 | `LanguageMetadata.cpp`, `ChuckSessionService.hpp`, `ai-g8-strudel-lsp-decision.md` |
| **Deterministic Completion** | AI-4, AI-G7 | `HathorLspClient.hpp`, `LspCompletionLogic.hpp`, `ChuckKeywords.hpp` |
| **Project-Aware Context** | AI-2, AI-8, AI-G3 | `ProjectReadFacade.hpp`, `AuthoringContext.hpp`, `CompletionContextProvider.hpp` |
| **AI Ghost Writing** | AI-9, AI-G1, AI-G2, AI-G4, AI-G6 | `GhostCompletionLogic.hpp`, `GhostLlmClient.hpp`, `GhostTextOverlay.hpp`, `FewShotCorpus.cpp` |
| **Ghost/Deterministic Coexistence** | AI-G5, AI-G6 | `CompletionCoordinator.hpp`, `GhostTextOverlay.hpp` |
| **AI Repair** | AI-7, AI-6 | `SongMutationService.hpp`, `RenderService.hpp` |
| **Architectural Foundation** | AI-1, AI-G9 | `ControlInterface.hpp`, `ai-g9-dependency-graph.json`, this file |

---

## Mandatory Architectural Guarantee

> **"LSP integration" MUST NEVER be treated as equivalent to "ghost writing."**

This is decision #19 made structurally impossible by the dependency graph:

1. **Distinct layers.** The Strudel LSP is in **Language Knowledge** +
   **Deterministic Completion**. `llm-ls`/FIM is in **AI Ghost Writing**. They
   do not share a layer. The coexistence mechanism (AI-G5) lives in the border
   zone and is explicitly about *keeping them separate*, not merging them.

2. **Distinct code paths.** Deterministic completion flows through
   `HathorLspClient` → `LspCompletionLogic` (LSP protocol). Ghost writing
   flows through `GhostLlmClient` → `GhostCompletionLogic` →
   `GhostTextOverlay` (JSON-RPC to llm-ls). `CompletionCoordinator` enforces
   mutual exclusion, not unification.

3. **Distinct dependency edges.** In the graph, AI-4 (LSP integration) and
   AI-G1 (llm-ls integration) are **siblings** that both depend on AI-3. Neither
   is a prerequisite of the other. There is no edge from AI-4 to AI-G1 or
   vice-versa. They feed a common parent (AI-9) as parallel inputs.

4. **Distinct responsibilities.** AI-4 produces exact, predictable completions
   (LSP protocol). AI-G1 produces probabilistic, multi-token, multi-line
   continuations (FIM). AI-G6 ensures ghost text is UI state, never document
   state — so LSP completion and ghost text cannot collide in the document.

5. **Distinct data flow.** The LSP returns structured completion items that
   the editor commits immediately. `llm-ls` returns `generated_text` that is
   rendered as a non-document overlay, accepted only by explicit user action.

---

## Forbidden Actions

The following actions are **forbidden** under this decision. If any would be
needed, the architectural guardrail must be revised via a formal decision
process — not silently acted upon:

1. **No duplicate language-knowledge stores.** Do not create a second
   `LanguageMetadata` or a second copy of Strudel/Tidal/CK definitions. AI-3
   is the single source. AI-G7's ChucK metadata reuses AI-3's
   `LanguageMetadata::chuckApi`.

2. **No second AI completion architecture.** All ghost writing routes through
   `llm-ls`/FIM (AI-G1). Do not build a custom LLM completion server
   (decision #20).

3. **No routing ghost writing through the deterministic system.** Ghost text
   (AI-G6) is never inserted into the document via the LSP completion path.
   `CompletionCoordinator` enforces this.

4. **No LSP responsibility for AI generation.** The Strudel LSP does not and
   cannot produce ghost text. It returns deterministic completion items; AI-G1
   returns probabilistic completions. They are never merged into one system.

5. **No MCP as language-intelligence authority.** MCP is the agent
   capability/interface layer (AI-1 capability model). It reaches language
   intelligence through the LSP→MCP bridge (AI-8). MCP never owns a copy of
   language definitions.

6. **No metadata as compiler diagnostics.** AI-3 `LanguageMetadata` is a
   compatibility/version manifest. It is never a diagnostic engine. ChucK
   diagnostics come from the real libchuck compiler (AI-5). Strudel diagnostics
   come from the LSP (AI-4). Metadata only gates which definitions are considered
   current.

7. **No service-boundary collapse.** Do not let the editor, AI layer, runtime
   engine, compiler, or MCP each develop their own model of Hathor. AI-1's
   shared service interface is consumed by all four callers.

---

## Relationship to Other AI-G Tasks

| Task | Relationship to AI-G9 |
|---|---|
| **AI-1** | Canonical contract that defines the service boundaries AI-G9 maps onto. |
| **AI-2** | Read-only project intelligence; feeds AI-8 and AI-G3 context assembly. |
| **AI-3** | Language knowledge foundation; the metadata version-gate that AI-G3/AI-G4/AI-G7 respect and that AI-G9 makes explicit. |
| **AI-4** | Deterministic `.hathor` completion; the sibling to AI-G1 ghost writing that AI-G9 keeps structurally separate. |
| **AI-5** | Real-compiler ChucK diagnostics; authoritative validation source for AI-G7 and the AI Repair layer. |
| **AI-6** | Asset lifecycle (renders → audition); part of the AI Repair loop AI-G9 closes. |
| **AI-7** | Safe structured mutation; the "repair" step in AI-G9's repair loop. |
| **AI-8** | Project-aware context; the shared context foundation that AI-G3 builds on. |
| **AI-9** | Inline AI completion entry point; the parent that AI-G1 spawns from. |
| **AI-G1** | llm-ls integration; the single AI generation point AI-G9 consolidates. |
| **AI-G2** | FIM design; the prefix/suffix/middle mechanism AI-G9 represents. |
| **AI-G3** | Location-specific context; the bounded retrieval AI-G9 keeps distinct from whole-repo dumps. |
| **AI-G4** | Versioned few-shots; the domain examples AI-G9 keeps version-gated. |
| **AI-G5** | Deterministic/ghost coexistence rules; the mutual-exclusion AI-G9 enforces structurally. |
| **AI-G6** | Ghost-text-as-UI-state; the document-state separation AI-G9 guarantees. |
| **AI-G7** | ChucK authoring; the case study AI-G9 uses to show deterministic + AI coexist without collapsing. |
| **AI-G8** | Strudel LSP preservation; the foundational constraint AI-G9 incorporates (no custom grammar/LSP). |

---

## Acceptance

- [x] The entire Track B authoring stack is represented in the program
      dependency graph (`ai-g9-dependency-graph.json` with 18 AI nodes + 3
      external nodes + 48 edges).
- [x] Every layer has one clearly defined responsibility (canonical_stack +
      layer_mapping sections).
- [x] Strudel LSP, ChucK diagnostics/language intelligence, Hathor metadata,
      project context, llm-ls/FIM, deterministic completion, ghost rendering,
      and AI repair are represented as distinct but connected layers.
- [x] The architecture makes it impossible to interpret LSP integration as a
      substitute for ghost writing (distinct layers, distinct code paths,
      sibling dependency edges, mandatory architectural guarantee, forbidden
      actions).
- [x] The dependency graph accurately reflects the implementation dependencies
      established by AI-1 through AI-G8 (edges validated against PROGRAM.md
      Phase H and Phase I tables).

---

## References

- `docs/PROGRAM.md` — Decision #14 (MCP is not where Hathor's intelligence
  lives), #15 (deterministic precedes LLM), #16 (reuse existing language
  intelligence), #17 (LSP ≠ MCP), #18 (versioned metadata; Strudel golden
  standard), #19 (LSP and AI ghost-writing are separate), #20 (reuse
  llm-ls/FIM; FIM is first-class), #21 (ChucK needs full authoring stack).
- `docs/PROGRAM.md` — Phase H table (AI-1…AI-8), Phase I table (AI-9, AI-G1…AI-G9).
- `docs/design/ai-g7-chuck-authoring.md` — AI-G7: no reusable ChucK LSP; real
  libchuck compiler + metadata fallback.
- `docs/design/ai-g8-strudel-lsp-decision.md` — AI-G8: `.hathor` = standard
  Strudel mini-notation; LSP is authoring-time, not runtime.
- `docs/llm-ls-integration-verification.md` — AI-G1: verified llm-ls FIM protocol
  surface (getCompletions, acceptCompletion, rejectCompletion, document sync).
- `reference/language-metadata/HathorLanguageMetadata.json` — AI-3 versioned
  supported-surface metadata.
- `reference/language-metadata/HathorFewShotExamples.json` — AI-G4 versioned
  few-shot corpus.
- `reference/strudel-lsp/strudel-lsp-server.cjs` — The reused Strudel LSP.
- `reference/strudel-golden/` — Golden fixtures for differential validation of
  the Hathor C++ parser against upstream Strudel.
- `control/LspContextProvider.hpp` — AI-8 LSP context abstraction boundary.
- `control/EditorContextProvider.hpp` — AI-8 editor snapshot abstraction.
- `control/AuthoringContext.hpp` — AI-8 dynamic context assembler.
- `control/CompletionContextProvider.hpp` — AI-G3 location-specific context.
- `control/ProjectReadFacade.hpp` — AI-2 read-only project intelligence.
- `control/ChuckSessionService.hpp` — AI-5 ChucK compiler/diagnostics.
- `control/RenderService.hpp` — AI-6 rendering/asset lifecycle.
- `control/SongMutationService.hpp` — AI-7 safe song mutation.
- `ui/HathorLspClient.hpp` — AI-4 LSP client (deterministic editor integration).
- `ui/LspCompletionLogic.hpp` — AI-4 deterministic completion logic.
- `ui/GhostLlmClient.hpp` — AI-G1 llm-ls client (ghost writing).
- `ui/GhostCompletionLogic.hpp` — AI-G1/AI-G2 ghost lifecycle + FIM.
- `ui/GhostTextOverlay.hpp` — AI-G6 ghost text as UI state.
- `ui/CompletionCoordinator.hpp` — AI-G5 deterministic/ghost coexistence.
- `ui/ChuckKeywords.hpp` — AI-G7 ChucK deterministic completion fallback.
