# Hathor Frontend Performance Budgets + Release Bar

Measured 2026-09-17/18 on macOS. Debug build for the suite, Release for
runtime numbers (`HATHOR_BUILD_APP=ON` throughout).

## Test suite (release gate)

| Suite | Result |
|-------|--------|
| Full `ctest` | **1290/1290 pass** |
| UI subset (`-R ui`) | 53/53 pass |
| Build warnings | zero (`-Wall -Wextra -Werror` clean) |

## Bundle sizes (Release, except where noted)

| Artifact | Size |
|----------|------|
| `HathorUI.app` | 166 MB Release (303 MB Debug) |
| `HathorUI` binary | 59 MB Release (180 MB Debug) |
| `hathor-audio-worker` | 7.1 MB Release (19 MB Debug) |
| `hathor-mcp` | 700 KB Release (5 MB Debug) |
| `llm-ls` sidecar | 97 MB (prebuilt, same both configs) |

## Startup (Release, headless launch, samples dir)

| Metric | Measured |
|--------|----------|
| Cold start to audio+worker ready | ~3.3 s first launch |
| Warm start to audio+worker ready | ~0.5 s |

## Steady-state idle (Release, headless, 60 s soak)

| Metric | Measured |
|--------|----------|
| CPU, transport idle | ~7.5% (was ~95%: the watcher compared an ignored walk against an un-ignored snapshot, so it rebuilt the whole tree every 2 s) |
| RSS over 60 s | flat at ~88 MB (+60 KB drift, noise) |
| Survival | 60 s+ clean shutdown on request (previously SIGABRT ~15–20 s from the rebuild churn) |
| Threads / fds | 22 threads, ~35 fds, stable across samples |

## Micro-benchmarks (Debug, enforced in `tests-ui/test_perf_budgets.cpp`)

| Benchmark | Measured | Budget |
|-----------|----------|--------|
| tokenise 10k-line doc (65k tokens) | ~17 ms | < 500 ms |
| workspace search, 200 files / 200 hits | ~39 ms | < 2000 ms |

## Budgets (enforced by construction this pass)

| Budget | Target | Mechanism |
|--------|--------|-----------|
| Message-thread stall | no unbounded loops on 60 Hz tick | ring drains capped (64 frames / 2048 events per tick) |
| Idle repaint rate | ~15 fps when transport stopped | `UITimer` idle throttle (every 4th tick) |
| Git status on message thread | never walks repo at 60 Hz | worker refresh at ~0.5 Hz into a locked cache |
| Explorer poll cost | skip VCS/dependency/build trees; polled and snapshot walks identical | shared `collectInto` walk + 50k-entry cap, no rebuild when unchanged |
| Search freeze | cancellable, off-thread | worker + generation + cancel flag |
| Chat history memory | ≤ 200 msgs/thread, ≤ 64 KB/msg | caps in `ChatSessionState` + container export |
| Terminal output memory | ≤ 512 KB tail | `kMaxOutputChars` trim |
| Snapshot memory | ≤ 1 MB per closed tab | `kMaxSnapshotBytes`, spill to disk |
| Icon cache growth | no per-theme leak | `clearCache()` on `setPalette()` |
| LSP message memory | ≤ 64 MB/message, ≤ 128 MB buffered | framer caps, malformed headers skipped not wiped |

## Not yet measured (before calling this shippable)

- keystroke-to-paint latency on a 10k-line file (tokenise path is budgeted
  above; full paint path needs a windowed run)
- wall-clock cost of repo-wide search on a large real workspace
- ASan/TSan clean on open/close-tab + workspace-switch stress loops
  (ASan app build exists at `build-asan/`; headless runs 100 s+ clean,
  but the stress loops themselves were not run under it)

## Release checklist

- [x] Debug + `HATHOR_BUILD_APP=ON` builds clean, no new warnings
- [x] `hathor-ui-tests` + full `ctest` green (1290/1290, incl. 2 perf budgets)
- [x] Release-config build + bundle size recorded (166 MB app)
- [x] Cold/warm start measured (3.3 s / 0.5 s to audio+worker ready)
- [x] 60 s idle soak: CPU/RSS flat, clean shutdown
- [x] Version single-sourced (`HATHOR_UI_VERSION`, About dialog)
- [x] `docs/SHORTCUTS.md` matches the registry
- [ ] Unmeasured budgets above measured and met
- [ ] Sanitizer stress loops clean
