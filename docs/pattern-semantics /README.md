# Pattern Semantics — Strudel reference for the Hathor port

This folder documents the *intent* and *exact algorithm* of the core Strudel pattern
combinators that the from-scratch C++ port (Hathor) must reproduce. Every claim is grounded
in the Strudel source code (`packages/`) or in the project's published papers/docs. Where the
code and a paper could not both be verified, that is flagged rather than glossed over.

Everything below describes the state of the Strudel repository as checked out for this work
(`main` branch). Line numbers refer to that checkout.

---

## Sources cited

| Tag | What it is | Where it lives |
|-----|-----------|----------------|
| **`[core]`** | The implementation itself: `packages/core/pattern.mjs`, `packages/core/signal.mjs`, `packages/core/euclid.mjs`, `packages/core/hap.mjs`, `packages/core/timespan.mjs`, `packages/core/fraction.mjs`, `packages/core/util.mjs` | repo |
| **`[mini]`** | `packages/mini/mini.mjs`, `packages/mini/krill.pegjs` | repo |
| **`[manual]`** | "Strudel Technical Manual" — `docs/technical-manual/index.md` (querying, Hap, scheduling). Note: this file self-describes as "rather out of date". | repo docs |
| **`[lc-iclc]`** | "Strudel: Algorithmic Patterns for the Web", ICLC 2023 demo paper — `docs/iclc2023-paper/demo.md` (queries, `sequence`, `slowcat`, `brackets`, `euclid`, `fast`) | repo docs |
| **`[draft-iclc]`** | "StrudelCycles: live coding algorithmic patterns on the web" — `docs/iclc2023-paper/paper.md`. **This file is a draft**: it contains unimplemented editor notes (`?`, `TBD`, `??`, "General motivations / related work") and, unlike the demo paper, was not used as ground truth here. | repo docs |
| **`[alternate-timelines]`** | McLean, *Alternate Timelines for TidalCycles* (Zenodo 5788732) — the paper cited throughout the repo as the formal description of Tidal's pattern model (Pattern = function from timespan to events). Not present as open text in the repo; referenced via `citations.json`. | link in repo docs |
| **`[toussaint]`** | Toussaint, *The Euclidean Algorithm Generates Traditional Musical Rhythms*, BRIDGES 2005 — cited by the `euclid` docblock in `euclid.mjs` and by the ICLC demo paper for `euclid`. | in-repo citation + docblock |
| **`[mclean-alg-pattern]`** | McLean, *Algorithmic Pattern* (NIME, Zenodo 4813352) — cited by the ICLC demo paper intro for the algorithmic-pattern model. | link in repo docs |

> **Citation honesty caveat.** The two formal papers most directly about pattern *semantics*
> (`[alternate-timelines]`, `[toussaint]`) are only present in this repo as citation keys, not
> as full text. The algorithmic descriptions in these notes therefore come from the actual
> code (`[core]`), and the papers are cited where they are name-checked by the code/doc-comments:
> for `euclid`'s Björklund/Toussaint background and for the general "Pattern as function of
> timespan → events" framing. If you need the papers' exact wording of formal properties, fetch
> them from the linked Zenodo/CiteSeerX records; do not rely on my paraphrase as a substitute.

---

## The core data model: what a Pattern *is*

A `Pattern` is defined in `packages/core/pattern.mjs:45`:

```js
class Pattern {
  constructor(query, steps = undefined) { ... }
}
```

A Pattern is essentially a **function from a `State` to a list of `Hap`s**: it has one field
`query` of type `state => Hap[]`, plus an optional rational hint `_steps` (steps-per-cycle),
used only for the optional "steps" alignment machinery. `pattern.mjs:49` and the technical
manual's "Querying" section describe the model: *Querying = asking a Pattern for events within
a certain time span* (`[manual]`).

Because a Pattern is a pure function of time rather than a stored sequence, arbitrary-length
generative results can be represented and transformed "without having to store the resulting
sequences in memory" (`[lc-iclc]`, "Making Patterns").

### `State` (`packages/core/state.mjs`)

The argument to `query`. It carries at minimum a `TimeSpan` (`span`) and a `controls` object
(which holds `randSeed`, `_cps`, sample params, etc.). Many combinators only read/mutate
`state.span`. The `randSeed` control is the mechanism through which `seed`/`withSeed` steer
randomness (see `degradeBy.md`).

### `Hap` — an Event (`packages/core/hap.mjs`)

A `Hap` ("hap", because JS reserves the word `Event`) is a value living on a timespan. Fields
(`hap.mjs:25`):

- `whole` — `TimeSpan | undefined`. The *whole* event duration. For a discrete event it equals
  (or contains) `part`; for a **continuous** signal/event it is `undefined`.
- `part` — `TimeSpan`. The active portion of the event. **Invariant: `part` never extends
  outside `whole`** (`hap.mjs:14`). A Hap whose `part` is a strict subset of `whole` is a
  *fragment* of a longer event.
- `value` — the payload (number, string, or object with control params).
- `context` — source-location metadata (used for editor highlighting).
- `stateful` — if true, `value` is a function (used for stateful modes) — irrelevant to the
  combinators documented here.

Helpers used everywhere below:
- `wholeOrPart()` (`hap.mjs:74`) returns `whole ? whole : part` (whole of a discrete event,
  part of a continuous one).
- `hasOnset()` (`hap.mjs:89`) is true when `whole.begin == part.begin`.
- `duration` (`hap.mjs:36`).

### `TimeSpan` (`packages/core/timespan.mjs`)

`{ begin, end }`, with `begin/end` being `Fraction`s. Key operations:
- `intersection(other)` returns a new TimeSpan or `undefined` — handles zero-width (point)
  intersections at the end of a span as non-intersecting (`timespan.mjs:74`).
- `spanCycles` — splits a span on cycle (=integer) boundaries (`timespan.mjs:15`).
- `.sam()`, `.nextSam()`, `.cyclePos()`, `.wholeCycle()` are defined on `Fraction`
  (`fraction.mjs:12`) — `sam() = floor(t)`, `nextSam = sam()+1`, cycle "sam" being the start
  of a cycle.

Time is measured in **cycles**, rational (`fraction.js` underlying, wrapped in
`fraction.mjs`). `[manual]` and `[lc-iclc]` both describe one cycle as the beat/tempo unit.

### Querying entry point

`Pattern.queryArc(begin, end, controls)` (`pattern.mjs:420`) builds a `new State(new
TimeSpan(begin,end), controls)` and calls `this.query(state)`. `splitQueries()` (`pattern.mjs:437`)
re-queries the pattern once per cycle-boundary of the requested span, flattening the results —
most time-domain combinators wrap themselves in `splitQueries()` so every returned Hap is
confined to a single cycle.

### Naming/registration conventions you should know

Most end-user combinators do not write `Pattern.prototype.someMethod` directly. They call
`register(name, func, patternify, preserveSteps, join)` (`pattern.mjs:1743`), which:

1. Adds `Pattern.prototype[name]` (method form, e.g. `pat.fast(2)`).
2. Adds `Pattern.prototype['_'+name]` for the un-patternified form (used internally, e.g.
   `pat._fast(...)`); this is present whenever `arity > 1`.
3. Adds a top-level curried function on `strudelScope`.
4. If `patternify=true` (default), numeric arguments are reified into patterns and combined
   with the pattern argument using `join` (default `innerJoin`), so arguments can themselves be
   patterns (e.g. `fast("<2 3>", pat)`).
5. If `preserveSteps=true`, the resulting pattern's `steps` hint is copied from the input
   pattern.

`reify(x)` (`pattern.mjs:1409`) turns a non-Pattern into a `pure(x)` (each cycle one Hap of
value `x`), or — when a string parser has been installed (the mini package does this via
`setStringParser`) — parses strings as mini-notation. This is why JS strings "just work" as
patterns.

---

## The combinators

| File | What it does |
|------|-------------|
| [`stack.md`](stack.md) | Play several patterns simultaneously (superimposition). |
| [`fastcat.md`](fastcat.md) | Cram several patterns sequentially into one cycle. |
| [`slowcat.md`](slowcat.md) | Play one pattern per cycle, in sequence. |
| [`fast.md`](fast.md) | Speed a pattern up. |
| [`slow.md`](slow.md) | Slow a pattern down. |
| [`rev.md`](rev.md) | Time-reverse each cycle. |
| [`every.md`](every.md) | Apply a function every n cycles (alias of `firstOf`). |
| [`euclid.md`](euclid.md) | Euclidean rhythmic mask (Björklund). |
| [`degradeBy.md`](degradeBy.md) | Randomly drop events with a given probability. |
| [`iter.md`](iter.md) | Phase-shift a pattern through equal subdivisions. |
| [`jux.md`](jux.md) | Apply a function to the right channel only (through the `pan` control). |
| [`chop.md`](chop.md) | Granular slicing of samples into parts. |
| [`striate.md`](striate.md) | Slice samples and trigger progressive portions per loop. |

### Mini-notation

See [`mini-notation-grammar.md`](mini-notation-grammar.md) for the grammar as *actually*
implemented in `krill.pegjs` (which extends beyond the "literature" core of
`space`, `[]`, `<>`, `*N`, `/N`, `!N`, `~`).