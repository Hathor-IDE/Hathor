---
name: Monolith IDE
colors:
  surface: '#131313'
  surface-dim: '#131313'
  surface-bright: '#3a3939'
  surface-container-lowest: '#0e0e0e'
  surface-container-low: '#1c1b1b'
  surface-container: '#201f1f'
  surface-container-high: '#2a2a2a'
  surface-container-highest: '#353534'
  on-surface: '#e5e2e1'
  on-surface-variant: '#b9ccb2'
  inverse-surface: '#e5e2e1'
  inverse-on-surface: '#313030'
  outline: '#84967e'
  outline-variant: '#3b4b37'
  surface-tint: '#00e639'
  primary: '#ebffe2'
  on-primary: '#003907'
  primary-container: '#00ff41'
  on-primary-container: '#007117'
  inverse-primary: '#006e16'
  secondary: '#c6c6c7'
  on-secondary: '#2f3131'
  secondary-container: '#454747'
  on-secondary-container: '#b4b5b5'
  tertiary: '#fcf8f8'
  on-tertiary: '#303030'
  tertiary-container: '#dfdcdc'
  on-tertiary-container: '#616060'
  error: '#ffb4ab'
  on-error: '#690005'
  error-container: '#93000a'
  on-error-container: '#ffdad6'
  primary-fixed: '#72ff70'
  primary-fixed-dim: '#00e639'
  on-primary-fixed: '#002203'
  on-primary-fixed-variant: '#00530e'
  secondary-fixed: '#e2e2e2'
  secondary-fixed-dim: '#c6c6c7'
  on-secondary-fixed: '#1a1c1c'
  on-secondary-fixed-variant: '#454747'
  tertiary-fixed: '#e5e2e1'
  tertiary-fixed-dim: '#c8c6c5'
  on-tertiary-fixed: '#1b1b1c'
  on-tertiary-fixed-variant: '#474746'
  background: '#131313'
  on-background: '#e5e2e1'
  surface-variant: '#353534'
typography:
  headline-lg:
    fontFamily: JetBrains Mono
    fontSize: 24px
    fontWeight: '700'
    lineHeight: 32px
    letterSpacing: -0.02em
  headline-md:
    fontFamily: JetBrains Mono
    fontSize: 18px
    fontWeight: '600'
    lineHeight: 24px
  body-lg:
    fontFamily: JetBrains Mono
    fontSize: 14px
    fontWeight: '400'
    lineHeight: 20px
  body-sm:
    fontFamily: JetBrains Mono
    fontSize: 12px
    fontWeight: '400'
    lineHeight: 18px
  label-md:
    fontFamily: JetBrains Mono
    fontSize: 11px
    fontWeight: '500'
    lineHeight: 16px
    letterSpacing: 0.05em
  code-default:
    fontFamily: JetBrains Mono
    fontSize: 13px
    fontWeight: '400'
    lineHeight: 22px
spacing:
  unit: 4px
  gutter: 1px
  sidebar-width: 260px
  margin-sm: 8px
  margin-md: 16px
  padding-input: 6px 12px
---

## Brand & Style

This design system is built for power users who value precision, speed, and focus. It draws heavily from **Minimalism** and **Modern Corporate** aesthetics, specifically tailored for integrated development environments (IDEs). 

The personality is clinical and efficient. The UI recedes into the background, allowing content—specifically code and data—to be the hero. The emotional response is one of absolute control and technical sophistication. We utilize deep blacks to reduce eye strain, high-contrast white for peak legibility, and tactical neon green accents to denote activity and "system-ready" states.

Key principles:
- **Density over whitespace:** Information density is preferred to minimize scrolling.
- **Precision:** Every border and alignment is mathematically exact.
- **Utility:** Visual flourishes are eliminated unless they serve a functional purpose (e.g., indicating the active line or a successful build).

## Colors

The palette is strictly monochromatic with a singular tactical accent. 

- **Primary (#00FF41):** Used exclusively for success states, active indicators (like the blinking cursor or active tab underline), and data visualizations.
- **Backgrounds:** The primary background is a deep, true black (#0B0B0B). Secondary surfaces (sidebars, panels) use #121212 to provide subtle depth without breaking the high-contrast aesthetic.
- **Borders:** Use #2A2A2A for all structural divisions. It should be barely visible, acting as a razor-thin guide rather than a heavy separator.
- **Typography:** Pure white (#FFFFFF) for primary actions and code; muted grey (#A0A0A0) for metadata, comments, and inactive UI labels.

## Typography

We use **JetBrains Mono** across all levels of the design system. This monospaced consistency reinforces the "developer-first" nature of the interface.

- **Scale:** Sizes are generally smaller than standard consumer apps to facilitate high information density. 
- **Hierarchy:** Established primarily through color (White vs. Grey) and weight (Medium vs. Regular) rather than significant jumps in font size.
- **Letter Spacing:** Labels and small UI elements use slightly increased letter-spacing for legibility at small scales (11px).
- **Mobile:** On smaller viewports, headlines scale down slightly, but the body text remains at 14px to ensure technical documentation remains readable.

## Layout & Spacing

The layout follows a **Fixed Grid** model for panels and a **Fluid** model for the editor core. 

- **Structural Borders:** Instead of gutters, panels are separated by 1px solid borders (#2A2A2A). This creates a "paneled" look where every pixel is accounted for.
- **Spacing Scale:** A strict 4px base unit. Most internal padding for buttons and list items should be 4px (x-axis) and 8px (y-axis).
- **Breakpoints:**
  - **Desktop (>1024px):** Multi-pane layout (Sidebar | Editor | Secondary Panel).
  - **Tablet (768px - 1024px):** Collapsible sidebar, editor becomes primary focus.
  - **Mobile (<768px):** Single-column view with a drawer-based navigation system.

## Elevation & Depth

This system avoids shadows and traditional Z-axis depth. Hierarchy is achieved through **Tonal Layers** and **Border Logic**.

- **Level 0 (Base):** #0B0B0B. The deepest layer, usually reserved for the code editor background.
- **Level 1 (Sidebars/Panels):** #121212. Slightly lighter to distinguish from the main workspace.
- **Level 2 (Modals/Overlays):** #1A1A1A with a 1px #333333 border. These should appear to sit directly on top of the UI without casting shadows.
- **Active States:** No elevation change; instead, use a 1px neon green border or a 2px left-accent line to indicate focus.

## Shapes

The design system uses **Sharp (0px)** corners for almost all elements. This emphasizes the technical, architectural nature of the IDE.

- **Buttons & Inputs:** Hard 90-degree angles.
- **Tabs:** Square corners, separated by 1px lines.
- **Exceptions:** Very small UI icons or status pips may use circular shapes, but all structural containers remain strictly rectangular.

## Components

### Buttons
- **Primary:** Black background, 1px #FFFFFF border, white text. On hover: #FFFFFF background, #0B0B0B text.
- **Tactical (Accent):** Black background, 1px #00FF41 border, #00FF41 text. 
- **Ghost:** No border, grey text. On hover: white text and #1E1E1E background.

### Input Fields
- **Default:** Background #0B0B0B, 1px #2A2A2A border. Text is white.
- **Focus:** 1px #00FF41 border. No outer glow or shadow.

### Tabs
- **Inactive:** #121212 background, grey text, 1px right/bottom border.
- **Active:** #0B0B0B background, white text, 2px top-border of #00FF41.

### Lists (File Tree)
- **Item:** 12px font size, 24px height. 
- **Hover:** Background #1E1E1E.
- **Selected:** Background #2A2A2A with a 2px left-border of #00FF41.

### Cards / Containers
- No padding-based cards. Use "Panels" defined by 1px borders and #121212 backgrounds. Header of the panel should have a subtle #1E1E1E background to distinguish the title area.