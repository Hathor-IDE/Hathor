# Petdex (Phase G) — D1–D4 Evidence & Decisions

## Status

**Decided / Recorded — D1 + D4 shipped.** This document records the D1 (manifest
fetch/cache + opt-in picker) and D4 (licensing/attribution gate) implementation
decisions, the **verified** facts about the live Petdex external dependency, and
the concrete **D2/D3 implementation decision discovered from the actual package
format** — the deliverable PROGRAM.md Phase G requires before any sprite work.

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
`pet.json` adds nothing (no grid, no animation states, no license).

**Decision:** D2 **requires** a WebP decoder; JUCE 8.0.4 provides none.
Recommended approach for D2: vendor/bundle a minimal libwebp (or a single-file
WebP decode) via the existing FetchContent/static-library pattern and decode
`spritesheet.webp` into `juce::Image`. The zip URL remains useful as a *single
download* that bundles `pet.json` + spritesheet, but it is not a
better-supported image format. Do NOT add a WebP dependency before D2; the
decision to add it is now **evidence-based** (the live asset is WebP and JUCE
cannot load it), satisfying the "no dependency without evidence" rule.

**Frame grid / animation states (D3):** neither the manifest nor any package
encodes the 8×9 grid, 192×208 frame size, or state names
(idle/working/…). The frame layout must be resolved from the **petdex
convention** (v1 sheets are 8 columns × 9 rows of 192×208 frames; v2 sheets add
rows) and/or spritesheet dimensions at decode time. This is a **documented
assumption to verify during D2/D3 against the real spritesheet dimensions** —
recorded here rather than silently baked in.

**Zip hygiene:** packages contain `__MACOSX/` entries; any D2 zip extraction
must skip them.

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
| No faked animation/rendering | No sprite rendering in D1; D3 gate is future work |
| D2/D3 decision documented | §"D2/D3 implementation decision" above |

## Platform caveats

- Linux networking: Petdex browsing degrades gracefully to Offline/UsingCache
  when libcurl is unavailable (JUCE lazy-loads curl symbols). Revisit if a
  Linux build wants Petdex: enable curl or vendor a tiny TLS-capable fetch.
- Service shutdown: if a fetch is in flight at app exit, the destructor joins
  the worker — worst case it waits for the 15 s connect timeout.
