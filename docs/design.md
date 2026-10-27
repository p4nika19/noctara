# Design system: Midnight Glass and Frost

Both themes share the same components, geometry, motion and token structure; only the token **values** differ. Every token lives in C++ (`src/theme/`) and reaches QML through the `Theme` singleton, as one `Q_GADGET` per group. A palette change is a single notification, not dozens of property updates. Components contain no hard-coded colors or sizes.

## Token groups (`Theme.*`)

| Group | Contents |
|---|---|
| `colors` / `targetColors` | 40 semantic roles (below). `colors` animates; `targetColors` is the end state, used by rasterized icons, which crossfade between two images. |
| `space` | 2/4/8/12/16/24/32/48 plus component metrics (control height, icon sizes, nav rail, row height…), scaled by *Interface zoom* |
| `radius` | xs 6, sm 12, md 20, lg 28, pill, scaled by zoom × corner-radius mode (sharp 0.3, round 1.4, custom multiplier) |
| `elevation` | shadow blur/offset for the base, raised and overlay levels |
| `font` | display/headline/title/subtitle (Manrope), body/label/caption/overline (Inter), mono/monoSmall (JetBrains Mono, `tnum`), numeric (Inter + `tnum`) |
| `motion` | fast 120, normal 200, slow 320, theme 400, accent 500, backdrop 600 ms, multiplied by *Animation speed* (0 = off). Easings: OutExpo (standard), OutBack (emphasized, OutCubic under Reduce motion). |
| `glass` | effective style after fallbacks, requested style, blur strength, tint, grain, rim width, Liquid Glass parameters per scheme |

## Color roles

`backdrop`, `backdropTop/Bottom`, `scrim`, `glassTint`, `surfaceBase/Raised/Overlay` (translucent tints), `solidBase/Raised/Overlay` (opaque), `rimHighlight`, `rimShadow`, `shadow`, `textPrimary/Secondary/Tertiary`, `iconPrimary/Secondary`, `divider`, `outline`, `stateHover/Pressed`, `accent`, `accentHover/Pressed`, `onAccent`, `accentContainer`, `onAccentContainer`, `glow`, `selection`, `focusRing`, `waveformPlayed/Unplayed/Buffered`, `skeletonBase/Highlight`, `danger`, `warning`, `success`.

## Palette generation (`theme/Palette.cpp`)

The whole palette comes from **one seed color** in OKLCH. The seed is either the adaptive accent from cover art (slice b) or the static accent setting.

- **Neutrals** are tinted night blue. Their hue is the scheme's neutral hue (Midnight 262°, Frost 250°) blended 25% toward the seed hue, at low chroma. Midnight is deliberately very dark but never black: backdrop L = 0.115 in OKLab. The default seed and static accent are blue (OKLCH 0.62/0.16/262°, `#4a7dff`).
- **Accent**: seed hue, lightness clamped to the scheme range (Midnight 0.68–0.82, Frost 0.42–0.56), chroma capped, gamut-mapped by reducing chroma at constant L and h.
- **Contrast is enforced, not hoped for.** Each foreground role moves in lightness, away from the background, until it meets WCAG AA against the *worst-case* surface. Worst cases are each glass tint composited over both backdrop-lightness bounds, the solid surfaces, and the bare backdrop bounds (Midnight 0.07–0.24, Frost 0.86–0.98). Text needs 4.5:1; icons, focus rings and the waveform need 3:1. On-accent text is checked against the accent and its hover and pressed states.
- Tests run the full matrix: {dark, light} × {default, neon green, red, blue, yellow, magenta, cyan, grayscale, black, white, pastel, dark brown} × glass tint {0, 0.5, 1}.

### Scheme differences (excerpt)

| Token | Midnight | Frost |
|---|---|---|
| backdrop L / surface L (base/raised/overlay) | 0.115 / 0.165, 0.19, 0.22 | 0.94 / 0.985, 0.99, 0.995 |
| surface alpha (base/raised/overlay) | 0.58 / 0.68 / 0.80 | 0.64 / 0.74 / 0.86 (white, always more opaque than Midnight) |
| text L (primary/secondary/tertiary) | 0.95 / 0.80 / 0.68 | 0.22 / 0.38 / 0.48 |
| shadow | near-black, α 0.45 | accent-tinted L 0.38, α 0.16 |
| glow | accent α 0.22 | soft bloom L 0.78, α 0.22 |
| grain | 1.0 × setting | 0.6 × setting |
| Liquid Glass: refraction / dispersion / specular / rim darkening | 0.055 / 0.012 / 0.55 / 0.25 | 0.045 / 0.006 / 0.75 / 0.10 |

Theme switching (System / Midnight / Frost) and accent changes crossfade in OKLab with a single progress value: 400 ms for the theme and 500 ms for the accent, instant under Reduce motion. *System* follows `QStyleHints::colorScheme` live.

## Components (`src/qml/components/`)

`GlassPanel`, `Button` (primary/secondary/ghost/danger), `IconButton`, `Icon`, `Switch`, `Slider`, `ComboBox`, `SegmentedControl`, `TextField`, `ColorPicker`, `ScrollBar`, `Tooltip`, `Toast` + `ToastHost`, `Skeleton` (shimmer, static under Reduce motion), `NavRail`, `FocusRing`, `StateLayer`, `Backdrop`. Controls are built on `QtQuick.Templates`, which gives keyboard handling and accessibility roles. Each one has a focus ring, hover and press feedback, and a translated accessible name. Settings rows (`settings/SettingsRow.qml`) are generated from the schema.

## Fonts

Manrope, Inter and JetBrains Mono (variable, OFL 1.1) are embedded in the resources and registered through `QFontDatabase`. All three cover Latin and Cyrillic. Fallback chain: user family → bundled family → Noto Sans → Noto Sans CJK → Noto Color Emoji → DejaVu Sans. User-chosen families that aren't installed fall back to the bundled ones and are logged. License texts are in `resources/fonts/licenses/` and shown in About.

## Icons

Icons are a subset of Lucide (ISC): stroke-based, rounded caps. `IconProvider` (`image://icon/<name>?c=<color>&s=<stroke>`) rasterizes them off the GUI thread at the requested size × DPR, replacing `currentColor` and the stroke width (1.25 / 1.5 / 2.0 from *Icon weight*). There are no icon fonts.

## Glass rendering

Slice (a) ships the Solid/tint path that the software renderer also uses. The shared backdrop pipeline (Dual Kawase, Gaussian, Liquid Glass) and the quality governor come in slice (b) and plug in behind `GlassPanel`.
