# `rev`

Time-reverse each cycle of a pattern.

## What it does

`rev(pat)` returns a Pattern whose events within each cycle appear in reverse temporal order.
It reverses **within a cycle** (not the whole pattern across cycles). Contrast `revv`
(`pattern.mjs:2475`), which negates time across the whole timeline. The docstring for `revv`
makes the distinction explicit: `rev` on `<[c d] [e g]>` gives `<[d c] [g e]>` (each cycle
reverses, cycle order unchanged); `revv` gives `<[e g] [d c]>` (`pattern.mjs:2470-2473`).

**Implementation** — `packages/core/pattern.mjs:2438` `register('rev', function (pat) {...})`,
registered with `patternify=false` (2nd arg `false`), `preserveSteps=true`.

## Exact algorithm

```
rev(pat):
  query(state):
    span       = state.span
    cycle      = floor(span.begin)              # sam of the cycle being queried
    next_cycle = cycle + 1                      # nextSam
    reflect(t) = cycle + (next_cycle - t)       # mirror time within [cycle, next_cycle]
    # reflect swaps begin<->end inside the reflected span (code notes this):
    reflected_span = TimeSpan(reflect(span.end), reflect(span.begin))
    haps = pat.query( state.withSpan(reflected_span) )
    return [ hap.withSpan( s => TimeSpan(reflect(s.end), reflect(s.begin))) for hap in haps ]
  wrap in splitQueries()
```

Key points:

1. `reflect(t) = cycle + (next_cycle - t)` mirrors a time `t` across the midpoint of the cycle
   `[cycle, cycle+1]`. E.g. `t = cycle + 0.25` → `cycle + 0.75`.
2. When applying `reflect` to a *span*, the code constructs the result with begin/end swapped:
   `TimeSpan(reflect(end), reflect(begin))`. The in-source comment (`:2447`)
   "`[reflected.begin, reflected.end] = [reflected.end, reflected.begin] -- didn't work`"
   records that they had to swap explicitly rather than assign in place (the TimeSpan has no
   in-place setter and `Fraction` objects are immutable).
3. The pattern is first queried with the *reflected* span (so the right source content is
   gathered), then every returned hap has its whole and part spans reflected (swapped), so the
   event order flips within the cycle.
4. `splitQueries()` at the end ensures cycle confinement (`:2456`).

## Edge cases and invariants

- `patternify=false` here means `rev` does **not** reify/join pattern arguments (it takes a
  single pattern argument anyway).
- `preserveSteps=true`: the result carries the source's `steps` hint.
- Handles `whole == undefined` (continuous haps): `hap.withSpan` maps `whole` only if present,
  leaving `undefined` wholes untouched (`hap.mjs:78-81`). Continuous events are still reversed
  in *part* time.
- Cycle index is derived from `floor(span.begin)`; negative times reflect within their own
  cycle correctly (reflection is per-cycle absolute).

## Determinism notes

Fully deterministic.

## Known deviations / gotchas

- There is an in-source comment noting the awkwardness of producing the reflected (swapped)
  span (`:2447`). If you port TimeSpan as immutable, replicate the explicit `TimeSpan(reflect(end),
  reflect(begin))` construction — do not attempt in-place mutation.
- `rev` reverses per-cycle; for whole-timeline reversal use `revv`. The two are easy to
  conflate.
- `palindrome` (`:2524`) is defined as `pat.lastOf(2, rev)` — alternating forwards/reversed
  every other cycle, built on `lastOf`/`slowcatPrime` (see `every.md` for those semantics).