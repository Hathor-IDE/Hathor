# AI-G8: Preserve the Existing Strudel LSP Decision — `.hathor` = Standard Strudel Mini-Notation

## Status

**Decided / Recorded.** This is a declaration document — a guardrail that
freezes the architectural decision already expressed in PROGRAM.md (decisions
#15, #16, #17, #18, #19, #20) and §AI-3. No new grammar, parser, or LSP is
introduced. No implementation is required to satisfy this record.

---

## Context

Hathor's `.hathor` file format is the project representation of **standard
Strudel mini-notation**. The Hathor engine re-implements Strudel's
mini-notation in C++ and differentially validates that reimplementation against
Strudel golden fixtures (`reference/strudel-golden/`).

A recurring risk during Phase H–K implementation is that the `.hathor`
**extension** — as opposed to the **notation** it carries — is interpreted as
an invitation to invent a "Hathor-specific" grammar, parser, or language server.
This document explicitly forbids that interpretation and records the canonical
layering that all Phase H–K work must respect.

---

## Decision

1. **`.hathor` files use standard Strudel mini-notation.** The `.hathor`
   extension is a project/file-system convention (analogous to `.ck` for ChucK
   source). It does **not** imply a custom dialect, superset, or grammar.
   Hathor's supported surface is a versioned subset of upstream Strudel/Tidal
   mini-notation, not a new language.

2. **The Strudel LSP (`strudel-lsp-server`) is reused as-is.** Hathor consumes
   the existing `strudel-lsp-server` (located at
   `reference/strudel-lsp/strudel-lsp-server.cjs`) for deterministic
   authoring intelligence on `.hathor` files. It is **not** forked,
   re-implemented, or re-parsed inside Hathor. Hathor only layers
   supported-surface / project-specific metadata where the LSP does not
   inherently provide it.

3. **The Hathor C++ parser is the runtime source of truth.** The Strudel LSP
   is an *authoring/intelligence service*, not the runtime parser. It does **not**
   replace or shadow Hathor's C++ engine implementation. Runtime authority
   resides solely in the C++ parser (`engine/src/`).

4. **LSP and AI ghost-writing are separate, complementary systems.** The Strudel
   LSP provides deterministic language intelligence; `llm-ls`/FIM provides
   probabilistic ghost-writing. Integrating the LSP must never be interpreted as
   a substitute for the AI ghost-writing layer (decision #19).

5. **Hathor metadata supplies only the supported-surface.** The versioned
   `LanguageMetadata` (`reference/language-metadata/HathorLanguageMetadata.json`)
   identifies what Hathor intentionally implements, validates, and supports.
   It is not a grammar — it is a compatibility manifest consumed by the editor,
   MCP, AI, and validation layers (decision #18).

---

## Canonical Architecture / Dependency Graph

```text
  `.hathor`
       │
       ▼
  Standard Strudel mini-notation
       │
       ├──► Strudel LSP  (strudel-lsp-server)
       │       └── deterministic editor intelligence
       │           (completion · hover · diagnostics · discovery)
       │
       ├──► Hathor supported-surface metadata
       │       └── supported constructs / project knowledge / versioning
       │           (LanguageMetadata { schemaVersion,
       │            hathorEngineCompat, strudelMiniNotationCompat,
       │            chuckLibVersion, definitions[] })
       │
       └──► llm-ls + FIM
               └── AI ghost writing
                   (inline ghost text · Fill-in-the-Middle ·
                    multi-line continuation · musical transformation ·
                    AI repair after diagnostics)

  Runtime authority:

  `.hathor` source
       │
       ▼
  Hathor C++ parser / engine   (reference/strudel-golden/ validation)
       └── runtime source of truth
```

### Layer responsibilities

| Layer | Component | Responsibility | Source of truth |
|---|---|---|---|
| **Runtime** | Hathor C++ parser (`engine/src/`) | Interpret `.hathor` source at runtime | Hathor C++ engine |
| **Deterministic intelligence** | Strudel LSP (`strudel-lsp-server.cjs`) | Completion, hover, diagnostics, symbol/construct discovery | Upstream Strudel/TypeScript |
| **Supported-surface knowledge** | `LanguageMetadata` (versioned JSON) | What Hathor supports; version compatibility | `reference/language-metadata/*.json` |
| **AI ghost-writing** | `llm-ls` + FIM (`GhostLlmClient`, `GhostCompletionLogic`) | Probabilistic inline completion, context-aware generation, AI repair | External LLM provider |
| **Integration glue** | `HathorLspClient`, `LspContextBridge`, `CompletionContextProvider` | Bridge editor ↔ LSP; route metadata to LSP & AI | Hathor `ui/` + `control/` |

### Validation mechanism

Hathor's C++ engine implementation is validated against Strudel golden fixtures
in `reference/strudel-golden/`. These JSON fixtures capture expected event
schedules for mini-notation constructs. The C++ parser must produce output that
matches these fixtures deterministically. Any drift is a runtime bug, not a
language-intelligence decision.

Key fixture examples:
- `euclid-3-8.json` — Euclidean rhythm generation
- `nested-subsequence-bracket.json` — Bracket grouping semantics
- `slowcat-bd-sn-hh.json` — Polymeter / angle-bracket semantics
- `degrade-by-0.0.json` — RNG degradation boundary conditions

The `docs/potential-improvements-over-strudel.md` file records future
considerations that might deliberately deviate from Strudel — these are
explicitly **NOT IMPLEMENTED** and must not be treated as language features.

---

## Guardrails (Explicitly Forbidden)

The following actions are **forbidden** under this decision. If any of them
would be needed, the architectural guardrail must be revised via a formal
decision process — not silently acted upon:

1. **No custom `.hathor` grammar.** Do not invent a Hathor-specific grammar,
   parser, or syntax extension for `.hathor` files. The `.hathor` extension is
   a file convention, not a language boundary.

2. **No custom Strudel-compatible grammar.** Do not create a new grammar
   (tree-sitter, Lezer, Antlr, etc.) that re-implements or partially tracks
   Strudel mini-notation for editor completion. Reuse the Strudel LSP.

3. **No custom Hathor LSP.** Do not build a dedicated Hathor language server
   for `.hathor` files. The Strudel LSP is the authoritative language-
   intelligence implementation.

4. **No LSP-as-runtime-parser.** The Strudel LSP is not a substitute for the
   Hathor C++ parser. It must not be invoked for runtime interpretation,
   validation, or execution.

5. **No forking of the Strudel LSP.** Do not fork, vendored-copy, or heavily
   modify `strudel-lsp-server`. Minimal integration glue (process lifecycle,
   JSON-RPC framing, callback wiring) is permitted; re-implementation of its
   language intelligence is not.

6. **No collapsing LSP into AI ghost-writing.** Integrating the Strudel LSP
   must never be interpreted as "LSP support is done" to the exclusion of the
   `llm-ls`/FIM ghost-writing layer. Decisions #19 and #20 forbid this.

7. **No LSP-as-diagnostics-authority for ChucK.** ChucK (`.ck`) diagnostics
   come from the real libchuck compiler, not from the Strudel LSP. The LSP
   does not cover ChucK source.

---

## Runtime Authority

```text
  `.hathor` source  ──►  Hathor C++ parser/engine  ──►  runtime interpretation
                              │
                              └─── validated against ──►  Strudel golden fixtures
```

The Strudel LSP and Hathor metadata are **authoring-time** services. They
improve the editing experience (completion, hover, diagnostics, AI context)
but carry no runtime authority. The C++ engine is the sole runtime source of
truth. If the LSP suggests a construct or the metadata marks something as
"supported," the engine must still independently parse and interpret it —
the LSP does not bypass the engine.

---

## Relationship to Other AI-G Tasks

| Task | Relationship to AI-G8 |
|---|---|
| **AI-3** | Defines the versioned metadata model that AI-G8 says the LSP layer on top of. |
| **AI-4** | Consumes the Strudel LSP for editor completion/hover/diagnostics — the direct integration site. |
| **AI-G1** | Defines the `llm-ls`/FIM ghost-writing layer — the complementary AI system. |
| **AI-G5** | Defines coexistence rules between deterministic LSP completion and ghost completion. |
| **AI-G7** | For `.ck` files: no reusable ChucK LSP exists, so deterministic metadata + real libchuck compiler are used. By contrast, AI-G8 confirms the Strudel LSP **is** reusable for `.hathor`. |
| **AI-G9** | Final dependency graph + diagram — incorporates AI-G8's layering as a foundational constraint. |

---

## References

- PROGRAM.md — Decision #15 (deterministic intelligence precedes LLM; no custom LSP)
- PROGRAM.md — Decision #16 (reuse existing language-intelligence implementations; `.hathor` = standard Strudel)
- PROGRAM.md — Decision #17 (LSP ≠ MCP; separate concerns)
- PROGRAM.md — Decision #18 (versioned language metadata; Strudel = golden standard for Phase 1)
- PROGRAM.md — Decision #19 (LSP and AI ghost-writing are separate, complementary; must not collapse)
- PROGRAM.md — Decision #20 (reuse existing `llm-ls`/FIM server; no custom LLM completion server)
- PROGRAM.md — §AI-3 (Language Intelligence Foundation)
- PROGRAM.md — §AI-G8 (Preserve the Existing Strudel LSP Decision)
- PROGRAM.md — §AI-G9 (Final Authoring Intelligence Stack)
- `reference/strudel-lsp/` — The Strudel LSP server (`strudel-lsp-server.cjs`, `strudel-lsp-server.js`)
- `reference/strudel-golden/` — Golden fixtures for differential validation
- `reference/language-metadata/HathorLanguageMetadata.json` — Versioned supported-surface metadata
- `docs/llm-ls-integration-verification.md` — Verification of the `llm-ls` FIM protocol (companion to this decision)
- `docs/potential-improvements-over-strudel.md` — Future considerations explicitly NOT implemented
- `ui/MiniNotationTokeniser.cpp` — Tokeniser for `.hathor` files (syntax highlighting only)
- `ui/EditorArea.cpp:255-261` — LSP client launch (manages `strudel-lsp-server` process)
- `engine/src/LanguageMetadata.cpp` — Metadata loading + version validation
