# Hathor Frontend Performance Budgets + Release Bar

Measured 2026-09-17 on macOS (Debug build, `HATHOR_BUILD_APP=ON`).
Re-measure on Release before shipping; budgets are targets, numbers below
are the current Debug reality.

## Test suite (release gate)

| Suite | Result |
|-------|--------|
| Full `ctest` | **1288/1288 pass** |
| UI subset (`-R ui`) | 53/53 pass |
| Build warnings | zero (`-Wall -Wextra -Werror` clean) |

## Bundle sizes (Debug)

| Artifact | Size |
|----------|------|
| `HathorUI.app` | 303 MB (Debug; Release size not yet recorded — see checklist) |
| `HathorUI` binary | 180 MB |
| `hathor-audio-worker` | 19 MB |
| `hathor-mcp` | 5 MB |
| `llm-ls` sidecar | 97 MB |

## Budgets (enforced by construction this pass)

| Budget | Target | Mechanism |
|--------|--------|-----------|
| Message-thread stall | no unbounded loops on 60 Hz tick | ring drains capped (64 frames / 2048 events per tick) |
| Idle repaint rate | ~15 fps when transport stopped | `UITimer` idle throttle (every 4th tick) |
| Git status on message thread | never walks repo at 60 Hz | worker refresh at ~0.5 Hz into a locked cache |
| Explorer poll cost | skip VCS/dependency/build trees | ignore list in `FsPollTimer` |
| Search freeze | cancellable, off-thread | worker + generation + cancel flag |
| Chat history memory | ≤ 200 msgs/thread, ≤ 64 KB/msg | caps in `ChatSessionState` + container export |
| Terminal output memory | ≤ 512 KB tail | `kMaxOutputChars` trim |
| Snapshot memory | ≤ 1 MB per closed tab | `kMaxSnapshotBytes`, spill to disk |
| Icon cache growth | no per-theme leak | `clearCache()` on `setPalette()` |
| LSP message memory | ≤ 64 MB/message, ≤ 128 MB buffered | framer caps, malformed headers skipped not wiped |

## Not yet measured (before calling this shippable)

- keystroke-to-paint latency on a 10k-line file
- wall-clock cost of repo-wide search on a large workspace
- idle CPU % with transport stopped vs running
- 60 s soak: threads/fds/memory flat
- ASan/TSan clean on open/close-tab + workspace-switch stress loops
- Release-build (`-O2`, stripped) bundle size + cold start time

## Release checklist

- [x] Debug + `HATHOR_BUILD_APP=ON` builds clean, no new warnings
- [x] `hathor-ui-tests` + full `ctest` green (1288/1288)
- [x] Version single-sourced (`HATHOR_UI_VERSION`, About dialog)
- [x] `docs/SHORTCUTS.md` matches the registry
- [ ] Release-config build + bundle size recorded
- [ ] Unmeasured budgets above measured and met
- [ ] Sanitizer stress loops clean
