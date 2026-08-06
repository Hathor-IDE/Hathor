# `striate`

Cut each sample into the given number of parts, triggering progressive portions of each
sample at each loop.

## What it does

`striate(n, pat)` assigns each event a *progressive* slice `{ begin, end }` into the sample
that advances over successive loops, while speeding the pattern up by `n` so the slices cycle
through quickly. The docstring: "Cuts each sample into the given number of parts, triggering
progressive portions of each sample at each loop" (`pattern.mjs:3562`).

**Implementation** — `packages/core/pattern.mjs:3570`
`register('striate', function (n, pat) { ... })`.

## Exact algorithm

```
striate(n, pat):
  slices        = [0, 1, ..., n-1]
  slice_objects = [ { begin: i/n, end: (i+1)/n } for i in slices ]
  slicePat      = slowcat(...slice_objects)        # one slice per cycle
  return pat
           .set(slicePat)          # structure from pat, value = slice from slicePat
           ._fast(n)               # speed up by n
           .setSteps( n * pat._steps )     # (steps enabled)
```

Key steps:

1. Build the same unit-interval `slice_objects` as `chop`.
2. `slicePat = slowcat(...slice_objects)` — a pattern whose value is slice `i` on cycle `i`
   (each slice value is `{ begin: i/n, end: (i+1)/n }`).
3. `pat.set(slicePat)` — the `set` combinator (`pattern.mjs:1083`, op `(a,b)=>b`) with default
   `in` alignment: structure is kept **from `pat`** (`appLeft`), and the value is replaced by
   the slice value from `slicePat` at that time. So each of `pat`'s events keeps its timing but
   carries a slice cursor that progresses cycle by cycle via the `slowcat`.
4. `_fast(n)` speeds the whole thing up by `n`, so within one cycle the slices sweep through
   all `n` positions (progressive portion per loop).
5. `steps` hint set to `n * pat._steps` (steps enabled).

Net effect vs `chop`: `striate` triggers each sample progressively across loops (the slice
cursor advances each time the sample recurs, and the pattern is `n`× faster), rather than
splitting one occurrence into `n` simultaneous grains.

## Edge cases and invariants

- `n == 0` → `slice_objects = []` → `slowcat()` empty → degenerate; `_fast(0)` → `silence`
  (`fast` guards factor 0). Not explicitly guarded at the `striate` level.
- `set`'s default alignment is `in` (structure from `pat`). If a different default join has
  been set via `setDefaultJoin`, `striate` calls `pat.set(...)` which uses the *current*
  default (`pattern.mjs:1233`, `:1202`). So `striate`'s behavior can change with the global
  default alignment — a subtle gotcha. (In the default config, structure is from `pat`.)
- Because `set` is applied before `_fast`, and `slowcat` of slices is *not* itself fasted, the
  slice progression rate is governed by the combined timing; the `_fast(n)` stretches the
  slice progression across the pattern's loops.
- Registered `patternify=true`; `n` may be a pattern.

## Determinism notes

Fully deterministic.

## Known deviations / gotchas

- `striate` does **not** rescale the slice into an existing `begin/end` on the sample value
  the way `chop` does (`chop`'s `merge`). It overwrites the value's `begin`/`end` with the
  unit-interval slice via `set`. If a sample already has a `begin/end`, `striate`'s `set`
  replaces them.
- Like `chop`, `striate`'s audible result depends on the sound engine honoring `begin`/`end`
  slice controls.
- The `_fast(n)` is the un-patternified `fast`; combined with `slow` elsewhere this is how you
  get `striate(n).slow(3)` in the docstring example (`:3568`).