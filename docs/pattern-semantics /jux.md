# `jux` (and `juxBy`, `juxFlip`)

Apply a function to a pattern in one stereo channel only ("juxtaposition"), creating a
stereo effect.

## What it does

`jux(func, pat)` returns a Pattern of the same structure, but each event is given a `pan`
value so that the pattern is heard in the **left** channel, while a copy to which `func` has
been applied is heard in the **right** channel. The two are combined with `stack`.

The docstring: "The jux function creates strange stereo effects, by applying a function to a
pattern, but only in the right-hand channel" (`pattern.mjs:2570`).

**Implementation**:
- `packages/core/pattern.mjs:2579` `register('jux', (func, pat) => pat._juxBy(1, func, pat))`
- The real work is `juxBy` at `:2541` `register(['juxBy','juxby'], function (by, func, pat) {...})`.
- `juxFlip`/`flux` (`:2594`) = `juxFlipBy(1, func, pat)`.

## Exact algorithm

```
jux(func, pat):
  return juxBy(1, func, pat)

juxBy(by, func, pat):
  by = by / 2
  left  = pat.withValue( v => { ...v, pan: (v.pan ?? 0.5) - by } )
  right = func( pat.withValue( v => { ...v, pan: (v.pan ?? 0.5) + by } ) )
  return stack(left, right).setSteps( lcm(left._steps, right._steps) )
```

Key points:

1. `by` is halved so that the effective pan offset from center is `±by/2` (default `by=1` →
   left at `pan=0`, right at `pan=1`, full stereo).
2. **Left**: every value is a copy of the original with `pan` set to `(existing pan ?? 0.5) -
   by`. If a value already has a `pan`, it is offset from that, not overwritten
   (`elem_or(val,'pan',0.5)`, `:2543-2548`).
3. **Right**: `func` is applied to a copy that has `pan` set to `(existing ?? 0.5) + by`. The
   function may itself set `pan` (e.g. `jux(rev)`, `jux(press)`, `jux(iter(4))` — docstring
   examples); the `pan` written by `func` overrides the offset `+by` if it sets `pan`, because
   `withValue`/`fmap` replaces the whole value object after the function runs. (If `func`
   doesn't touch `pan`, the right channel keeps `+by`.)
4. The two are combined with `stack` (`:2552`) and `steps` set to the lcm of the two sides'
   steps.

### `juxFlip` / `juxFlipBy`

`juxFlipBy(by, func, pat)` = `juxBy( slowcat(by, -by), func, pat )` (`:2565`): the pan offset
alternates sign each cycle, so the ears "flip" (`juxFlip` docstring "flips the ears each
cycle").

## Edge cases and invariants

- `func` is applied only to the **right** copy. `jux` never modifies the left copy except for
  its `pan`.
- If the pattern's values are not objects (plain strings/numbers), `withValue` merging
  `{ ...v, pan }` will attach `pan` to a non-object? Actually `Object.assign({}, val, {...})`
  with a primitive `val` produces `{0: 'a', pan: ...}` — **this is a footgun**: `jux` assumes
  object values (control params). The `pan` control is meaningful only for sample/synth values
  that are objects. In practice `jux` is used on patterns that carry `pan`-able sound values.
- `by` default `1`; `juxBy(0, func, pat)` → `by=0`, both channels at `pan=0.5` → effectively
  mono (both copies identical pan) but still doubled.
- Registered `patternify=true`; `func` and `by` may be patterns.
- `steps` = lcm of the two sides.

## Determinism notes

Fully deterministic (composition of `withValue`, `stack`). No randomness unless `func` brings
it.

## Known deviations / gotchas

- `jux`'s left copy uses `pan = 0.5 - by` (with `by=1` → `pan=-0.5`? No: `by = 1/2 = 0.5`,
  so left `pan = 0.5 - 0.5 = 0`, right `pan = 0.5 + 0.5 = 1`). So `jux` alone gives hard
  left/right panning. `juxBy` lets you set a narrower width.
- The `pan` control is what SuperDough/webaudio reads to place audio in the stereo field; if
  the C++ port's sound engine has no `pan` control, `jux` has no audible effect. The combinator
  itself is just a pan-annotating `stack` — that's the whole "effect".
- Aliases: `juxby` for `juxBy`; `juxflip`, `flux` for `juxFlip` (`:2541`, `:2562`,
  `:2594`). `fluxBy` is an alias of `juxFlipBy`.