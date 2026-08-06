# Mini-notation grammar as actually implemented

The mini-notation parser is a PEG grammar at `packages/mini/krill.pegjs`, compiled with
[Peggy](https://peggyjs.org/) into `packages/mini/krill-parser.js`. `packages/mini/mini.mjs`
(`patternifyAST`) turns the resulting AST into Strudel patterns. This file documents the
grammar **as implemented** (from `krill.pegjs`), not just as described in the tutorial, and
lists the extra syntax beyond the core set you're porting.

The repo's own framing: the mini notation is "implemented as a PEG grammar", based on
[krill](https://github.com/Mdashdotdashn/krill) by Mdashdotdashn, and the "AST can then be used
to construct a pattern using the regular Strudel API" (`[manual]`, "Mini Notation").
`patternifyAST` walks the AST; the mapping rules are in `mini.mjs:77-178`.

Terminology used in the grammar header comment (`krill.pegjs:7-11`):
- **mini(notation)** = a series of elements between quotes
- **stack** = vertically aligned slices sharing the same overall length (the `,` op)
- **sequence** = horizontally aligned elements (space separated)
- **choose** = elements of which one is chosen at random (the `|` op)

---

## Core token: `step`

A **step** (atom) is a run of one-or-more characters matching
`unicode_letter | [0-9~] | '-' | '#' | '.' | '^' | '_'` — but the grammar *rejects* a step
that is exactly `"."` or `"_"` (used for other purposes) (`krill.pegjs:107-110`). Non-numeric
steps become string values; steps that parse as numbers become numbers
(`mini.mjs:164`). The special rest symbol `~` (and `-` as a *value*) become `silence`
(`mini.mjs:157`): in `patternifyAST`, `ast.source_ === '~' || ast.source_ === '-'` → silence.

> Note: `-` is dual-use. As a standalone step value it is silence; as a character inside a
> longer step (e.g. `c-3`) it is just a letter char of the step. The mini test asserts
> `'a - b [- c]'` ≡ `'a ~ b [~ c]'` (`mini.test.mjs:121-122`).

---

## Core combinators you're porting

### Space-separated sequences → `sequence`/`fastcat`
`sequence = '^'? (slice_with_ops)+` → `PatternStub(alignment: 'fastcat')`
(`krill.pegjs:175-176`). `patternifyAST` maps `fastcat`/default to `strudel.sequence(...)`
(`mini.mjs:139`). Each element is divided equally over the enclosing timespan
(`[lc-iclc]` "brackets: elements inside brackets are divided equally over the time of their
parent").

### `[...]` subsequences → nested sequence
`sub_cycle = '[' ws stack_or_choose ws ']'` (`krill.pegjs:113`). A sub-cycle is a
`stack_or_choose` — i.e. inside `[ ]` you may again have commas, pipes, dots, sequences, etc.
It nests arbitrarily (`c3 [d3 [e3 f3]]`).

### `<...>` slow-sequences → `slowcat`
`slow_sequence = '<' ws polymeter_stack ws '>'` — "We simply defer to a sequence and change
the alignment to slowcat" (`krill.pegjs:122-125`); the alignment is set to
`polymeter_slowcat`, and `patternifyAST` builds it as `strudel.stack(...children.map(child =>
child._slow(child.__weight)))` (`mini.mjs:95-101`). Each element lasts one cycle of the result.

### `*N` fast-stretch
`op_fast = '*' a:slice` → op `{ type:'stretch', amount:a, type:'fast' }` (`krill.pegjs:153-154`).
Builder: `pat.fast(amount)` (`mini.mjs:27`). `a` may itself be a slice/pattern
(`a*<3 5>` supported — mini test `:28-30`). No `*` without a following operand (the grammar
requires a `slice`).

### `/N` slow-stretch
`op_slow = '/' a:slice` → `{ type:'stretch', type:'slow' }` (`krill.pegjs:150-151`);
builder `pat.slow(amount)` (`mini.mjs:27`). `a` may be a slice.

### `!N` replicate
`op_replicate = ws '!' a:number?` → replicate op (`krill.pegjs:137-145`). "To support both
`x!4` and `x!!!` as equivalent": each `!` increments the count; `!` with no number adds 1
(`a ?? 2` then `-1` logic). Builder: `pat._repeatCycles(amount)._fast(amount)`
(`mini.mjs:30-34`). So `a!3` = `a a a`.

### `~` rest / silence
See `step` above: `~` → `silence`.

---

## Additional syntax present in Strudel's mini-notation (beyond the core set)

| Syntax | Rule / alignment | Meaning |
|--------|------------------|---------|
| `,` (comma) | `stack_tail`, alignment `'stack'` (`krill.pegjs:178-180`) | stack = play simultaneously. `c3,e3,g3` |
| `\|` (pipe) | `choose_tail`, alignment `'rand'` (`:183-185`) | choose one per cycle at random. Each `\|` gets its own seed (`seed++`) |
| `.` (dot) | `dot_tail`, alignment `'feet'` (`:187-189`) | "foot": splits subsequences as an alternative to `[]`. `a . b c` ≡ `a [b c]` |
| `@N` / `_N` | `op_weight`, `@` or `_` (`:134-135`) | elongation/weight. `a@3 b` → `a` 3/4, `b` 1/4. `_` also elongates: `a _ b _ _` ≡ `a@2 b@3` (`:134`, `mini.test.mjs:205-210`) |
| `(p, s)` / `(p, s, r)` | `op_bjorklund` (`:147-148`) | euclidean rhythm. `a(3,8)` → `bjorklund(3,8)`; optional 3rd is rotation. Patternable: `a(<3 5>, <8 16>)` |
| `?N` / `?` | `op_degrade` (`:156-157`) | degrade. `?` = 50%, `?0.2` = 20%. Each `?` increments the parser `seed` (used to decorrelate, see `degradeBy.md`) |
| `{...}` polymeter | `polymeter`, alignment `'polymeter'` (`:116-117`) | `{a b, c d e}` — different lengths per stack row |
| `%N` | `polymeter_steps` (`:119-120`) | explicit steps-per-cycle for a polymeter (`{a b, c d e}%5`) |
| `^` | `sequence = '^'? ...` (`:175`) | marks the sequence as a "step source" (`__steps_source`, used by `_steps` bookkeeping; see `mini.test.mjs:211-221`) |
| `:` | `op_tail` (`:159-160`) | append element (list-building). `a:b` → `['a','b']` |
| `..` | `op_range` (`:162-163`) | numeric range. `0 .. 4` → `[0,1,2,3,4]` |
| `%` (with `{ }`) | `polymeter_steps` | see above |
| Haskell-style keywords | the "experimental haskellish parser" (`krill.pegjs:202-257`) | `scale`, `target`, `euclid p s [r]`, `slow n`, `fast n`, `rotL n`, `rotR n`, `struct`, `cat[...]`, `$` (operator application), `setcps`, `setbpm`, `hush`, `//` comments |

The haskellish block (`krill.pegjs:200-281`) is a *separate, experimental* top-level grammar
(`mini_or_operator`, `statement = mini_definition / command`) — most Strudel usage goes through
the `mini`/`m` entry points which use `stack_or_choose` (`mini.mjs:226-249`). The `h()`
function in `mini.mjs:246` is the raw krill entry for that experimental surface.

---

## Compilation mapping (`mini.mjs:87-150`)

`patternifyAST` switches on `alignment`:

| AST alignment | Strudel call |
|---------------|--------------|
| `fastcat` (default, space) | `strudel.sequence(...children)`; `_steps = children.length` |
| `stack` (comma) | `strudel.stack(...children)` |
| `polymeter_slowcat` (`< >`) | `strudel.stack(...children.map(c => c._slow(c.__weight)))` |
| `polymeter` (`{ }`) | align each child by `child.fast(stepsPerCycle / child.__weight)`, then `stack` |
| `rand` (`\|`) | `strudel.chooseInWith(strudel.rand.early(randOffset * seed).segment(1), children)` |
| `feet` (dot) | `strudel.fastcat(...children)` |
| weighted (any child has `@N`) | `strudel.timeCat(...[weight, child])` |

---

## Determinism / randomness notes

- `\|` choose and `?` degrade use the same `rand` signal and the module-level
  `randOffset = 0.0003` (`mini.mjs:11`), with per-occurrence seeds from the parser's `seed`
  counter, to keep separate random choices uncorrelated. Details in `degradeBy.md`.
- Everything else (space, `[]`, `<>`, `*`, `/`, `!`, `@`, `^`, `:`, `..`, `{}`, `%`) is
  deterministic.

---

## Discrepancies / gotchas vs. common documentation

- `-` is silence **only** as a standalone step; inside a token it's a normal character. The
  tutorial may present `-` and `~` as interchangeable rests; the parser only maps the exact
  single-char step.
- The grammar requires an operand after `*` and `/` (they are `* a:slice`, `/ a:slice`); a bare
  trailing `*`/`/` is a parse error.
- `< >` internally becomes `polymeter_slowcat`, not a plain `slowcat`, so its implementation
  path (weighted stack of `slow`ed children) differs subtly from `slowcat` even though the
  audible result matches one-element-per-cycle. The mini test asserts
  `minV('<a b>') == ['a']` (only first cycle shown) (`mini.test.mjs:40-42`).
- `@N` and `_N` both implement weight/elongation; `_` without a number and repeated `_` chain
  (`a _ b _ _` ≡ `a@2 b@3`).
- Whitespace is fully flexible between tokens (`ws = [ \n\r\t\u00A0]*`), so `a( 3 , 8 )` parses.