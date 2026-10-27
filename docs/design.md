# Design system: Midnight Glass and Frost

The visual language follows the "Midnight Glass" reference the user supplied (`~/Downloads/Midnight Glass Home.html`). Its main traits:

- a near-black blue-violet base lit by soft aurora light in the palette colors
- light, accent-tinted glass with a sheen from the top, a 1px light edge, an inner highlight and soft drop shadows
- pill-shaped controls, gradient primary actions and accent glows
- mono uppercase group labels, and a compact appearance panel with presets

Frost is the light counterpart. It uses the same structure with pale bases, milky white glass and dark ink.

Two deliberate deviations from the reference, both at the user's request:

- The bundled fonts stay as they are (Manrope headings, Inter text, JetBrains Mono). There is no font switcher.
- Glow is toned down by default (*Glow intensity* 30%), and no animation overshoots its target.

Every token lives in C++ (`src/theme/`) and reaches QML through the `Theme` singleton, as one `Q_GADGET` per group. Components contain no hard-coded sizes. Fixed colors appear only as deliberate highlight constants: white veils and black inner shades.

## Token groups (`Theme.*`)

| Group | Contents |
|---|---|
| `colors` / `targetColors` | 45 semantic roles, listed below. `colors` animates. `targetColors` is the end state, used by rasterized icons, which crossfade two images instead of re-rendering every frame. |
| `space` | 2…48 px scale plus component metrics, scaled by *Interface zoom*. *Density* (compact 0.82 / comfortable 1 / spacious 1.18) scales gaps only, not control sizes. |
| `radius` | xs 6, sm 12, md 20, lg 28 (panels), pill, and `button` (pill / soft 14 / sharp 6 from *Button shape*). Scaled by zoom and the corner-radius mode. |
| `font` | display 38/800, headline 30/800, title 21/700, subtitle 17/700 (Manrope); body, bodyStrong, label 13/600, caption (Inter); `eyebrow` (Inter 11/800, uppercase, +1.7 tracking); `groupTitle` (JetBrains Mono 11, uppercase, +1.5); mono/monoSmall/numeric with tabular figures |
| `motion` | fast 120, normal 200, slow 320, theme 400 ms; accent crossfade 350 / 1200 / 2600 ms from *Colour transitions*. All are multiplied by *Animation speed* (0 = off). Easing is decelerating only (`OutExpo`, `OutQuint`), so nothing overshoots. |
| `glass` | effective panel style after fallbacks, blur strength, tint, grain, Liquid Glass parameters per scheme, `buttons` (button material), `backdrop` (aurora / cover / dark), `backdropAnimated`, `glow` and the three glow toggles |

## Color roles

Backdrop: `backdrop`, `backdropTop/Bottom`, `scrim`.

Glass: `glassTint`, `surfaceBase/Raised/Overlay`, `solidBase/Raised/Overlay`, `edge`, `sheen`, `rimHighlight`, `rimShadow`, `shadow`.

Content: `textPrimary/Secondary/Tertiary`, `iconPrimary/Secondary`, `divider`, `outline`, `stateHover/Pressed`.

Accent family: `accent` (fills, icons, selection; 3:1), `accentText` (small accent text; 4.5:1), `accentLight` (the lighter companion hue, +40°, used for gradients and the aurora), `accentDeep` (the deep shade), `accentHover/Pressed`, `onAccent`, `accentContainer`, `onAccentContainer`, `glow`, `selection`, `focusRing`.

Data and status: `waveformPlayed/Unplayed/Buffered`, `skeletonBase/Highlight`, `danger`, `warning`, `success`.

## Palette generation (`theme/Palette.cpp`)

The whole palette comes from **one seed** in OKLCH: the adaptive accent from cover art (slice b), or the static accent. The default static accent is `#6774f9`, the OKLCH midpoint between the earlier violet and blue, at hue 275°.

- Neutrals are tinted. Their hue is the scheme's neutral hue (272° Midnight, 268° Frost) blended 20% toward the seed. The Midnight base is L 0.11: never black.
- In Midnight, base and raised glass is a light, accent-tinted veil at low opacity (10% / 17% at the default tint). Overlays (drawers, popups, tooltips) are dark glass at 86%. In Frost, glass is milky white at 52–86%.
- Contrast is enforced, not hoped for. Each foreground role moves in lightness until it meets WCAG AA against the worst case: every glass tint composited over both backdrop-lightness bounds (Midnight 0.07–0.32, Frost 0.84–0.98), plus the solid surfaces. Text needs 4.5:1; icons, `accent`, the focus ring and the waveform need 3:1. `accentText` exists so the accent can stay saturated without failing small-text contrast.
- Glow alpha scales with *Glow intensity*.
- Tests run the full matrix: {Midnight, Frost} × 12 pathological seeds × glass tint {0, 0.5, 1}.

## Backdrop

| Style | Rendering |
|---|---|
| Aurora (default) | Four large radial light pools (accent, accentLight, accentDeep, accent) over the base, plus a vignette. Drawn with radial gradients, so no blur pass is needed. Strength = 0.35 + 0.65 × *Glow intensity*. |
| Cover art | The blurred artwork. It falls back to aurora until a track with artwork plays (slices b and e). |
| Dark | A nearly plain base with a faint accentDeep shade from the top-left. |

The pools drift very slowly (22–34 s sine loops). They animate only when *Animate backdrop* is on, Reduce motion is off, the GPU path is active, and the window is active and not minimized. A tiled grain texture (`image://noise`, generated once) sits over everything at an opacity set by *Grain*.

## Button materials (`appearance.glass.buttons`)

`ButtonSurface.qml` is the single background for buttons, icon buttons and dropdowns:

| Material | Look |
|---|---|
| Frosted glass (default) | milky veil, sheen from the top, light edge, inner top highlight |
| Liquid glass | more transparent body, bright rim, specular glint near the top-left (the light direction), light caught at the lower edge (accent in Midnight), rim darkening toward the bottom |
| Tinted glass | accent-tinted body with an accent edge |
| Clear | outline only |
| Solid | opaque surface |
| *flat* (internal) | toolbar / icon-button style, visible on hover |
| *primary* (internal) | accent → accentLight gradient, inner highlight, accent glow |

Hover adds an accent halo when *Button and selection glow* is on. Press scales to 0.94–0.96 with a decelerating curve and shades the bottom. *Reduce transparency* forces Solid, except for Clear.

Panel blur algorithms (Dual Kawase / Gaussian / Liquid Glass shader) are a separate setting, `appearance.glass.style`. The shared backdrop pipeline that feeds them lands in slice (b).

## Shadows and glows

`Shadow.qml` draws soft shadows and colored glows from an analytic rounded-rectangle SDF (`resources/shaders/shadow.frag`, compiled to `.qsb` at build time). It renders one quad, with no offscreen pass and no texture. Qt 6.12's `RectangularShadow` was evaluated and rejected: it leaks about 2 KB per destroyed instance (measured with ASan: 50 instances leak 104 KB), which hover glows and list delegates would accumulate. Shadows exist on the GPU path only.

## Settings UI

The settings UI is styled after the reference's appearance drawer:

- **Groups.** Mono uppercase group titles, with a subtle per-group reset.
- **Toggles.** A bold label with a hint on the left, the switch on the right.
- **Choices and sliders.** Segmented choices and sliders span the row under a label line that carries the value read-out in `accentText` mono.
- **Dropdowns.** Long-option dropdowns stay inline.
- **Categories.** Sidebar categories are lit pill items, the same treatment as the active navigation item.
- **Appearance page.** Starts with **presets** (Midnight, Glacier, Neon) and the live palette dots. A preset only writes ordinary settings, and the active one is detected by value (otherwise "custom").
- **Quick Appearance panel.** Ctrl+Shift+A, or right-click the settings button. A dark glass drawer that slides in from the right, built from the same generated rows (`SettingsUi.subset(keys)`), with Reset (just those keys) and All settings.

## Components (`src/qml/components/`)

`GlassPanel`, `ButtonSurface`, `Button` (primary / secondary / ghost / danger), `IconButton`, `Icon`, `Switch`, `Slider`, `ComboBox`, `SegmentedControl` (equal-width segments when stretched), `TextField`, `ColorPicker`, `PaletteDots`, `PresetTiles`, `ScrollBar`, `Tooltip`, `Toast` + `ToastHost`, `Skeleton`, `NavRail`, `FocusRing`, `StateLayer`, `Shadow`, `Backdrop`. Controls are built on `QtQuick.Templates`, which gives keyboard handling and accessibility roles. Each one has a focus ring, hover and press feedback, and a translated accessible name.

## Fonts and icons

Manrope, Inter and JetBrains Mono (variable, OFL 1.1) are embedded and cover Latin and Cyrillic. Fallback chain: family → bundled → Noto Sans → Noto Sans CJK → Noto Color Emoji → DejaVu Sans. The icons are a Lucide subset (ISC), rasterized off-thread at the exact size, tinted with color tokens; stroke width comes from *Icon weight*.
