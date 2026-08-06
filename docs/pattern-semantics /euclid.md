# `euclid` (Euclidean rhythm / Björklund)

Change a pattern's structure to form an Euclidean rhythm: place `pulses` onsets evenly across
`steps` positions.

## What it does

`euclid(pulses, steps, pat)` returns a Pattern whose per-cycle structure is the Euclidean (or
Björklund) distribution of `pulses` onset-steps within `steps` total steps: the original
pattern's value is heard on each "on" step and silence on each "off" step. This is a *masking*
operation (`pat.struct(binaryMask)`), not a change to the values themselves.

**Implementation** — `packages/core/euclid.mjs:140`
`register('euclid', function (pulses, steps, pat) { return pat.struct(_euclidRot(pulses, steps, 0)); })`.
The mask generator is `bjorklund` (`euclid.mjs:43`), ported from Rohan Drape's Haskell "Music
Theory" module (header comment, `euclid.mjs:7-8`).

## Exact algorithm

### 1. Björklund mask generation

```
bjorklund(ons, steps):
  inverted = ons < 0
  absOns   = |ons|
  offs     = steps - absOns
  ones     = [ [1] ] * absOns        # list of lists: one list per onset
  zeros    = [ [0] ] * offs          # list of lists: one per off
  (n, x)   = _bjorklund([absOns, offs], [ones, zeros])
  pattern  = flatten(x[0]) ++ flatten(x[1])
  return inverted ? pattern.map(v => 1 - v) : pattern
```

`_bjorklund(n, x)` is the recursive Euclidean/Björklund loop (`euclid.mjs:38`):

```
_bjorklund([ons, offs], [xs, ys]):
  if min(ons, offs) <= 1: return ([ons, offs], [xs, ys])
  if ons > offs: return _bjorklund( ...left(n,x) )      # left step
  else:          return _bjorklund( ...right(n,x) )    # right step

left([ons, offs], [xs, ys]):
  (_xs, __xs) = splitAt(offs, xs)              # first `offs` elements vs rest
  return ([offs, ons - offs],
          [ zipWith(concat, _xs, ys), __xs ])

right([ons, offs], [xs, ys]):
  (_ys, __ys) = splitAt(ons, ys)
  return ([ons, offs - ons],
          [ zipWith(concat, xs, _ys), __ys ])
```

This is the standard two-row Euclidean algorithm (the "Björklund"/"Khun Chay Lo" recurrence):
repeatedly distribute the smaller run of gaps into the larger run of onsets until the 
remainder is ≤ 1. The result is a flat binary list of length `steps`. A negative `ons` inverts
the mask (swaps 1↔0).

### 2. Applying the mask

```
euclid(pulses, steps, pat):
  mask = bjorklund(pulses, steps)
  return pat.struct(mask)               # rotate 0
```

`struct(mask)` (`pattern.mjs:1257`) is `pat.keepif.out(mask)`: the mask pattern is used as the
timing structure (via `appRight`), and the event value is kept only where the mask is truthy
(= 1). Concretely, value is kept where `mask == 1`, replaced by silence (removed) where
`mask == 0`. (Mechanics of `keepif`/`out` are spelled out in `pattern.mjs:1109`,
`:1217-1218`, and `_opOut` at `:779`.)

`euclidRot`/`euclidrot` (`euclid.mjs:152`) is the same but takes a `rotation` (in steps) and
does `rotate(mask, -rotation)` before applying: `_euclidRot(pulses, steps, rotation)`
(`euclid.mjs:132`).

### Example (from the test suite)

`bjorklund(3, 8)` → `[1,0,0,1,0,0,1,0]`, so `fastcat('a').euclid(3,8)` yields events at
`0→1/8`, `3/8→1/2`, `3/4→7/8` (`packages/core/test/euclid.test.js:17-23`).

## Edge cases and invariants

- **Negative pulses inverts the mask**: `bjorklund(-3,8)` → `[0,1,1,0,1,1,0,1]`
  (`euclid.test.js:8`). `euclid` passes `pulses` straight to `bjorklund`, so this inversion
  applies.
- `pulses == steps`: all ones → full pattern step. `pulses == 0`: all zeros → silence.
  `pulses > steps`: `offs = steps - pulses < 0`; `Array(offs).fill(0)` with negative length
  yields an empty `zeros` array. **This is degenerate/unspecified** — not guarded in `bjorklund`; the
  documented usage requires `pulses ≤ steps`.
- The ICLC demo paper notes Tidal's `euclid(p, s, o)` "place p pulses evenly over s steps,
  with offset o" (`[lc-iclc]`, "Pattern Example"); Strudel's three-argument version is the
  separate `euclidRot`, so the rotation is a distinct function rather than a third positional
  arg to `euclid`. (The paper shows `[a3 c3](3, 4, 1)`, i.e. Tidal style; in Strudel the
  mini-notation `(p, s)` form has no offset — offset uses `euclidRot`. This is a genuine
  naming/API deviation to be aware of if the min-notation `(3,4,1)` is to be supported.)
- `pulses == 0` → `Array(0).fill` empty ones; `_bjorklund` returns immediately (min ≤ 1) with
  `x[0]` empty; result all zeros → silence. Fine.
- The Euclidean recurrence's known "which pulses are kept" convention: the implementation here
  agrees with Toussaint's classic results **except** a documented set of corrections. The
  mini test (`packages/mini/test/mini.test.mjs:96-127`) explicitly tweaks: `(3,4)`→`x x x ~`
  (not `x ~ x x`), `(5,6)`→`x x x x x ~`, `(5,16)`→ ends `… x ~ ~ ~`, `(7,8)`→`x x x x x x x
  ~`, commenting "Toussaint is wrong". So **Strudel intentionally deviates from Toussaint's
  published tables** for these cases; the code's recurrence output is authoritative.

## Determinism notes

Fully deterministic — pure function of `pulses` and `steps`. No randomness.

## Known deviations / gotchas

- The `[3,4]`, `[5,6]`, `[5,16]`, `[7,8]` outputs differ from Toussaint's printed tables (see
  above and `mini.test.mjs:96-127`); the tests document this as intended. Match the *code*, not
  the tables, when porting.
- `euclidRot`, `bjork`, `euclidLegato`, `euclidLegatoRot`, `euclidish`/`eish` are all separate
  registered functions sharing `bjorklund`/`_euclidRot` (`euclid.mjs:144-226`). They are not
  part of this task but share the same generator — reuse the same `bjorklund` core.
- `useRNG`/randomness and `euclid` are unrelated; nothing here is random.