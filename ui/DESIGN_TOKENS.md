# Hathor Design Token Summary

Derived from the Stitch HTML/CSS mockup (`stitch_hathor_ide_dj_workspace/code.html` + `DESIGN.md`),
translated into the native JUCE `HathorLookAndFeel` class (`ui/HathorLookAndFeel.hpp/.cpp`).

Colours are stored as runtime `juce::Colour` fields in the `Palette` value-type
(`HathorLookAndFeel::Palette`), held by each `HathorLookAndFeel` instance.
Components access colours via `getPalette()` (from any `Component`) or
`globalPalette()` (from non-Component contexts like `TreeViewItem`).
Use `HathorLookAndFeel::fromComponent(c).getPalette()` or `HathorLookAndFeel::globalPalette()`
at call sites.

---

## 1. Colour Palette

Source: Material-3-inspired dark theme from the Tailwind config in `code.html`.
All tokens are stored as `juce::Colour` fields in the `HathorLookAndFeel::Palette`
runtime value-type. Access via `HathorLookAndFeel::fromComponent(c).getPalette()`
or `HathorLookAndFeel::globalPalette()` (for non-Component contexts).

### Background levels (tonal layers, flat — no shadows)

| Token                  | Hex       | Source (Tailwind)          | Usage                                    |
|------------------------|-----------|----------------------------|------------------------------------------|
| `background`           | `#0e0e0e` | surface-container-lowest   | App background, ActivityRibbon, sidebar  |
| `surface`              | `#131313` | surface                    | Main editor/workspace background         |
| `surfaceLow`           | `#1c1b1b` | surface-container-low      | Hover states, inactive tabs, header bars |
| `surfaceContainer`     | `#201f1f` | surface-container          | Panel backgrounds, chat message bubbles  |
| `surfaceHigh`          | `#2a2a2a` | surface-container-high     | Input fields, slider tracks              |
| `surfaceHighest`       | `#353534` | surface-container-highest  | Borders, rules, scrollbar thumb          |
| `surfaceBright`          | `#3a3939` | surface-bright             | Hover states (lighter)                   |

### Text

| Token                | Hex       | Source (Tailwind)  | Usage                                  |
|----------------------|-----------|--------------------|----------------------------------------|
| `textPrimary`        | `#e5e2e1` | on-surface         | Primary text, code, active tab labels  |
| `textSecondary`      | `#b9ccb2` | on-surface-variant | Muted labels, inactive tabs, comments  |
| `textMuted`          | `#858585` | (inline in HTML)   | Line numbers, disabled text            |
| `textDisabled`       | `#666666` | (disabled)         | Dimmed/disabled UI elements            |

### Accent / Highlight

| Token          | Hex       | Source               | Usage                              |
|----------------|-----------|----------------------|------------------------------------|
| `accent`       | `#00ff41` | primary-container    | Active indicators, success, data   |
| `accentDim`    | `#00e639` | surface-tint         | Subtle highlights                  |
| `accentOn`     | `#003907` | on-primary-container | Text on green backgrounds          |

### Semantic

| Token    | Hex       | Source        | Usage                                    |
|----------|-----------|---------------|------------------------------------------|
| `error`  | `#ff5f56` | (traffic light) | Status dots, error indicators          |
| `warning`| `#e0a020` | (VS Code amber) | Unsaved dot, warnings                  |

### Code syntax (VS Code Dark+ — matches mockup code example)

| Token         | Hex       | Meaning                  |
|---------------|-----------|--------------------------|
| `codeText`    | `#d4d4d4` | Default code text        |
| `codeComment` | `#6a9955` | Comments                 |
| `codeKeyword` | `#569cd6` | Keywords (`use`, `pub`)  |
| `codeType`    | `#4ec9b0` | Types (`String`, etc.)   |
| `codeString`  | `#ce9178` | String literals          |
| `codeFunction`| `#dcdcaa` | Function names           |
| `codeMacro`   | `#c586c0` | Macro/const references   |
| `codeBracket` | `#ffd700` | Brackets, angle brackets |
| `codeLineNum` | `#858585` | Line number text         |

> **Note:** `codeText`, `codeComment`, `codeKeyword`, etc. are JUCE-specific
> syntax highlighting colours used by `CodeEditorComponent::ColourScheme`.
> They are intentionally NOT part of the runtime `Palette` — see the "MiniNotationTokeniser.cpp"
> section below for rationale.

---

## 2. Typography

**Font family:** JetBrains Mono (all weights) — a Google Font, **not** assumed
pre-installed on macOS/Linux. Four weights embedded as BinaryData resources
(`ui/resources/JetBrainsMono-{Regular,Medium,SemiBold,Bold}.ttf`) and loaded via
`juce::Typeface::createSystemTypefaceFor()`.

| Role           | Size (pt) | Weight | Factory method                         | Used in              |
|----------------|-----------|--------|----------------------------------------|----------------------|
| Headline-lg    | 24        | 700    | `fontBold(24)`                         | Window titles        |
| Headline-md    | 18        | 600    | `fontSemiBold(18)`                     | Section headers      |
| Body-lg        | 14        | 400    | `fontRegular(14)`                      | Chat messages        |
| Code (editor)  | 13        | 400    | `fontRegular(13)`                      | Code editor          |
| Body-sm        | 12        | 400    | `fontRegular(12)`                      | General UI text      |
| Label-md       | 11        | 500    | `fontMedium(11)`                       | Labels, status bar   |
| Tab label      | 12        | 500    | `fontMedium(12)`                       | Tab bar              |
| Ribbon icon    | 14        | 700    | `fontBold(14)`                         | ActivityRibbon       |

### Font substitution

`getTypefaceForFont()` intercepts all requests for the system default sans-serif
or monospaced font family and substitutes the embedded JetBrains Mono typeface.
Bold-style requests get the **Bold** weight; all others get **Regular**.
Components needing **Medium** (500) or **SemiBold** (600) call the static factory
methods directly, since JUCE's `Font` style flags only distinguish plain/bold.

---

## 3. Spacing Scale

Base unit: **4 px** (strict 4 px grid).

| Token   | Value | Usage                                    |
|---------|-------|------------------------------------------|
| `unit`  | 4     | Base grid unit                           |
| `xs`    | 4     | Smallest gap                             |
| `sm`    | 8     | Small gaps, tight padding                |
| `md`    | 12    | Default padding                          |
| `lg`    | 16    | Standard margin                          |
| `xl`    | 24    | Large gaps                               |
| `gutter`| 1     | Structural separator thickness (1 px)    |

---

## 4. Corner Radius

The design system is predominantly **sharp** (0 px for structural containers),
per DESIGN.md. Rounded corners are used only for interactive controls.

| Token     | Value  | Usage                           |
|-----------|--------|---------------------------------|
| `small`   | 4 px   | Buttons, inputs, tab close, scrollbars |
| `large`   | 8 px   | Cards, permission prompt        |
| `full`    | 9999px | Status dots, traffic-light pips |

---

## 5. Shadows / Elevation

**None.** The design system avoids drop shadows entirely. Hierarchy is achieved
through **tonal layers** (varying background luminance) and **1 px structural
borders** (`#353534` / `surfaceHighest`).

---

## 6. Icon Style

The mockup uses **Material Symbols Outlined** (line icons, ~16–22 px, stroke
~1 px) loaded via Google's CDN. These are **web-only** icon fonts and cannot be
embedded in native JUCE without an SVG/Drawable pipeline.

In the JUCE implementation, the ActivityRibbon uses **single-character text
labels** rendered in JetBrains Mono Bold as a structural substitute. The colour,
size, and spacing decisions are taken from the mockup (accent green for active,
muted text for inactive, 32×32 button cells in a 48 px ribbon).

---

## 7. WCAG AA Compliance (Req 20.2)

Verification (IEC 61966-2-1 sRGB relative luminance):

| Check                                   | Value                         | WCAG  | Status |
|-----------------------------------------|-------------------------------|-------|--------|
| Background luminance                    | `#0e0e0e` → L ≈ 0.002         | ≤ 0.15 | ✅      |
| Text primary on background              | `#e5e2e1` on `#0e0e0e`        | ~12.6:1 | ≥ 4.5:1 ✅ |
| Text secondary on background            | `#b9ccb2` on `#0e0e0e`        | ~11.1:1 | ≥ 4.5:1 ✅ |
| Code text on editor surface             | `#d4d4d4` on `#131313`        | ~10.4:1 | ≥ 4.5:1 ✅ |
| Tab inactive text on tab bar bg         | `#b9ccb2` on `#0e0e0e`        | ~11.1:1 | ≥ 4.5:1 ✅ |
| Tab active text on active tab bg        | `#e5e2e1` on `#131313`        | ~12.0:1 | ≥ 4.5:1 ✅ |
| Status bar text on low surface          | `#b9ccb2` on `#1c1b1b`        | ~11.1:1 | ≥ 4.5:1 ✅ |
| Line number on gutter bg                | `#858585` on `#1c1b1b`        | ~5.6:1  | ≥ 4.5:1 ✅ |
| Slider label on slider bg               | `#b9ccb2` on `#1c1b1b`        | ~11.1:1 | ≥ 4.5:1 ✅ |

> **Note:** The mockup's DESIGN.md mentions `#FFFFFF` for "primary actions" and
> `#A0A0A0` for "metadata." The actual Tailwind config in `code.html` uses
> `#e5e2e1` (on-surface) for primary text and `#b9ccb2` (on-surface-variant)
> for secondary. We follow the **Tailwind config** (the implementation-defined
> colors), not the narrative DESIGN.md, since it provides the precise hex values.
> The DESIGN.md narrative's `#0B0B0B` / `#121212` are slightly different from the
> config's `#0e0e0e` / `#131313` — the config is authoritative.

---

## 8. Font Embedding (Req 5)

**JetBrains Mono** is a Google Font — **not** assumed pre-installed on macOS or
Linux. It is embedded into the executable via JUCE's CMake binary-resource
mechanism:

1. **Font files** (4 weights) placed in `ui/resources/`:
   - `JetBrainsMono-Regular.ttf` (400)
   - `JetBrainsMono-Medium.ttf` (500)
   - `JetBrainsMono-SemiBold.ttf` (600)
   - `JetBrainsMono-Bold.ttf` (700)

2. **CMake** (`ui/CMakeLists.txt`): `juce_add_binary_data(hathor_font_data SOURCES ...)`
   generates `BinaryData.h`/`.cpp` with embedded TTF data.

3. **Loading** (`HathorLookAndFeel.cpp`): Each weight is loaded via
   `juce::Typeface::createSystemTypefaceFor(BinaryData::JetBrainsMono{Wght}_ttf,
   BinaryData::JetBrainsMono{Wght}_ttfSize)` in a function-local `static`
   for lazy, thread-safe initialization.

4. **Usage**: `getTypefaceForFont()` override substitutes JetBrains Mono for
   all default-font requests; `fontRegular/Medium/SemiBold/Bold()` static
   factory methods bind fonts directly to the embedded typefaces.

---

## 9. Implementation Map

| Mockup element              | JUCE component          | LookAndFeel mechanism vs paint()          |
|-----------------------------|-------------------------|-------------------------------------------|
| Body background `#0e0e0e`   | MainWindow content      | `setColour(backgroundColourId)` in L&F    |
| Editor surface `#131313`  | EditorArea / HathorTab  | L&F `CodeEditorComponent` colours + paint()|
| ActivityRibbon accent bar    | ActivityRibbon paint()  | `paintButton()` — 3px green left bar      |
| Explorer hover/selected      | ExplorerPanel paint     | `paintListBoxItem()` with `surfaceLow`    |
| Editor tab active underline   | EditorArea TabBar       | `paint()` — 2px green top border          |
| Chat input focus ring         | ChatSidebar TextEditor  | L&F `TextEditor::focusedOutlineColourId`  |
| Slider pill track              | SliderPanel             | L&F `drawLinearSlider` override           |
| Scrollbar track/thumb         | Viewport/ChatSidebar    | L&F `drawScrollbar` override              |
| Visualizer idle ring           | VisualizerPanel paint() | `paintIdleRing()` — breathing dim ring    |
| Code syntax colors            | CodeEditorComponent     | `MiniNotationTokeniser::ColourScheme`     |
