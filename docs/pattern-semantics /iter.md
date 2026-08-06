# `iter` (and `iterBack`)

Repeatedly phase-shift a pattern through equal subdivisions of a cycle.

## What it does

`iter(times, pat)` creates `times` staggered copies of `pat`, each shifted by `1/times` of a
cycle, and plays them one per cycle (via `slowcat`). The result loops through `times` distinct
phase-offsets of the source pattern, advancing the phase by one subdivision each cycle.

**Implementation** — `packages/core/pattern.mjs:2711` `_iter`, exported via
`register('iter', ...)` at `:2720`. `iterBack` is the same with `back=true` (`:2739`).

## Exact algorithm

```
_iter(times, pat, back=false):
  times = Fraction(times)
  copies = []
  for i in 0 .. (times - 1):
    phase = Fraction(i) / times
    copies[i] = back ? pat.late(phase) : pat.early(phase)   # phase offset
  return slowcat(...copies)

iter(times, pat)    = _iter(times, pat, back=false)   :: false → early
iterBack(times, pat) = _iter(times, pat, back=true)    :: true  → late
```

Where:
- `early(cycles)` (`pattern.mjs:2228`) shifts the pattern earlier by `cycles`:
  `withQueryTime(t => t + cycles)` then `withHapTime(t => t - cycles)` — i.e. nudge left.
- `late(cycles)` (`pattern.mjs:2249`) is `early(-cycles)` (the JSDoc example shows the
  implementation `pat.early(Fraction(0).sub(offset)).continuity...`).
- `slowcat(...)` (`pattern.mjs:1528`) plays each copy for one cycle with the offset
  correction described in `slowcat.md`.

So `iter(4)` of a one-pattern yields, across 4 consecutive cycles, the pattern at phases
`{0, 1/4, 2/4, 3/4}` (each `early(i/4)`), then repeats. `iterBack` yields the same offsets in
reverse order (`late(i/4)`).

## Edge cases and invariants

- `times` is converted to a `Fraction`; `listRange(0, times.sub(1))` builds `times` offsets.
- `times == 1`: `_iter` produces one copy (`early(0)` = identity) wrapped in a `slowcat` of one
  item → identity (`slowcat` short-circuits singles). So `iter(1)` is roughly identity.
- `times == 0` or negative: `times.sub(1)` is negative/`-1`; `listRange` with `max < min`
  yields an empty `Array.from({length: negative})` → `[]` → `slowcat()` empty → silence-ish
  degenerate result. **Not explicitly guarded** — treat `times ≤ 0` as unspecified/degenerate.
- Non-integer `times` (e.g. `iter(2.5)`): `listRange(0, 1.5)` → indices `0,1` (two copies),
  not fractional; the Fraction `times` only affects the range count, not the subdivision
  resolution. Treat as "floor + 1 copies" behavior — the phase steps are still `1/times`?
  Actually the loop iterates `i` over integers `0..times-1` floored, and phase = `i/times`. So
  fractional `times` produces `ceil(times)` copies with phase spacing `1/times`. Worth noting.
- Registered with `preserveSteps=true` (`:2726`) → steps hint preserved.

## Determinism notes

Fully deterministic (composition of `early/late` + `slowcat`).

## Known deviations / gotchas

- `iter` is built on `slowcat`, so it inherits `slowcat`'s cycle-skip-correction offset math —
  not the raw `%` indexing of `slowcatPrime`. Good; but note `every` uses the *other* one.
- `iterBack` docstring: "Known as iter' in tidalcycles" (`:2730`).
- Because each copy is `early(i/times)`, and `early` shifts hap time forward by `i/times`
  cycles, the phase accumulates correctly across cycles only modulo the wrap of `fastcat`
  inside the pattern; the `slowcat` wrapper ensures exactly one copy per cycle so phases never
  cancel. If the source pattern is multi-cycle, phase offsets interact with its internal cycles
  via `slowcat`'s offset mapping.