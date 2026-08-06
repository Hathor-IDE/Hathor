# `slowcat` (a.k.a. `cat`)

Play the given patterns one per cycle, in sequence across cycles. In mini-notation the
angled `<…>` slow-sequence compiles to `slowcat`.

## What it does

`slowcat` returns a Pattern that switches between its arguments one per cycle: on cycle 0 you
hear pattern 0, cycle 1 pattern 1, …, then it wraps around. Each argument spans exactly one
cycle of the result.

**Implementation** — `packages/core/pattern.mjs:1528` `export function slowcat(...pats)`.
`cat` is a pure alias (`pattern.mjs:1586`), and the `<…>` mini syntax maps to
`polymeter_slowcat` at the AST then to `slowcat` in the builder (`mini.mjs:96`,
`element-ast` in `mini.mjs`).

## Exact algorithm

```
slowcat(p0, ..., p_{m-1}):
  pats = [ (Array.isArray(pat) ? fastcat(...pat) : reify(pat)) for pat in args ]
  if m == 1: return pats[0]                # FAST PATH
  query(state):
    span   = state.span
    pat_n  = _mod(floor(span.begin), m)    # pick which constituent, handles negative time
    pat    = pats[pat_n]                   # if undefined (past), return []
    # align cycles of the chosen child so we never *skip* its cycles:
    offset = floor(span.begin) - floor(span.begin / m)
    return pat.withHapTime(t => t + offset)          # shift child haps forward by offset
             .query(state.withSpan(span => {
                 span.time(g => g - offset)          # shift query span back by offset
             }))
  steps = lcm(pat._steps for pat in pats)
  return new Pattern(query).splitQueries().setSteps(steps)
```

### Why the `offset` term exists

The comment in the code (`pattern.mjs:1544-1546`) explains: "A bit of maths to make sure that
cycles from constituent patterns aren't skipped. For example if three patterns are slowcat-ed,
the fourth cycle of the result should be the second (rather than fourth) cycle from the first
pattern." Without this shift, a query for result-cycle 3 (which maps to child index 0,
child-cycle 3) would pull child's 4th cycle instead of rewinding to the child's cycle "1".

Concretely `offset = floor(begin) - floor(begin / m)`. The query span is shifted back by
`offset`, the child is queried in that shifted span, and the returned hap times are shifted
forward by `offset`. `\.s` `splitQueries()` at the end confines everything to cycle bounds.

## Edge cases and invariants

- **Empty**: no args → `pats.length != 1`, `pats[0]` undefined; query returns `[]` for each
  cycle. Degenerate (see `fastcat.md`).
- **Single**: returned immediately without offset math or `splitQueries` — identity.
- **Negative cycles** (querying the past): `_mod(span.begin.sam(), m)` makes the index
  non-negative (`_mod` is the true-modulo at `util.mjs:96`), but if the computed index lands
  on `undefined` the code returns `[]` (comment: "pat_n can be negative, if the span is in the
  past"). Concretely, `pats[pat_n]` is shape-checked with `if (!pat) return []`.
- Children's cycles are mapped so they repeat, never skipped (the offset above).
- `steps` hint = lcm of children's steps (when steps are enabled).
- The 1-child case does *not* wrap in `splitQueries`.

## Determinism notes

Fully deterministic. The offset is a pure function of `span.begin`.

## Known deviations / gotchas

- `slowcat` uses the *corrected* cycle mapping (offset trick). The older/simpler
  `slowcatPrime` (`pattern.mjs:1559`) does **no** such mapping and can "skip cycles" —
  `pats[Math.floor(begin) % m]`, undefined → `[]`. `every` (see `every.md`) is built on
  `slowcatPrime`, so its cycle-counting semantics differ from `slowcat`! Port both.