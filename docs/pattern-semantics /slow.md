# `slow` (a.k.a. `sparsity`)

Slow a pattern down by a factor. In mini-notation, `/N` compiles to `slow(N)`.

## What it does

`slow(factor, pat)` returns a Pattern where the source plays once over `factor` cycles of the
result, i.e. the pattern's content is stretched. It is defined in terms of `fast` with the
reciprocal factor.

**Implementation** — `packages/core/pattern.mjs:2119` `register(['slow','sparsity'],
function (factor, pat) { return pat._fast(Fraction(1).div(factor)); })`.

## Exact algorithm

```
slow(factor, pat):
  if factor == 0: return silence
  return pat._fast( 1 / factor )     # fast with reciprocal factor
```

That is exactly the inverse operation of `fast`: query the source over `[a/factor, b/factor]`
(a smaller window) and multiply returned hap times by `factor` (stretch). Since the source is
cyclic, you see fewer cycles of the source per result-cycle, each stretched.

## Edge cases and invariants

- `factor == 0` → **silence** (`:2120`), matching `fast`'s guard. Here it also avoids
  `Fraction(1).div(0)`.
- `factor` may be a Pattern (`slow("<2 3>", pat)`), same patternified path as `fast`.
- Reciprocal is computed as exact `Fraction`, preserving rational precision.
- Unlike `fast`, `slow` does **not** pass `preserveSteps=true` in its registration (the
  `register` call has no 4th arg). In practice the inner `_fast` carries the steps through its
  own `preserveSteps`, so steps behave as with `fast`; but be aware the two are not identically
  plumbed.

## Determinism notes

Fully deterministic.

## Known deviations / gotchas

- `sparsity` is a synonym (`:2119`).
- `slow` is the same relation to `/` in mini notation as `fast` is to `*`
  (`op_slow` → `{ type:'stretch', type:'slow' }`, builder `pat.slow(amount)`, `mini.mjs:27`).
- `slow` should not be confused with `slowcat` (one pattern per cycle) — `slow` stretches a
  *single* pattern's time, `slowcat` time-multiplexes several patterns across cycles.
- Because `slow` is literally `_fast(1/factor)`, any quirk of `fast`'s time-scaling applies
  here too (see `fast.md`).