# llm-ls Integration Verification (AI-G1)

> **Verified against llm-ls source** (github.com/huggingface/llm-ls, `main` branch)
> Date: 2026-08-10
> Method: Direct source code inspection of `crates/custom-types/src/llm_ls.rs`,
> `crates/custom-types/src/request.rs`, and `crates/llm-ls/src/main.rs`.

## Summary

llm-ls is a Rust LSP server (Apache-2.0, ~880 GitHub stars) that provides
LLM-powered code completion via FIM (Fill-in-the-Middle) prompting. It supports
HuggingFace Inference API, TGI, Ollama, llama.cpp, and OpenAI-compatible backends.

## Verified Protocol Surface

### Launch Method

llm-ls is launched as a stdio-based LSP server (default behavior). The `--port`
flag enables TCP socket mode. Our integration uses stdio transport.

**Source**: `crates/llm-ls/src/main.rs` — `Args` struct:
```rust
#[arg(long = "port")]
socket: Option<usize>,
#[arg(short, long, default_value_t = true)]
stdio: bool,
```
When no `--port` is given, `tokio::io::stdin()/stdout()` is used.

### Transport

Standard LSP Content-Length framing (CRLF), same as `HathorLspClient`.
Reuses existing `LspMessageFramer`.

### Custom Methods

| Method | Type | Direction |
|---|---|---|
| `llm-ls/getCompletions` | Request (`id`) | Client → Server |
| `llm-ls/acceptCompletion` | Notification (no `id`) | Client → Server |
| `llm-ls/rejectCompletion` | Notification (no `id`) | Client → Server |

**NOTE**: `$/cancelRequest` is NOT handled for custom methods. llm-ls uses
`tower-lsp` which has native cancellation for standard LSP requests, but the
custom `llm-ls/getCompletions` method is not a cancellable request. Our
`GhostCompletionLogic` implements client-side cancellation via `cancelPendingRequest()`
and revision-based staleness detection.

### Request: `llm-ls/getCompletions`

Params (`GetCompletionsParams` struct, `#[serde(rename_all = "camelCase")]`):

```rust
pub struct GetCompletionsParams {
    #[serde(flatten)]
    pub text_document_position: TextDocumentPositionParams,
    #[serde(default, deserialize_with = "parse_ide")]
    pub ide: Ide,
    pub fim: FimParams,
    pub api_token: Option<String>,
    pub model: String,
    #[serde(flatten)]
    pub backend: Backend,
    pub tokens_to_clear: Vec<String>,
    pub tokenizer_config: Option<TokenizerConfig>,
    pub context_window: usize,
    pub tls_skip_verify_insecure: bool,
    #[serde(default)]
    pub request_body: Map<String, Value>,
    #[serde(default)]
    pub disable_url_path_completion: bool,
}
```

Key field details:

- **`textDocument`**: `TextDocumentIdentifier` (flattened) — has `uri` and
  optional `version`. Extra fields like `languageId` or `text` are ignored by
  serde (no `deny_unknown_fields`).

- **`position`**: `{line: u32, character: u32}` — 0-based.

- **`ide`**: `Option<Ide>` enum, serialized as lowercase. Valid values:
  `neovim`, `vscode`, `jetbrains`, `emacs`, `jupyter`, `sublime`,
  `visualstudio`, `unknown`. Sending an unrecognized value causes deserialization
  failure. **Hathor sends `"unknown"`.**

- **`fim`**: `FimParams { enabled: bool, prefix: String, middle: String, suffix: String }`.
  **The document text before/after cursor is NOT provided here** — llm-ls
  extracts it from the synced document (via `didOpen`/`didChange`). The `fim.prefix`
  is additional context prepended to the prompt; `fim.suffix` is inserted
  between prefix and suffix context; `fim.middle` is the FIM middle token.

- **`api_token`**: `Option<String>` — send `null` if not needed.

- **`backend`**: Internally tagged enum with tag `"backend"`, serialized as
  **lowercase**:
  ```rust
  #[serde(rename_all = "lowercase", tag = "backend")]
  pub enum Backend {
      HuggingFace { url: String },  // "huggingface"
      LlamaCpp { url: String },     // "llamacpp"
      Ollama { url: String },       // "ollama"
      OpenAi { url: String },       // "openai"
      Tgi { url: String },         // "tgi"
  }
  ```
  Serialized form: `{"backend": "huggingface", "url": "https://api-inference.huggingface.co"}`

- **`tokenizer_config`**: `Option<TokenizerConfig>` where:
  ```rust
  #[serde(untagged)]
  pub enum TokenizerConfig {
      Local { path: PathBuf },
      HuggingFace { repository: String, api_token: Option<String> },
      Download { url: String, to: PathBuf },
  }
  ```
  This is an **object enum**, not a string. Sending a plain string (e.g.
  `"default"`) causes deserialization failure. **Hathor sends `null`** so llm-ls
  auto-resolves the tokenizer from the HuggingFace model repository.

- **`request_body`**: `Map<String, Value>` with `#[serde(default)]` — send
  empty object `{}` if not used.

- **`disable_url_path_completion`**: `bool` with `#[serde(default)]` — defaults
  to `false`.

### Response: `llm-ls/getCompletions`

```rust
pub struct GetCompletionsResult {
    pub request_id: Uuid,
    pub completions: Vec<Completion>,
}

pub struct Completion {
    pub generated_text: String,
}
```

The response is wrapped in a standard JSON-RPC 2.0 response:
```json
{
    "jsonrpc": "2.0",
    "id": "<client-generated-uuid>",
    "result": {
        "request_id": "<server-generated-uuid>",
        "completions": [{"generated_text": "..."}]
    }
}
```

**Note**: The `request_id` in the result is server-generated (new UUID per
request), NOT the client's request ID. The client should correlate responses
via the JSON-RPC `id` field, which echoes the client's UUID.

### Notifications: `llm-ls/acceptCompletion` / `llm-ls/rejectCompletion`

```rust
pub struct AcceptCompletionParams {
    pub request_id: Uuid,
    pub accepted_completion: u32,
    pub shown_completions: Vec<u32>,
}

pub struct RejectCompletionParams {
    pub request_id: Uuid,
    pub shown_completions: Vec<u32>,
}
```

These are sent as JSON-RPC notifications (no `id` field).

## Document Synchronization

llm-ls requires documents to be synced via standard LSP notifications:
- `textDocument/didOpen` — must be sent before `llm-ls/getCompletions`
- `textDocument/didChange` — sent on each text change
- `textDocument/didClose` — cleanup

**Critical**: If the document URI is not in llm-ls's `document_map`, the
`get_completions` handler returns empty completions. HathorTab now mirrors
document sync calls to the GhostLlmClient alongside the existing LSP client
calls.

llm-ls supports `Incremental` text document sync (only full document text
is needed per the LSP spec — `contentChanges` is handled).

## Language Identification

llm-ls's `Document::open` uses the `languageId` for AST parsing (tree-sitter)
to determine completion type (single-line, multi-line, or empty). If the
language is not supported by tree-sitter, llm-ls falls through to a
heuristic-based check (next character is whitespace → single-line completion;
otherwise → no completion).

- `.hathor` files → `languageId: "hathor"` (may not have tree-sitter grammar;
  heuristic-based completion)
- `.ck` files → `languageId: "chuck"` (same — heuristic-based)

## Limitations

1. **No cancellation protocol**: llm-ls does not support `$/cancelRequest`
   for `llm-ls/getCompletions`. Staleness is handled client-side via:
   - Revision tracking (incremented on each editor change)
   - Client-side timeout (5s default)
   - `cancelPendingRequest()` for client-side cancellation

2. **Tokenizer config**: llm-ls expects `Option<TokenizerConfig>` (an enum of
   objects). Sending a string value will cause deserialization failure.
   Hathor sends `null` to let llm-ls auto-resolve.

3. **Backend enum is lowercase**: The `backend` field uses lowercase variant
   names (e.g., `"huggingface"` not `"HuggingFace"`).

4. **IDE field**: Must be a valid `Ide` variant. `"unknown"` is the default.
   Sending `"hathor"` causes deserialization failure.

5. **Accept/reject are non-critical**: These are telemetry-only notifications.
   The server only logs them and returns `Ok(())`. Format mismatch won't break
   the core completion path.

6. **AST-based completion filtering**: llm-ls's `should_complete` function
   uses tree-sitter to decide whether to return completions. For unsupported
   languages, it may return `Empty` (no completions), which means ghost text
   won't appear if the heuristic determines no completion is appropriate.

7. **No streaming support**: llm-ls sends completions as a single response
   after the LLM finishes generating. There is no incremental/streaming
   completion path exposed in the LSP protocol.

## Hathor Integration Architecture

```
HathorTab (JUCE message thread)
    │
    │ onEditorChange / ghostTick
    ▼
GhostCompletionLogic (JUCE-free, testable)
    │ 1. Debounces (300ms default)
    │ 2. Tracks revision for staleness
    │ 3. Calls buildFimContext (returns empty — llm-ls handles document context)
    │ 4. Returns GhostCompletionRequest + UUID requestId
    ▼
GhostLlmClient (JUCE Timer-based polling, POSIX fork/exec)
    │ 1. Resolves provider config (env vars → GhostProviderConfig)
    │ 2. requestGhostCompletion → req.toJson() → framed JSON-RPC
    │ 3. Polls stdout for responses every 50ms
    │ 4. checkTimeout → fires 5s timeout
    │ 5. Delivers raw GhostCompletionResponse + requestId via callback
    ▼
GhostCompletionLogic.onGhostResponse(requestId, response, nowMs)
    │ 1. Validates requestId matches pending request
    │ 2. Validates response.request_id matches (server-generated UUID)
    │ 3. Checks revision match (stale rejection)
    │ 4. Checks timeout
    │ 5. Extracts best completion → GhostResult
    ▼
GhostTextOverlay (JUCE Component)
    │ Renders ghost text at cursor position
```

## Environment Variables (Provider Configuration)

| Variable | Description | Default |
|---|---|---|
| `GHOST_ENABLED` | Enable/disable ghost text | `false` |
| `GHOST_BACKEND` | Backend type: `huggingface`, `llamacpp`, `ollama`, `openai`, `tgi` | `huggingface` |
| `GHOST_MODEL` | Model identifier (e.g., `bigcode/starcoder`) | (required) |
| `HF_API_TOKEN` | HuggingFace API token | (required for HF) |
| `OPENAI_API_KEY` | OpenAI API key | (required for OpenAI) |
| `GHOST_URL` | Custom URL for non-default backends | backend-specific |
| `GHOST_CONTEXT_WINDOW` | Max context window tokens | `2048` |
| `GHOST_TLS_SKIP_VERIFY` | Skip TLS verification (dev only) | `false` |
| `GHOST_TOKENIZER_CONFIG` | Local tokenizer path | (auto-resolved) |

Credentials are read per-request from environment variables. They are never
persisted in editor state, project files, or MCP context.
