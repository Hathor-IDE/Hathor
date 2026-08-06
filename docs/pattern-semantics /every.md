# `every` (a.k.a. `firstOf`)

Apply a function to a pattern every `n` cycles, starting from the first cycle.

## What it does

`every(n, func, pat)` returns a Pattern that plays `pat` normally for `n-1` out of every `n`
cycles, and plays `func(pat)` on the remaining cycle. `every` is registered as a synonym of
`firstOf`; `lastOf` is the "starting from the last cycle" variant.

**Implementation** — `packages/core/pattern.mjs:2188` `register(['firstOf','every'], function
(n, func, pat) { ... })`.

## Exact algorithm

```
every(n, func, pat):
  pats = [ func(pat) ] + [ pat ] * (n - 1)      # unshift; func(pat) first
  return slowcatPrime(...pats)
```

That is, `pats` is an array of length `n` whose first element is `func(pat)` and the rest are
plain `pat`. Then `slowcatPrime(...pats)` (`pattern.mjs:1559`) is used:

```
slowcatPrime(p0, ..., p_{m-1}):
  pats = [reify(p) for p in args]
  query(state):
    pat_n = floor(state.span.begin) % m         # plain mod, NO skip-cycle correction
    pat   = pats[pat_n]
    return pat ? pat.query(state) : []          # undefined -> empty
  return new Pattern(query).splitQueries()
```

So on cycle index `c = floor(begin)`, the returned pattern is `pats[c mod n]`: `func(pat)` on
cycle 0, plain `pat` on cycles 1..n-1, repeat.

`firstOf` is identical (same registration). `lastOf` (`:2159`) is the mirror: `pats =
[pat]*(n-1)` then append `func(pat)`, so `func(pat)` fires on the *last* cycle of each group.

## Edge cases and invariants

- **Uses `slowcatPrime`, not `slowcat`.** This is important: `slowcatPrime` has no
  skip-cycle correction (see `slowcat.md`). For the common case where `pat` is a plain
  one-cycle pattern this makes no audible difference, but with multi-cycle/multi-step patterns
  `every`'s alternation is a *strict* index-based cycle slice and does not rewind children.
- `n == 1`: `pats = [func(pat)]`, `slowcatPrime` of one item → every cycle is `func(pat)`.
- `n == 0` or negative: `Array(n-1)` yields an empty (or length-negative) fill and `pats` has
  1 element; `slowcatPrime` indexes `% 1`. Degenerate, not explicitly guarded — treat as
  unspecified. (The `every` doc uses `n` ≥ 1.)
- Undefined `pats[pat_n]` → returns `[]` (silence), guarding against out-of-range indices
  (`slowcatPrime` comment, `:1563`).
- Registered with default `patternify=true`, `join=innerJoin`, so `n` and `func` may be
  patterns, and `func` is applied to a reified `pat`.
- **Not** `preserveSteps` (no 4th arg). Steps hint is not copied.

## Determinism notes

Fully deterministic.

## Known deviations / gotchas

- `every` is literally `firstOf` (one registration, two names). If the docs (e.g. the ICLC
  draft `paper.md` example `every(3, fast(2), sound("bd sd"))`) are read as "every 3rd cycle",
  note the implementation starts counting at cycle **0** (the first cycle is transformed), the
  same as `firstOf`'s docstring "starting from the first cycle".
- Because it depends on `slowcatPrime` (plain `%`), `every`'s transformation follows the
  *absolute* cycle index, not the phase of any wrapping `slow`/`fast`. If you layer
  `every(3, ...)` on a pattern that has already been `slow`ed, the "every 3 cycles" refers to
  result-cycles, and the source's own cycle phase is not corrected.