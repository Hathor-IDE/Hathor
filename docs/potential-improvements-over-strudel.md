# Potential improvements over Strudel (Phase 1 reference only)

This file is a **running, documentation-only log** of places where Hathor's
Phase 1 behavior currently matches Strudel exactly (by design — Strudel is the
golden standard for Phase 1) but where a *future*, deliberately-considered
variant could plausibly deviate for the better.

Nothing in this file is implemented during Phase 1. Each entry records:

1. **What Strudel does** (ground truth).
2. **Why it might be worth reconsidering someday** (ergonomics rationale).
3. An explicit **NOT IMPLEMENTED — reference only** marker.

Add to this list whenever differential testing against the Strudel golden
fixtures surfaces something worth remembering — *without acting on it during
Phase 1*. The purpose is to preserve the insight so it is not lost, not to
green-light any change.

---

## 1. `degradeBy` correlation-by-default

- **What Strudel does.** `degradeBy(prob, p)` with the default seed (Strudel's
  `randSeed` default `0`) is a deterministic function of
  `(whole.start, seed)`. Two independently-constructed `degradeBy` instances
  with the same default seed therefore produce *identical* (correlated)
  results — see `reference/strudel-golden/degrade-by-0.5-instance-a.json` ==
  `degrade-by-0.5-instance-b.json`. Hathor matches this 1:1.
- **Why it might be worth reconsidering someday.** Correlation-by-default is
  surprising: two separate `degradeBy` nodes in a graph silently produce the
  same pattern, which can mask bugs or unintentionally couple unrelated music.
  An opt-in *decorrelated* variant (a distinct per-instance seed) would be
  safer as a default, with correlation opt-in. Strudel sidesteps this with its
  `?` mini-notation operator, which applies a small per-instance time offset
  (`rand.early(0.0003 * seed)`) — i.e. a decorrelating variant. Hathor could
  add an analogous opt-in decorrelated `degradeBy` later.
- **NOT IMPLEMENTED — reference only.** Hathor matches Strudel's
  correlation-by-default during Phase 1. Do not change.

## 2. `degradeBy(0.0)` drops the `t == 0` event

- **What Strudel does.** `degradeBy(0.0, p)` does **not** act as an identity.
  Strudel's legacy RNG yields `rand(0) == 0` exactly (because `__xorwise(0)
  == 0` and `Math.abs((0 % 2^29) / 2^29) == 0`). The keep rule is strict
  `rand > prob` (Strudel's `filterValues(v => v > x)`), so at `prob == 0.0` the
  event whose `whole.start == 0` fails `rand > 0` and is dropped. This is
  confirmed by `reference/strudel-golden/degrade-by-0.0.json`: over `bd*8` on
  `[0,1)` the fixture lists 7 events at `1/8, 1/4, 3/8, 1/2, 5/8, 3/4, 7/8` —
  the `t == 0` event is absent. (The fixture's prose says "0% removal / all
  survive", but its actual event list is authoritative and omits `t == 0`;
  Hathor matches the event list, not the prose.)
- **Why it might be worth reconsidering someday.** A user reading the prose
  "0% removal" reasonably expects `degradeBy(0.0) == identity`. The `t == 0`
  drop is a measure-zero artifact of the legacy RNG landing exactly on the
  `> 0` boundary. A future "epsilon-safe" degrade variant (e.g. `rand > prob`
  with a tiny epsilon, or a `rand >= prob` rule) could make `0.0` a true
  identity and align behavior with the prose — but that would *diverge* from
  Strudel's ground-truth fixture, so it must be a separate, explicitly-chosen
  variant, not the default.
- **NOT IMPLEMENTED — reference only.** Hathor matches Strudel's ground-truth
  behavior: `degradeBy(0.0)` drops only the `t == 0` event. Do not change.
