# `chop`

Cut each sample into the given number of equal parts ("granular synthesis").

## What it does

`chop(n, pat)` takes a pattern of sample values and replaces each sample with a *sequence* of
`n` sub-events, each carrying `{ begin, end }` slice coordinates into the sample. This enables
grain-style playback: the sample is sliced into `n` equal chunks and the chunks are played in
order within the sample's duration.

**Implementation** — `packages/core/pattern.mjs:3544`
`register('chop', function (n, pat) { ... })`.

## Exact algorithm

```
chop(n, pat):
  slices       = [0, 1, ..., n-1]
  slice_objects = [ { begin: i/n, end: (i+1)/n } for i in slices ]   # unit-interval slices
  merge(a, b):                          # merge slice into an existing sample value
    if a has begin & end (numbers):
      d = a.end - a.begin
      b = { begin: a.begin + b.begin * d,  end: a.begin + b.end * d }   # scale slice into a
    return { ...a, ...b }               # sample params + { begin, end } slice
  func(o) = sequence( [ merge(o, slice) for slice in slice_objects ] )  # n steps per sample
  return pat.squeezeBind(func).setSteps( n * pat._steps )               # (steps enabled)
```

Key steps:

1. Build `slice_objects` as fractions of the unit interval: chunk `i` spans `[i/n, (i+1)/n]`.
2. `merge(a, b)`: given the sample value `a` (an object with optional numeric `begin`/`end`
   already set) and a unit-interval slice `b`, the slice is rescaled into `a`'s interval
   (`a.begin + b.begin * d` … where `d = a.end - a.begin`). If `a` has no `begin`/`end`, the
   slice is used as-is. The sample's other params are kept via `{...a, ...b}`.
3. `func(o)` maps a single sample value `o` into a `sequence` of `n` merge results — i.e. a
   one-cycle `fastcat` of the `n` slices.
4. `pat.squeezeBind(func)` (`pattern.mjs:390`) = `pat.fmap(func).squeezeJoin()`. `squeezeJoin`
   (`:347`) focuses each outer (sample) event's span, queries the inner pattern within it, and
   munges: `whole = inner.whole ∩ outer.whole`, `part = inner.part ∩ outer.part`, value from
   inner. Net effect: **each sample event is expanded into its `n` slices packed inside the
   sample's own timespan.**
5. `steps` hint set to `n * pat._steps` (when steps enabled).

So a single sample whose part lasts `D` cycles becomes `n` sub-events, each spanning `D/n` and
pointing at `[i/n, (i+1)/n]` of the sample.

## Edge cases and invariants

- `n` is used directly to build `slice_objects`; `n == 0` → `slices = []` → `func(o) =
  sequence()` → `fastcat()` empty → degenerate/empty (not guarded). `n >= 1` is the contract.
- `merge` checks `'begin' in a && 'end' in a && a.begin !== undefined && a.end !== undefined`
  before rescaling; non-numeric `begin/end` (e.g. present but not numbers) would fall into the
  arithmetic path and could produce NaN — the guard only checks presence, not numeric type.
  In practice sample params use numbers.
- `squeezeJoin` drops haps whose wholes don't intersect and whose parts don't intersect
  (`:366-377`); events that fall outside the outer span vanish.
- The slices are *equal width*; for unequal-width slicing use `slice`/`splice` instead
  (`pattern.mjs:3617`).
- Registered `patternify=true` (default `join=innerJoin`), so `n` may be a pattern.

## Determinism notes

Fully deterministic.

## Known deviations / gotchas

- `chop` works on the **value object's `begin`/`end`** (sample-cursor controls used by
  SuperDough/webaudio). If the C++ sound engine does not honor `begin`/`end` slice controls,
  `chop` produces extra events with no audible effect. The combinator's output contract is "a
  pattern of `{ ..., begin, end }` sample values."
- The docstring pairs `chop` with `.rev()` (reverse order of chops) and `.loopAt(2)` — those
  are *separate* combinators applied after `chop`, not part of it.
- Contrast with `striate` (see `striate.md`): `chop` packs all `n` slices *inside* each
  sample's own duration via `squeezeBind`; `striate` instead uses a `slowcat` of slice values
  combined with the pattern and `fast(n)`, so slices advance progressively across the pattern's
  own loops.