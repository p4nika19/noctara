# Design system: Midnight Glass and Frost

The visual language follows the "Midnight Glass" reference the user supplied (`~/Downloads/Midnight Glass Home.html`). Its main traits:

- a near-black blue-violet base lit by soft aurora light in the palette colors
- light, accent-tinted glass with a sheen from the top, a 1px edge whose top catches the light, and soft drop shadows
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

The whole palette comes from **one seed** in OKLCH: the adaptive accent from cover art (see *AccentEngine* below), or the static accent. The default static accent is `#6774f9`, the OKLCH midpoint between the earlier violet and blue, at hue 275°.

- Neutrals are tinted. Their hue is the scheme's neutral hue (272° Midnight, 268° Frost) blended 20% toward the seed. The Midnight base is L 0.11: never black.
- In Midnight, base and raised glass is a light, accent-tinted veil at low opacity (10% / 17% at the default tint). Overlays (drawers, popups, tooltips) are dark glass at 86%. In Frost, glass is milky white at 52–86%.
- Contrast is enforced, not hoped for. Each foreground role moves in lightness until it meets WCAG AA against the worst case: every glass tint composited over both backdrop-lightness bounds (Midnight 0.07–0.32, Frost 0.84–0.98), plus the solid surfaces. Text needs 4.5:1; icons, `accent`, the focus ring and the waveform need 3:1. `accentText` exists so the accent can stay saturated without failing small-text contrast.
- Glow alpha scales with *Glow intensity*.
- Tests run the full matrix: {Midnight, Frost} × 12 pathological seeds × glass tint {0, 0.5, 1}.

## Backdrop

| Style | Rendering |
|---|---|
| Aurora (default) | Four large radial light pools (accent, accentLight, accentDeep, accent) over the base, plus a vignette. Drawn with radial gradients, so no blur pass is needed. Strength = 0.35 + 0.65 × *Glow intensity*. |
| Cover art | The pre-blurred, tone-mapped artwork from the AccentEngine, crossfading on track change and drifting very slowly. It falls back to aurora until a track with artwork plays. Dev flag: `--artwork <file>`. |
| Dark | A nearly plain base with a faint accentDeep shade from the top-left. |

The pools drift very slowly (22–34 s sine loops, driven by a 12 Hz clock). They animate only when *Animate backdrop* is on, Reduce motion is off, energy saving is inactive, the GPU path is active, and the window is active and not minimized. The grain texture (`image://noise`, generated once) is zero-mean: faint white and black specks, so it does not brighten the base. It is tiled over the **backdrop only**, never over text and controls. Glass panels add their own fainter grain in the glass shader.

## Button materials (`appearance.glass.buttons`)

`ButtonSurface.qml` is the single background for buttons, icon buttons and dropdowns:

| Material | Look |
|---|---|
| Frosted glass (default) | milky veil, sheen from the top, light edge, inner top highlight |
| Liquid glass | more transparent body, bright lit rim, light caught at the lower edge (accent in Midnight), rim darkening toward the bottom |
| Tinted glass | accent-tinted body with an accent edge |
| Clear | outline only |
| Solid | opaque surface |
| *flat* (internal) | toolbar / icon-button style, visible on hover |
| *primary* (internal) | accent → accentLight gradient, inner highlight, accent glow |

Hover adds an accent halo when *Button and selection glow* is on. Press scales to 0.94–0.96 with a decelerating curve and shades the bottom. *Reduce transparency* forces Solid, except for Clear.

Panel blur algorithms (Dual Kawase / Gaussian / Liquid Glass shader) are a separate setting, `appearance.glass.style`, described next.

## Glass pipeline

One shared pipeline, no per-panel blur:

```
Backdrop (aurora | cover | dark, grain)
   └─ capture: ShaderEffectSource at 1/downsample of the window (only re-rendered when the backdrop changes)
        ├─ kawase, liquid: Dual Kawase down x N -> up x N   (kawase_down.frag / kawase_up.frag)
        └─ gaussian:       horizontal pass -> vertical pass   (gaussian.frag)
              └─ output texture ──► every GlassPanel samples the texel behind itself
                                       (glass.vert maps qt_Matrix back to window coordinates)
                                       ├─ glass.frag   frosted: saturation/brightness lift, faint grain, SDF mask
                                       └─ liquid.frag  Liquid Glass (opt-in surfaces only)
```

- **Parameters** come from `theme::blurPlan(style, strength, quality)`, which is pure and unit-tested. Quality changes the capture resolution (High 1/2, Medium 1/3, Low 1/4) and, at Low, removes one Kawase level. The offsets compensate, so the blur radius stays the same: the governor changes the cost, not the look. *Blur strength* maps to 1–5 Kawase levels, or to a Gaussian sigma of 2–40 px, which is captured at a resolution that keeps sigma at or below 3/5/8 texels.
- **Cost does not grow with panels.** Each panel adds one quad that samples the shared texture. A static backdrop is never re-blurred. The aurora drift updates at 12 Hz, because it moves only a few pixels per second, so even the animated backdrop re-blurs about 12 times a second instead of at the display rate.
- **Liquid Glass** is an original shader. The rounded-rectangle SDF provides the surface normal. Within `thickness` px of the rim, the sample is pulled inwards by up to `refraction` px (lens profile `rim²`), and R/G/B bend by 1 ± `dispersion`. Light comes from `lightAngle` (top-left): the rim facing the light gets a specular highlight, the whole rim gets a Fresnel lift, and the rim facing away gets the accent-tinted catch light (Midnight) and is darkened. The parameters are per scheme (`glassTokensFor`). With the Liquid Glass style every panel uses it (`GlassPanel.liquid`, on by default). List rows and other repeated items are never GlassPanels, so the effect does not multiply across lists. The specular highlight is a thin line on the rim (`rim⁸`), so large rounded corners facing the light do not flare.
- **Solid** (requested, *Reduce transparency*, software renderer, or the governor's last tier) uses the opaque `solid*` surfaces. No shader is loaded.
- **Lit edge** (`RimLight.qml`). The light from above is drawn **on** the 1 px edge as a gradient stroke (`ShapePath.strokeGradient`, curve renderer, anti-aliased). It follows the rounded corners and fades out down the sides within about 1.6 × the radius. It replaced a straight 1 px inner line, which looked detached for three reasons: it stopped short of the corners, it sat one pixel below the edge (so the top looked doubled while the sides did not), and in Frost it was nearly opaque white. Glass panels and glass buttons use it. Liquid Glass panels light their own rim in the shader.
- **Defaults:** *Glass tint* and *Grain* start at 0% (scale 0–100%). Tint 0% is the clearest, darkest glass in Midnight: since config v6 the scale is shifted by 20%, so the former 0% is now 20% and the former 80% is now 100% (stored values are migrated). Contrast is still enforced at every setting. Grain 0% means no grain. At 0% grain the backdrop keeps a dithering floor of one to two 8-bit steps, which is invisible as texture but removes the banding rings of wide radial gradients. Config v5 restores absolute values from the short-lived relative v4 format.
- `GlassPanel.cornerInset` = r·(1 − 1/√2) is the smallest margin that keeps a content corner inside a rounded corner. Content near panel edges uses `max(margin, cornerInset)`, so large corner radii never push controls into the curve.

## AccentEngine (`theme/AccentEngine.cpp`)

- `analyseArtwork()` is pure and deterministic. It downscales to ≤ 64×64, converts to OKLab and runs k-means with k = 6 (farthest-point initialisation, no randomness). Each cluster is scored as population^0.75 × (chroma + 0.01), with penalties for near-black and near-white (L < 0.2 or > 0.93) and for dull colours (C < 0.03 or < 0.06). The seed keeps the winner's hue. Its lightness is clamped to 0.5–0.78, and its chroma is set by *Palette extraction* (Vivid 0.13–0.26, Balanced 0.07–0.15, Soft 0.04–0.085). Artwork without usable colour (white, black, grayscale) yields no seed, and the previous accent stays.
- `blurredBackdrop()` is the cover-art backdrop. It is blurred at 32×32 (three box passes in OKLab), and its lightness is remapped into the scheme's legible range (`backdropMinL..MaxL`: Midnight 0.07–0.32, Frost 0.84–0.98), so the contrast guarantees hold over any artwork. Chroma is scaled by 0.85 in Midnight and by 0.3 in Frost (a pale wash). The same image serves the GPU and software paths.
- `AccentEngine` runs both functions on the thread pool (~4 ms in release for a 600 px image), caches results per artwork key and extraction mode (LRU, 16 entries), drops stale results, and delivers seeds to `Theme` (which crossfades) through signals.

## Shadows and glows

`Shadow.qml` draws soft shadows and colored glows from an analytic rounded-rectangle SDF (`resources/shaders/shadow.frag`, compiled to `.qsb` at build time). It renders one quad, with no offscreen pass and no texture. Qt 6.12's `RectangularShadow` was evaluated and rejected: it leaks about 2 KB per destroyed instance (measured with ASan: 50 instances leak 104 KB), which hover glows and list delegates would accumulate. Shadows exist on the GPU path only.

## Settings UI

The settings UI is styled after the reference's appearance drawer:

- **Groups.** Mono uppercase group titles, with a subtle per-group reset.
- **Toggles.** A bold label with a hint on the left, the switch on the right.
- **Choices and sliders.** Segmented choices and sliders span the row under a label line that carries the value read-out in `accentText` mono.
- **Dropdowns.** Long-option dropdowns stay inline.
- **Categories.** Sidebar categories are lit pill items, the same treatment as the active navigation item.
- **Buttons follow *Button material*:** text buttons (including Reset page and the per-section Reset buttons), icon buttons, dropdowns, and the tracks of segmented controls. Exceptions: primary actions (accent gradient) and the navigation rail (flat; its moving pill is the selection).
- **Appearance page.** Starts with the **Theme** group: the colour tiles: Auto, Midnight and Frost, three tiles sharing the full width. They are the only control for `appearance.theme.mode` (a config-only key), so there is no separate Theme selector. Auto (the default) follows the desktop colour scheme live: Midnight when it is dark, Frost when it is light. A preset changes colours only: it sets the theme mode and never touches materials, shapes or layout. Midnight and Frost are product names and stay untranslated; "Auto" is translated. The active preset is detected by value.
- **Performance page.** Starts with a live status block: renderer and device, effective glass style and quality, frame rate, GPU time per frame against the budget, the active frame cap, and why effects are reduced (governor, energy saving, software rendering). When the governor has lowered quality, a **Restore full quality** button appears. The *Glass style* and *Effects quality* rows show the same reasons as status lines.
- **Quick Appearance panel.** Ctrl+Shift+A, or right-click the settings button. A dark glass drawer that slides in from the right, built from the same generated rows (`SettingsUi.subset(keys)`), with Reset (just those keys) and All settings.

## Components (`src/qml/components/`)

`GlassPanel`, `BackdropBlur`, `ButtonSurface`, `Button` (primary / secondary / ghost / danger), `IconButton`, `Icon`, `Switch`, `Slider`, `ComboBox`, `SegmentedControl` (equal-width segments when stretched), `TextField`, `ColorPicker`, `PaletteDots`, `PresetTiles`, `ScrollBar`, `Tooltip`, `Toast` + `ToastHost`, `Skeleton`, `NavRail`, `FocusRing`, `StateLayer`, `Shadow`, `Backdrop`. Controls are built on `QtQuick.Templates`, which gives keyboard handling and accessibility roles. Each one has a focus ring, hover and press feedback, and a translated accessible name.

## Fonts and icons

Manrope, Inter and JetBrains Mono (variable, OFL 1.1) are embedded and cover Latin and Cyrillic. Fallback chain: family → bundled → Noto Sans → Noto Sans CJK → Noto Color Emoji → DejaVu Sans. The icons are a Lucide subset (ISC), rasterized off-thread at the exact size, tinted with color tokens; stroke width comes from *Icon weight*.
