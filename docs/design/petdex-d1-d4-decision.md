# Petdex (Phase G) — D1–D4 Evidence & Decisions

## Status

**Decided / Recorded — D1–D4 shipped.** This document records the D1 (manifest
fetch/cache + opt-in picker), D2 (sprite acquisition + WebP decoding), D3
(frame slicing + animation) and D4 (licensing/attribution gate) implementation
decisions, the **verified** facts about the live Petdex external dependency,
and the concrete **D2/D3 implementation decision discovered from the actual
package format** — the deliverable PROGRAM.md Phase G requires before any
sprite work.

Every external claim below was re-verified against live data on **2026-08-12**
(manifest download, zip download, pet.json inspection, JUCE 8.0.4 source
inspection) — it is evidence, not assumption.

---

## Context

Hathor ships a Settings → Petdex section (A2) where the user may opt-in to a
Petdex mascot. Decision #5: **no default mascot** — nothing is downloaded or
displayed until the user explicitly selects a pet. D1–D4 are one own-pass:
manifest fetch/cache + picker (D1), spritesheet fetch/decode (D2), frame-slice +
animation (D3), attribution/licensing gate (D4).

---

## Verified facts (live data, 2026-08-12)

### Manifest

- **URL:** `https://petdex.dev/api/manifest`
- **Shape:** `{ "generatedAt": "<iso>", "total": N, "pets": [...] }`
- **Live count:** **4521** pets at verification time (the audit in PROGRAM.md
  recorded 4425 — the catalog is **dynamic** and must never be hard-coded).
- **Per-pet fields (8):**
  `slug, displayName, kind, submittedBy, spritesheetUrl, petJsonUrl, zipUrl,
  spriteVersionNumber`.
- **`kind` values seen:** `character`, `creature`, `object`.
- `displayName` and `submittedBy` may contain non-ASCII text (e.g. CJK names,
  numeric handles).

### `zipUrl` package (downloaded + inspected)

```
pet.json              (273 B)  — {"id","displayName","description","spritesheetPath"}
__MACOSX/._pet.json           — macOS metadata junk (must be skipped)
spritesheet.webp      (2.17 MB) — WebP spritesheet, same format as spritesheetUrl
__MACOSX/._spritesheet.webp   — junk
```

- **No license file, no COPYING/README/attribution file.**
- **No frame-grid / animation-state metadata** (nothing about "8×9 / 192×208"
  or `idle`/`working` states is present in the package).
- `petJsonUrl`'s `petjson.json` is the **same minimal content** as the zip's
  `pet.json` (verified by direct fetch).

### JUCE 8.0.4 image/network capabilities (source inspected)

- **No WebP decoder anywhere in JUCE 8.0.4** (`juce-src` tree contains no WebP
  support; image formats are PNG/JPEG/GIF + BMP etc.).
- `juce::URL::createInputStream`/`WebInputStream`:
  - macOS → NSURLSession (`juce_Network_mac.mm`) — HTTPS works out of the box.
  - Windows → WinInet (`juce_Network_windows.cpp`).
  - Linux → `juce_Network_curl.cpp`, symbols **lazily** loaded
    (`JUCE_LOAD_CURL_SYMBOLS_LAZILY`) — without curl installed the fetch fails
    cleanly (error path), it does not crash or hang.
  - `JUCE_USE_CURL=0` is already set in the build; on mac/win this is harmless.

---

## D1 decisions

1. **Small internal model** (`PetdexTypes.hpp`, JUCE-free): `PetdexPet` (the 8
   fields), `PetdexManifest`, `PetdexManifestStatus`, `PetdexManifestResult`.
   No extra framework, no generalised remote-resource layer.
2. **Service boundary** (`PetdexManifestService`, JUCE glue): the UI only sees
   the service + JUCE-free model/parser/cache/attribution types. It never sees
   URLs, cache paths, or threads.
3. **Async only.** All cache I/O + HTTP + parsing run on a detached background
   thread; results are delivered via `MessageManager::callAsync` to a callback
   the UI guards with `Component::SafePointer`. The JUCE message thread and
   audio thread are never blocked.
4. **Strictly opt-in.** The service does zero work in its constructor. The
   fetch begins only when `SettingsComponent` builds the Petdex section — which
   requires the user to open the Settings tab. Starting Hathor downloads
   nothing; selecting a pet downloads nothing (D1 downloads the *catalog*
   metadata only; pet resources are D2 work).
5. **Cache** at `<userApplicationDataDirectory>/Hathor/Petdex/manifest.json`,
   stored as an envelope `{version, fetchedAtEpochMs, manifest}`.
   - Fresh (< 24 h) cache → served with **no network call**.
   - Stale cache → served immediately (picker stays usable) while a background
     refresh runs; on failure the stale cache remains in use with an explicit
     "refresh failed" message.
   - No cache + network failure → `Offline` status surfaced in the section.
   - "Refresh catalog" button forces a network refresh (invalidation).
6. **Load policy is pure and tested** (`PetdexLoadPolicy`, JUCE-free): the
   fresh-cache / stale-cache / offline branching is a pair of pure functions
   unit-tested headless, so the D1 caching + failure behaviour has test
   coverage without needing the JUCE service.
7. **Robust parsing** (`PetdexManifestParser`, JUCE-free): malformed JSON,
   missing fields, garbage URLs (only http/https kept), entries without a
   usable `slug` (the stable selection key) are skipped, `total` is always the
   parsed count, response size capped at 8 MB, 15 s connect timeout.
8. **Selection vs resource state (requirement 11).** Selection is a persisted
   `slug` under the existing A2 key `settings.petSelection`; the catalog cache
   is manifest metadata; downloaded sprite/zip resources do not exist yet and
   will be a separate slug-keyed cache owned by D2/D3. The three are never
   conflated.
9. **A2 integration.** The Petdex section is inside the existing Settings tab,
   uses the existing Apply/Reset/close edit-buffer semantics, and persists via
   the existing `ApplicationProperties` path. "(none)" remains a valid state.
   Selection persists as the **slug** (stable), not the display name.

---

## D4 — licensing/attribution gate

**Facts:** the manifest has **no license field**, and neither the zip package
nor `pet.json`/`petjson.json` contains license or attribution data. The only
attribution information that exists anywhere is the manifest's `submittedBy`
handle. Per-pet assets are owned by their submitters; the petdex.dev platform
itself is MIT-licensed code, and the platform runs a takedown review process —
none of which is machine-readable per pet.

**Decision (`PetdexAttribution`, JUCE-free):**
- Hathor **does not invent or claim a license**. The gate never produces a
  license string.
- A pet is **displayable only when attribution can actually be shown**
  (`submittedBy` present): `canDisplay == true`, credit line
  `"Submitted by <submitter> · Petdex community gallery (petdex.dev)"`.
- If attribution cannot be established (no submitter), `canDisplay == false`
  and the UI explains **why** the pet cannot be displayed instead of silently
  rendering it.
- The Settings section always shows the credit **and** the notice that the
  manifest declares no per-pet license, for every pet that is selected.

**Documented uncertainty (not silently assumed):**
- There is **no per-pet license** in the manifest or packages; the submitter
  handle is the only attribution. Some pets are fan-art of third-party IP;
  Hathor cannot know those rights and does not attempt to — it displays the
  submitter credit that the manifest provides and no more.
- D2/D3 rendering code **must** consult `PetdexAttribution::canDisplay()`
  before showing any pet.

---

## D2/D3 implementation decision (from the actual package format)

**The `zipUrl` package does NOT avoid the WebP problem.** Its spritesheet is
`spritesheet.webp` — the identical WebP format as `spritesheetUrl`. The zip's
`pet.json` adds nothing (no grid, no animation states, no license). **D2
requires a WebP decoder, and JUCE 8.0.4 provides none** — the dependency below
is evidence-based (the live asset is WebP), satisfying the "no dependency
without evidence" rule. The zip is deliberately **not** used: fetching the
manifest's `spritesheetUrl` directly avoids zip parsing and the `__MACOSX/`
junk entirely.

### D2 — sprite acquisition + decoding (implemented)

- **Decoder:** libwebp **v1.4.0** (BSD-3), fetched via the existing
  `FetchContent` pattern (root `CMakeLists.txt`; `WEBP_BUILD_*` extras all
  OFF, static `webp` target). **Verified** in a scratch build before
  integration: compiles cleanly and a lossless RGBA encode→decode round-trip
  is pixel-exact. Linked by `hathor-ui` and `hathor-ui-tests`.
- **Fetch:** the selected pet's `spritesheetUrl` (WebP) is fetched by
  `PetdexHttp` (shared `juce::URL` GET helper, 20 s timeout, 32 MB cap — also
  reused by the D1 manifest service). The sprite bytes are cached raw on disk
  (`pets/<slug>/sprite.webp`) so later launches decode from disk.
- **Decode:** `PetdexWebpDecoder` (JUCE-free, libwebp) → unpremultiplied RGBA;
  `PetWidget` converts to a `juce::Image` with the verified JUCE pixel
  convention (premultiplied `PixelARGB`, `[B,G,R,A]` byte order) and
  un-premultiplies by alpha.
- **Off the audio path:** all download/extract/decode runs on a detached
  worker thread (`PetdexResourceService`, same discipline as the D1 service);
  results arrive via `callAsync` guarded by `Component::SafePointer`.
- **Failure handling:** network/HTTP/oversize/empty responses and decode
  failures (corrupt, truncated) are reported as a widget `Unavailable` state —
  never a crash. A corrupt disk cache is detected at decode time, removed,
  and re-downloaded. Re-decoding the same asset is avoided via a single-slot
  in-memory decoded-sprite cache.

### D3 — frame slicing + animation (implemented)

**Verified against a real pet's decoded sheet and the petdex source**
(2026-08-12): the live `homelander` spritesheet is exactly **1536 × 1872** =
8 × 192 columns × 9 × 208 rows; v2 sheets are 8 × 11 rows (1536 × 2288).
Neither the manifest nor the packages encode this — the grid is derived from
**decoded sheet dimensions** and the state mapping from the project's own
`src/lib/pet-states.ts` convention table:

| row | state | frames | durationMs |
|---|---|---|---|
| 0 | idle | 6 | 1100 |
| 1 | running-right | 8 | 1060 |
| 2 | running-left | 8 | 1060 |
| 3 | waving | 4 | 700 |
| 4 | jumping | 5 | 840 |
| 5 | failed | 8 | 1220 |
| 6 | waiting | 6 | 1010 |
| 7 | running | 6 | 820 |
| 8 | review | 6 | 1030 |

- **`PetdexFrameGrid`** (JUCE-free): analyzes decoded dimensions; sheets not a
  multiple of 192×208, or with ≠8 columns, are reported invalid (clear error,
  no garbage slicing). Only rows that exist on the sheet are advertised.
- **`PetdexAnimation`** (JUCE-free): deterministic timing — frame is a pure
  function of elapsed time inside the state
  (`perFrameMs = durationMs / frames`), so the same timeline always produces
  the same frames. State transitions reset the clock. **Safe fallback:** an
  unknown state id, a state whose row exceeds the sheet, or a state with zero
  frames falls back to `idle`; an invalid grid leaves the animation inert.
- **UI concern only:** `PetWidget` drives `PetdexAnimation` from a 40 ms
  `juce::Timer` on the message thread (~25 fps); it never runs on the audio
  thread and performs no allocation in steady state. App-state reactivity is
  minimal by design: an injected probe ("any pattern slot playing") maps to
  the `running` state, otherwise `idle` — no invented large state machine.
- The state mapping is the **petdex convention, not silently invented**: the
  exact rows/frames/durations above are copied from the project's source and
  asserted verbatim in the unit tests.

### D4 enforcement at display time (implemented)

- `PetdexAttribution::buildSnapshot()` captures the D4 record at selection
  time; it is **persisted** (`pets/<slug>/attribution.json`) beside the cached
  sprite so the gate re-runs on every launch **without the manifest or
  network** — cached pets cannot bypass it.
- `PetWidget::setSelectedPet()` is a **blocking gate**: a snapshot with
  `canDisplay == false` puts the widget in `AttributionBlocked` and the pet is
  **never drawn**. Every drawn pet shows its credit (submitter · petdex.dev)
  as a caption plus the no-per-pet-license notice in its tooltip.
- A missing/corrupt attribution snapshot at startup also blocks display (with
  the explanatory notice) — the gate fails safe, never silent.

### Launch / lifecycle decisions

- **Startup:** a persisted selection restores from the disk cache when
  present — **offline launch requires zero network**. If the selection exists
  but the sprite was never cached (e.g. the earlier download failed), the
  explicitly-requested download is completed in the background; no *catalog*
  fetch ever happens at startup, and launching with no selection does no
  network work at all.
- **Apply:** selection → resolve the manifest entry → build + persist the D4
  snapshot → gate → load sprite (memory → disk → network). Changing the
  selection re-runs the whole chain; the widget ignores results for a slug it
  no longer shows. Deselecting ("(none)") clears the widget; the persisted
  slug becomes empty.
- **Zip:** not used (see above) — `__MACOSX/` hygiene is moot.

---

## Acceptance mapping

| Requirement | Where satisfied |
|---|---|
| Reachable from Settings → Petdex | `SettingsComponent::buildPetdexSection` |
| No download/display without explicit selection | Service starts only when Settings opens; selection is a persisted slug; D2 owns resources |
| Async manifest fetch, never blocks UI threads | `PetdexManifestService` background thread + `callAsync` |
| Robust to network failure / malformed / stale | Parser skip/fallback rules; UsingCache/Offline states |
| Manifest caching works | `PetdexCacheStore` + envelope + 24 h freshness |
| A2 Apply/Reset semantics | Existing edit-buffer model; slug selection via `settings.petSelection` |
| "(none)" valid | Combo item 1, empty persisted slug |
| D4 gate before rendering | `PetdexAttribution::canDisplay` + credit/notice in UI |
| Package/image format verified from actual data | This document (live downloads + JUCE source inspection) |
| No WebP/zip dep without evidence | None added in D1; D2 decision above is evidence-based |
| D4 gate before rendering (blocking) | `PetWidget::setSelectedPet` — non-displayable snapshot is never drawn |
| Cached pets re-gated on launch | Persisted attribution snapshot + resource cache |
| Offline launch works | Disk-cached sprite decoded at startup, no network |
| Corrupt cache / failed download / failed decode | Corrupt cache removed + re-downloaded; widget `Unavailable` state |
| Animation never touches the audio thread | `PetdexAnimation` driven by a message-thread `juce::Timer` |
| No default mascot | Widget `NoPet` until an explicit selection exists |
| No duplicate settings architecture | Same A2 Settings → Petdex section; one selection key |
| D2/D3 decision documented | §"D2/D3 implementation decision" + tables above |

## Platform caveats

- Linux networking: Petdex browsing degrades gracefully to Offline/UsingCache
  when libcurl is unavailable (JUCE lazy-loads curl symbols). Revisit if a
  Linux build wants Petdex: enable curl or vendor a tiny TLS-capable fetch.
- Service shutdown: if a fetch is in flight at app exit, the destructor joins
  the worker — worst case it waits for the 15 s connect timeout.
