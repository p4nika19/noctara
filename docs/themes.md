# Themes

Noctara's look comes from design tokens (colors per scheme, radii, spacing, fonts, motion, glass parameters; see
[design.md](design.md)). A **user theme** is a TOML file that overrides any subset of them. Themes live in
`<config>/themes/` (`~/.config/noctara/themes/` on Linux) and are chosen under *Settings → Appearance → Custom
theme* (an advanced setting; config key `appearance.theme.custom` = the file name without `.toml`).

- **Data only.** A theme can only set token values. Nothing in it is executed, loaded as QML or compiled as a
  shader, and no path in it is opened. Files larger than 256 KiB are ignored.
- **Validated.** Unknown tables or keys are reported and skipped; a value of the wrong type or out of range is
  reported and the built-in value stays; only a TOML syntax error rejects the whole file. The picker lists the
  problems with their line numbers.
- **Hot-reloaded.** Saving the file applies it at once (the folder is watched). Removing it, or a syntax error,
  falls back to the built-in look.
- **Readable.** With *Enforce readable contrast* on (the default), text, icons and accents that a theme makes
  unreadable are moved to the nearest tone that meets WCAG AA against the glass they sit on (the hue is kept).
  Contrast is measured as drawn, so a translucent `#aarrggbb` colour first gains opacity, and only then changes
  tone. With it off, the picker shows a warning instead.
- **Shareable.** *Import* checks a file and copies it into the folder (under a free name); *Export* saves the
  selected one elsewhere. The folder button opens it in the file manager.

Settings still win over a theme: a font chosen in the settings beats the theme's, and blur, tint, grain, glow and
the corner/zoom/density sliders scale whatever the theme sets.

## Format

```toml
[theme]                 # all optional
name = "Aurora"         # shown in the picker; default: the file name
description = "…"
author = "…"
accent = "#3ddc97"      # the palette's seed whenever the cover art does not decide the accent

[colors.dark]           # Midnight; [colors.light] for Frost. "#rrggbb" or "#aarrggbb"
backdrop = "#06110d"
textPrimary = "#f0fff8"

[radius]                # px at 100 % zoom, 0–64
lg = 32

[space]                 # px at 100 % zoom, 0–160
rowHeight = 56

[fonts]                 # family names; missing fonts fall back to the bundled ones
heading = "Manrope"
body = "Inter"
mono = "JetBrains Mono"

[motion]                # ms at speed 1×, 0–5000
normal = 240

[glass.dark]            # Liquid Glass and rim parameters per scheme; [glass.light] for Frost
refraction = 20
```

### Tokens

| Table | Keys |
|---|---|
| `colors.dark`, `colors.light` | `backdrop`, `backdropTop`, `backdropBottom`, `scrim`, `glassTint`, `surfaceBase`, `surfaceRaised`, `surfaceOverlay`, `solidBase`, `solidRaised`, `solidOverlay`, `rimHighlight`, `rimShadow`, `shadow`, `textPrimary`, `textSecondary`, `textTertiary`, `iconPrimary`, `iconSecondary`, `divider`, `outline`, `stateHover`, `statePressed`, `accent`, `accentText`, `accentLight`, `accentDeep`, `edge`, `sheen`, `accentHover`, `accentPressed`, `onAccent`, `accentContainer`, `onAccentContainer`, `glow`, `selection`, `focusRing`, `waveformPlayed`, `waveformUnplayed`, `waveformBuffered`, `skeletonBase`, `skeletonHighlight`, `danger`, `warning`, `success` |
| `radius` | `xs`, `sm`, `md`, `lg`, `button` |
| `space` | `xxs`, `xs`, `sm`, `md`, `lg`, `xl`, `xxl`, `xxxl`, `controlHeight`, `controlHeightSmall`, `iconSize`, `iconSizeSmall`, `iconSizeLarge`, `navRailWidth`, `navRailWidthExpanded`, `nowPlayingHeight`, `rowHeight`, `pageMargin` |
| `fonts` | `heading`, `body`, `mono` |
| `motion` | `fast`, `normal`, `slow`, `theme`, `backdrop`, `shimmer` |
| `glass.dark`, `glass.light` | `rimWidth` (0–4), `refraction` (0–64), `thickness` (0–64), `dispersion`, `specular`, `fresnel`, `rimDarkening`, `saturationLift` (0–1), `brightnessLift` (−0.5–0.5), `lightAngle` (0–360°) |

Glass tint and grain are not theme tokens: they are the user's sliders.

## Examples

Three themes ship in [`docs/themes/`](themes/); copy one into the themes folder or import it.

- [`aurora.toml`](themes/aurora.toml): cool greens over a deep teal night, rounder panels, calmer motion.
- [`paper.toml`](themes/paper.toml): flat and quiet, small corners, a serif heading font (best with the *Flat*
  surface style and *Plain* buttons).
- [`compact.toml`](themes/compact.toml): built-in colors with denser rows and smaller controls.
