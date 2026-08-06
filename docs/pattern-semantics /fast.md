# `fast` (a.k.a. `density`)

Speed a pattern up by a factor. In mini-notation, `*N` compiles to `fast(N)`.

## What it does

`fast(factor, pat)` returns a Pattern where the source pattern plays `factor` times inside the
span where it would otherwise play once. Because patterns are cyclic, this is achieved by
"zooming out" the query time (querying more cycles of the source) and then compressing the
returned hap times by `factor`, so within one cycle of the result you hear `factor` cycles of
the source.

**Implementation** — `packages/core/pattern.mjs:2083` `register(['fast','density'], function
(factor, pat) { ... })`.

## Exact algorithm

```
fast(factor, pat):
  if factor == 0: return silence
  factor = Fraction(factor)                      # exact rational
  fastQuery = pat.withQueryTime(t => t * factor)  # multiply BOTH span begin & end by factor
  return fastQuery.withHapTime(t => t / factor)   # divide both whole & part times by factor
         .setSteps(pat._steps)                     # preserve steps hint
```

Where (all in `pattern.mjs`):
- `withQueryTime(f)` (`:476`) applies `f` to the query timespan via `withTime`, i.e.
  `state.withSpan(span => new TimeSpan(f(span.begin), f(span.end)))`.
- `withHapTime(f)` (`:501`) applies `f` to every returned hap's whole and part via
  `hap.withSpan(span => span.withTime(f))` (`:78`).

So the whole flow for a query `[a, b]`:
1. Query the source over `[a·factor, b·factor]`.
2. Take the returned haps and map each timespan `t ↦ t/factor`.

Because the source is cyclic, querying `factor×` as much time yields `factor×` as many events,
and dividing their times by `factor` packs them into the original window. Net effect: `factor`
occurrences per cycle.

## Edge cases and invariants

- `factor == 0` → **silence** (`pattern.mjs:2086`). Explicitly guarded; not a division-by-zero
  crash.
- `factor` may itself be a Pattern (`fast("<2 3>", pat)`) — `patternify=true` reifies and
  inner-joins the numeric argument, so the factor can vary per cycle.
- `factor` is converted to an exact `Fraction`; no floating-point drift in the timespans.
- `steps` hint is preserved from the source pattern (`preserveSteps=true`, `:2094`), then
  `setSteps(pat._steps)`.
- Because the factor is applied to *absolute* query times, a factor that is a rational like
  `3/2` is handled exactly (query `[a·3/2, b·3/2]`, divide by `3/2`).

## Determinism notes

Fully deterministic.

## Known deviations / gotchas

- `density` is a synonym of `fast` (same registration, `:2083`). In Tidal, `density` is the
  canonical name; Strudel keeps both.
- `fast` is *not* the same as `fastGap` (`:2009`), which compresses into part of the cycle and
  leaves the rest empty; `fast` repeats and fills the whole cycle.
- `hurry` (`:2103`) is `fast` plus a `speed` control on the value (`pat._fast(r).mul(pure({
  speed: r }))`) — it both speeds the pattern and the sample playback. Don't confuse with
  `fast`.
- The mini-notation `*N` operator compiles via `op_fast` → `{ type:'stretch', type:'fast' }`,
  and the mini builder does `pat.fast(amount)` (`mini.mjs:27`). A *patterned* factor
  (`a*<3 5>`) is supported because `fast` accepts a pattern factor.