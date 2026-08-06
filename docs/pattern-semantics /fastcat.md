# `fastcat` (a.k.a. `sequence`, `seq`)

Cram the given patterns one-after-another into a single cycle. In mini-notation the plain
space-separated sequence compiles to `fastcat`.

## What it does

`fastcat` takes several patterns and returns a Pattern that plays them *sequentially within
one cycle*: pattern 0 occupies the first fraction `1/n` of the cycle, pattern 1 the next, and
so on. It is exactly `slowcat` (one pattern per cycle) followed by a speed-up of `n`.

`sequence(...)` and `seq(...)` are pure aliases of `fastcat` (`pattern.mjs:1654`, `:1672`).
The mini-notation builder calls `sequence(...children)` for a space-separated group
(`mini.mjs:139`), and the README/`[manual]` example `sequence(c3, [e3,g3])` yields
`0→1/2: c3`, `1/2→3/4: e3`, `3/4→1: g3` (`[draft-iclc]`, `[lc-iclc]` "Representing
Patterns") — i.e. equal division.

**Implementation** — `packages/core/pattern.mjs:1638` `export function fastcat(...pats)`.

## Exact algorithm

```
fastcat(p0, p1, ..., p_{n-1}):
  result = slowcat(reify(p0), reify(p1), ...)   # slowcat handles array->sequence reify
  if n > 1:
    result = result._fast(n)                     # speed up by n
    result._steps = n
  if n == 1 and p0 has __steps_source:
    result._steps = p0._steps
  return result
```

It composes `slowcat` with `_fast(n)` (the un-patternified `fast`, `pattern.mjs:2090`), so
`fastcat` is `slowcat`-then-`fast`. `_steps` is set to `n` (number of items) — except the
single-pattern case, where it copies the source's steps.

## Edge cases and invariants

- Single argument: `slowcat` short-circuits to that pattern (`pattern.mjs:1532`); `fastcat`
  then returns it unchanged (no `_fast(1)` — that would be a no-op anyway). `steps` is taken
  from the source only if `__steps_source` is set.
- Zero arguments: `fastcat()` calls `slowcat()` (empty), `slowcat` with no args: `pats.length
  == 1` is false, so it proceeds to build a query over `pats[0] = undefined`; querying yields
  `[]` for every cycle (`_mod(sam, 0)` divides by zero). This is degenerate/undefined
  behavior; in practice the mini builder never calls it empty, and `stepcat()` explicitly
  returns `nothing` (silence) for the empty case (`pattern.mjs:3119`).
- Array arguments are recursively flattened into `fastcat` via `slowcat`'s reify step
  (`slowcat` maps `Array.isArray(pat) ? fastcat(...pat) : reify(pat)`).
- Because it is `slowcat` then `fast`, the constituent patterns' *internal* structure is
  preserved: each child is compressed into its slot but not otherwise modified.

## Determinism notes

Fully deterministic. No randomness.

## Known deviations / gotchas

- The important subtlety is that `fastcat` is defined in terms of `slowcat`, and `slowcat`
  does non-trivial offset math (see `slowcat.md`) to avoid skipping cycles of the children.
  When you port `fastcat`, port that carefully.
- `sequence`, `seq`, `fastcat`, and the top-level `cat` are distinct: `cat = slowcat`
  (`pattern.mjs:1586`), `fastcat/sequence/seq` are the crammed variant.
- There is a commented-out note in `pattern.mjs:1349` and the older `shapeshifter` transpiler
  that treated these as "factories"; the current `register` mechanism supersedes that.