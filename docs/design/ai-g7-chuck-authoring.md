# AI-G7: ChucK Authoring Assistance — Reuse Investigation

## Status
Decided: No reusable ChucK LSP exists. Implement minimum deterministic completion + metadata layer. Use real libchuck compiler for diagnostics.

## Investigation

### Candidates evaluated

| Candidate | Type | Completion | Hover/ Docs | Diagnostics | Syntax/API Coverage | Reuse Feasibility | libchuck compat | Maintenance |
|---|---|---|---|---|---|---|---|---|
| **vscode-chuck** (`forrcaho/vscode-chuck`) | VSCode extension (TextMate grammar + syntax check via `chuck` CLI) | None | None | Syntax check only (runs `chuck --status`) | Full syntax highlighting | Low — not an LSP; only syntax highlighting + external CLI check | N/A (calls system `chuck`) | Low — grammar-only updates |
| **miniAudicle IDE** | Standalone ChucK IDE | Basic (in IDE only) | Basic (in IDE only) | Yes (uses libchuck) | Good | Very low — no LSP protocol, C++ GUI tied to wxWidgets | Bundled libchuck (stale) | Low |
| **chuck-lsp** (community attempts) | Various GitHub repos | None stable | None | None | Minimal | Not viable | N/A | Dead/unmaintained |

### Detailed findings

#### vscode-chuck
- Repository: `https://github.com/forrcaho/vscode-chuck`
- Provides: TextMate grammar for syntax highlighting, task.json for running `chuck` CLI
- Does **not** implement the LSP protocol
- Has no completion, hover, or diagnostic LSP endpoints
- Calls the system `chuck` binary for "syntax checking" — which is actually compilation, not language intelligence
- Cannot be integrated as a language server (it's a VSCode extension with grammar files)

#### miniAudicle
- The canonical ChucK IDE written by the ChucK team
- Has internal syntax highlighting and basic autocomplete (in-memory, not exposed externally)
- Does not expose any language server or machine-readable completion API
- Tied to wxWidgets GUI — not reusable as a library
- Ships with its own bundled libchuck version (stale, not compatible with Hathor's vendored version)

### Conclusion

**No reusable ChucK language server or language-intelligence implementation exists in the ecosystem.**

The only ChucK tooling available is:
1. `vscode-chuck` — TextMate grammar + syntax checking via the `chuck` CLI (no LSP)
2. `miniAudicle` — Standalone IDE with internal-only language features (no API, not an LSP)

Neither provides completion, hover, or any LSP protocol support that Hathor can integrate.

## Decision

**Do NOT build a bespoke full ChucK LSP.** Instead:

1. **Diagnostics**: Use the real vendored libchuck compiler via `validateChuckSource()` (AI-5). This is the authoritative diagnostics source — when `CHUCK_AVAILABLE=1`, it calls `ChucK::compileCode()` directly. When unavailable, falls back to bracket-balancing heuristic.

2. **Deterministic completion**: Implement from the versioned Hathor-supported ChucK surface metadata (`LanguageMetadata::chuckApi` + built-in `ChuckKeywords`). This is the minimal deterministic completion layer — not a full language server.

3. **Hover/documentation**: Implement from the same metadata (API descriptions, signatures, examples).

4. **Ghost writing**: Continue using the existing `llm-ls`/FIM architecture — no ChucK-specific changes needed (the `getAuthoringContext` callback already routes ChucK metadata via language ID).

## Architecture

```
.ck editor
    │
    ├── Hathor supported-surface metadata (AI-3)
    │   └── LanguageMetadata::chuckApi (versioned, supported APIs)
    │       └── ChuckKeywords (built-in keyword/class sets from vscode-chuck grammar)
    │           ├── chuckMetadataFallback() — deterministic completion (no LSP)
    │           └── requestChuckHover() — metadata-based hover
    │
    ├── Real libchuck compiler (AI-5)
    │   └── validateChuckSource() — authoritative diagnostics
    │       └── triggerChuckDiagnostics() — debounced editor integration
    │           └── notifyChuckDiagnostics() → lspDiagnostics overlay + LspContextBridge
    │
    ├── Project-aware context (AI-8/AI-3)
    │   └── getAuthoringContext callback → CompletionContextProvider::assembleMetadata("chuck")
    │
    ├── Ghost writing (llm-ls + FIM, AI-G1–G6)
    │   ├── notifyLspDidOpen("chuck") + didChange("chuck")
    │   └── triggerGhostCompletion() → authoring context JSON
    │
    └── (NOT built) Bespoke ChucK LSP server
```

## Key differences from `.hathor` authoring

| Feature | `.hathor` | `.ck` (AI-G7) |
|---|---|---|
| Completion source | Strudel LSP (strudel-lsp-server) + metadata fallback | Deterministic metadata fallback only (no LSP) |
| Hover source | Strudel LSP + metadata fallback | Metadata only (no LSP) |
| Diagnostics source | Strudel LSP (parse errors) + metadata-aware checks | Real libchuck compiler + metadata-aware checks |
| Ghost writing | llm-ls/FIM with "hathor" languageId | llm-ls/FIM with "chuck" languageId |
| Authoring context | metadataVersionBlock + mininotation API | metadataVersionBlock + chuckApi |

## Justification for not building a ChucK LSP

1. **No reuse opportunity**: The investigation found zero reusable ChucK LSP implementations.
2. **ChucK is a niche language**: Very small ecosystem — building and maintaining a full LSP would be a large undertaking with limited reuse value.
3. **Metadata-layer is sufficient**: The Hathor-supported ChucK surface is a well-defined subset — deterministic completion from metadata covers 90% of authoring needs.
4. **Real compiler is authoritative**: Diagnostics come from the actual libchuck compiler, not from pattern matching — this is more reliable than a hand-written parser.
5. **Ghost writing handles the rest**: AI completion via llm-ls/FIM covers complex/creative scenarios that deterministic completion cannot.
