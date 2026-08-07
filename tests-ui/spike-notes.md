# ACP Transport Spike — Task 0.1 — Findings

**Date:** 2024  
**Status:** PASSED (mechanical tests) / DEFERRED (live-agent tests)  
**Spike binary:** `build/tests-ui/acp_spike` — exits 0 on 56/56 checks

---

## 1. Transport API — `juce::ChildProcess` vs `posix_spawn` + pipes

### Decision: Use `posix_spawn` + `pipe` + dedicated read/write fds

`juce::ChildProcess` was evaluated but **not chosen** for the following reasons:

- `juce::ChildProcess` exposes `readProcessOutput()` which is a blocking,
  single-call read that accumulates all available output up to a buffer size.
  There is no documented support for concurrent writes to stdin while streaming
  stdout asynchronously.
- The JUCE source for `ChildProcess` on macOS uses `posix_spawn` internally
  but only exposes a single `writePipe` fd through its public API. Concurrent
  bidirectional use would require reaching into private members or patching JUCE.
- `juce::ChildProcess` would introduce a JUCE dependency into `hathor-mcp`,
  violating Req 31 (no JUCE links in the `hathor-mcp` target).

### Chosen API: `posix_spawn` + `pipe(2)` + `poll(2)` on macOS

The spike (`test_bidirectional_stdio`) confirmed:
- `posix_spawn` successfully spawns `/bin/cat` with redirected stdin/stdout.
- `posix_spawn_file_actions_adddup2` correctly wires the pipe fds.
- The parent writes 353 bytes of JSON-RPC 2.0 to the child's stdin and reads
  back exactly 353 bytes from the child's stdout — no deadlock, no corruption.
- `O_NONBLOCK` on the parent's read end + `poll(2)` with a 5-second deadline
  provides a clean timeout mechanism; no blocking indefinitely.
- Child exit is detected via `waitpid(WNOHANG)` in the poll loop.

### Recommendation for `AcpAgentSession` (Task 4.2)

```
parentWriteFd  →  child stdin   (agent receives JSON-RPC requests)
childStdoutFd  →  parent reads  (agent emits session/update notifications)
```

The sender thread owns `parentWriteFd` exclusively — only it writes.  
The reader thread owns `childStdoutFd` exclusively — only it reads.  
No mutex is needed on the fds themselves because each is owned by one thread.

Use `poll(parentWriteFd, POLLOUT, timeout_ms)` before each write to detect
a blocked or dead agent process without hanging the sender thread.

**`juce::ChildProcess` status:** Not recommended for bidirectional ACP stdio.
Use raw POSIX APIs as confirmed by this spike.

---

## 2. `mcpServers` Field — Claude Code and Gemini CLI Interoperability

### Status: DEFERRED — requires manual testing with live agent binaries

The mechanical JSON-RPC message structure was fully validated in the spike:

| Message | Structure verified |
|---|---|
| `initialize` | ✅ Round-trips through nlohmann/json without loss |
| `session/new` with `mcpServers` | ✅ Schema correct; `env` array with `HATHOR_SOCKET_PATH` in place |
| `session/prompt` | ✅ `sessionId` + `prompt` array with `type`/`text` entries |
| `session/update` (`agent_message_chunk`) | ✅ Parses correctly |
| `session/update` (`tool_call`) | ✅ Parses correctly |
| `session/update` (`tool_call_update`) | ✅ Parses correctly |
| MCP `tools/call` → `ControlInterface` command | ✅ Translation logic verified |

### What still requires manual verification before Task 4.1

The following assertions **cannot** be tested mechanically without live agent
binaries and are explicitly deferred:

1. **Claude Code** (`claude` CLI): Does it read `mcpServers` from `session/new`
   and spawn `hathor-mcp` as a subprocess with the specified `command`, `args`,
   and `env`?

2. **Gemini CLI** (`gemini` CLI): Same question as above. Gemini CLI's ACP v1
   support and its handling of `mcpServers` has not been confirmed against the
   spec's requirements at the time of this spike.

3. **`session/update` tool notifications**: Do both agents emit `tool_call` and
   `tool_call_update` notifications as JSON-RPC notification objects when
   `hathor-mcp` receives and processes an MCP `tools/call`?

4. **End-to-end MCP tool invocation**: Does sending `session/prompt` with
   "call set_pattern" result in the agent issuing an MCP `tools/call` to
   `hathor-mcp`, which then writes the command over the Unix socket?

### Manual test procedure (to run before Task 4.1)

```bash
# Build hathor-mcp stub (Task 1.5 deliverable)
cmake --build build --target hathor-mcp

# In terminal 1 — start a Unix socket listener (netcat or socat)
export TMPDIR=/tmp/
SOCK=/tmp/hathor-$(pgrep -f test).sock
socat UNIX-LISTEN:$SOCK,fork STDIO

# In terminal 2 — run the spike with a live agent
# Set HATHOR_AGENT to the agent executable
export HATHOR_AGENT=$(which claude)
# Run a manual ACP session using the spike's message templates
./build/tests-ui/acp_spike --live-agent  # (not yet implemented; see spike code)
```

The live-agent path is explicitly out of scope for this spike executable.
It will be implemented as a separate integration test in Task 4.2 validation.

---

## 3. Unix Socket Round-Trip

### Status: PASSED ✅

The spike validates the full socket lifecycle that `AcpAgentSession` and
`hathor-mcp` will use in production:

| Check | Result |
|---|---|
| `socket(AF_UNIX, SOCK_STREAM, 0)` + `bind` + `listen` | PASS |
| Client `connect` to named path | PASS |
| Server `accept` | PASS |
| Client sends `{"cmd":"set-pattern","notation":"bd sn","slot":"d1"}\n` | PASS |
| Server reads and echoes `{"ok":true}\n` | PASS |
| Client parses `{"ok":true}` correctly | PASS |
| Socket file removed after `unlink` | PASS |

Socket path format: `$TMPDIR/hathor-<pid>-spike.sock` — matches the production
format `$TMPDIR/hathor-<pid>.sock` specified in Req 32 / design.md.

The spike uses a **synchronous server** (main thread accepts one connection)
and a **concurrent client** (separate `std::thread`). In production,
`AcpAgentSession::start()` will use a non-blocking accept loop on a separate
thread so it doesn't block the JUCE message thread.

**Important finding on `TMPDIR` on macOS:** `$TMPDIR` on macOS expands to a
per-user path like `/var/folders/xx/.../T/`. The path is longer than on Linux
(`/tmp/`). Since `sun_path` in `sockaddr_un` is only 104 bytes on macOS (vs
108 on Linux), the socket path length must be validated at runtime. The spike
includes this check. For production code, use:

```cpp
// In AcpAgentSession::start():
const char* tmpdir = std::getenv("TMPDIR");
if (!tmpdir || tmpdir[0] == '\0') tmpdir = "/tmp";
std::string socketPath = std::string(tmpdir)
    + "hathor-" + std::to_string(::getpid()) + ".sock";
// Assert: socketPath.size() < 104 (macOS sun_path limit)
```

---

## 4. Recommendation — Proceed to Group 4?

### Summary

| Validation area | Status | Blocking? |
|---|---|---|
| (a) posix_spawn + pipe bidirectional stdio | ✅ CONFIRMED working | No |
| (b) ACP JSON-RPC message structure | ✅ CONFIRMED correct | No |
| (b) mcpServers live-agent interop | ⏳ DEFERRED — manual test needed | **Conditional** |
| (c) Unix socket round-trip | ✅ CONFIRMED working | No |

### Recommendation: **PROCEED to Group 4** with one tracking item

The transport layer (posix_spawn + pipes), socket IPC, and JSON-RPC message
serialisation are all confirmed working. The only remaining uncertainty is
whether the specific `mcpServers` schema in `session/new` is honoured by
Claude Code and Gemini CLI at runtime.

**Mitigations:**
1. `AcpAgentSession` (Task 4.2) should be designed so the `mcpServers` schema
   is trivially replaceable — a single JSON construction site that can be
   patched if the live-agent test reveals a field name discrepancy.
2. If an agent doesn't support `mcpServers` natively, the fallback is a
   `_hathor/tool_call` extension prompt (mentioned in the task spec), which
   can be added to `session/prompt` without changing the transport layer.
3. The socket and stdio transport are fully decoupled from `mcpServers` — even
   if agent interop needs adjustment, no transport rework is required.

**Action item before Task 4.1 merge:** Run the manual verification procedure
from Section 2 with both `claude` and `gemini` CLI and record results. If
either agent fails to honour `mcpServers`, open a separate issue to implement
the fallback before Task 4.5 (error/reconnect handling).

The transport spike fulfils all mechanical success criteria defined in
Task 0.1. No transport-layer blockers were found.
