# `stack`

Play the given patterns at the same time ("superimposition"). In mini-notation the comma
`…, …` compiles to `stack`.

## What it does

`stack` takes any number of argument patterns, reifies each into a Pattern, and returns a
Pattern whose query returns the union (concatenation) of the queries of all the argument
patterns for the same query state. All events of all patterns share the same time, i.e. all
patterns are active simultaneously over the entire query span.

**Implementation** — `packages/core/pattern.mjs:1449` `export function stack(...pats)`.

## Exact algorithm

```
stack(p0, p1, ...):
  pats = [reify(p) for p in [p0, p1, ...]]            # arrays become sequence(...)
  query(state):
    flat = []
    for p in pats: flat += p.query(state)             # concatenate, not intersect
    return flat
  steps-hint = lcm(pat._steps for pat in pats)        # only when __steps enabled
```

Notes:

1. Arrays passed as arguments are themselves treated as `sequence(...)` (nested `fastcat`).
   This guards against infinite recursion when stacking the same array-like structures.
2. The query time span is **not** altered — every sub-pattern sees the full state. There is
   no intersection of timespans (that would be the separate "alignment" machinery: `In`,
   `Out`, `Mix`, … default `In`; see `pattern.mjs:1233` and the `Pattern` abstract).
3. `steps` hint is set to the **lcm** of the children's `steps` (when the "steps" feature is
   enabled via `calculateSteps(true)`). The child *events* are not transformed; `steps` is a
   bookkeeping hint used by the optional "step" alignment helpers such as `stackLeft`,
   `stackRight`, `stackCentre`, `pm/polymeter`.

## Edge cases and invariants

- Zero patterns: `stack()` returns a Pattern with an empty `pats` array whose query returns
  `[]` — i.e. silence for the whole span. There is no special "undefined -> silence" guard in
  `stack` itself (that guard lives in the mini-notation builder, `_stackWith`).
- Individual arguments may be arrays (`stack("g3", ["e4","d4"])`); the array is first turned
  into a `sequence` before being stacked — see `pattern.mjs:1451`.
- No cycle-boundary splitting is done by `stack` itself; each child pattern is already
  responsible for its own `splitQueries` behavior when needed.
- Because events are only concatenated, if two children produce an event at the same time
  (common with `gm` inside inner `set`/apply), both survive; `stack` does **not** merge
  overlapping occurrences. Combining/disambiguation of simultaneous events is the domain of
  the applicative alignment operators (`appLeft`/`appRight`) used by `set`/`keep`, not
  `stack`.

## Determinism notes

Fully deterministic: no randomness, no dependence on a seed. Order of returned events is
the concatenation order of the argument patterns in the order given.

## Known deviations / gotchas

- The JSDoc for `stack` (`pattern.mjs:1433`) lists synonyms `polyrhythm, pr`; the top-level
  names `polyrhythm` and `pr` are aliases (`pattern.mjs:1328-1329`). `polymeter` (`pm`) is a
  *different* function (keeps each pattern at its own tempo across cycles).
- Because `stack` merely concatenates, event ordering within a cycle is argument order, which
  is printed in console order by `stack`. Structurally irrelevant for sound, but matters if you
  count on an exact ordering when porting tests.