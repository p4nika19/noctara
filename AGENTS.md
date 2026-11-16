# AGENTS.md

Project memory for agents continuing work on Noctara. This repository was previously developed with Claude Code. The inherited project knowledge has been reviewed and is retained here and in the canonical documents referenced below.

## Read first

1. `CLAUDE.md` is the inherited, comprehensive engineering handbook. Treat all of its build, test, architecture, security, QML, threading, i18n and performance rules as project instructions. Do not wait for those rules to be repeated in a task.
2. `docs/product-spec-full.md` is the complete verbatim original product prompt. Later user corrections override it.
3. `docs/claude-prompt-history.md` is the imported chronological history of all 405 Noctara prompts, including recoverable paste-cache contents. Image pixels were not present in that history, so only their placeholders survive.
4. `docs/plan.md` is the product/implementation history: verified toolchain, numbered architectural decisions, vertical slices (a)–(j), completed work, review changes and open verification items. Where its status is older than the imported memory below, the newer status wins.
5. `README.md` contains installation, dependencies and public build/run commands.
6. The module-specific documents in `docs/` are authoritative for their subsystems: architecture, design, GPU, network, caching, API, authentication, playback, DRM helper/IPC, themes, localization and performance.
7. `reports/macOS port feasibility.md` and `research_notes/macOS port feasibility/` contain the later macOS feasibility investigation. `reports/` and `research_notes/` are intentionally ignored by git.

The local `.claude/` directory is empty, but the user explicitly authorised importing Claude Code's global project memory and history on 2026-11-16. The original prompt and prompt archive are now preserved in this repository; do not claim that they are unavailable.

## Current project state

- Native SoundCloud desktop client, C++23, Qt 6 Quick, xmake, clang and mold.
- All planned slices (a)–(j) are marked done in `docs/plan.md`.
- Subsequent work includes complete per-account likes/library snapshots, list-following queues, playlist artwork fallback, self-installing protected playback, queue dragging, in-list search and a Downloaded page.
- The behaviour-preserving optimisation pass was completed on 2026-11-07/08. Measured highlights: settings slider step 0.97→0.16 ms, models benchmark ~960→630 µs, button crossfade frame 5.6→4.3 ms (150 buttons), list row reuse ~2.1 ms→26 µs, and 300 tooltip hosts 104→77 ms. Keep backdrop drift enabled by default. Deliberately skipped: palette crossfade OkLab precompute, one-pass track parser and hidden Home rows.
- Platform support is currently Linux-first. macOS/Windows ports were assessed but not started: estimates were ~3–4 and ~5–7 developer-weeks respectively without DRM. Start any port by testing the stock castlabs helper for VMP/EVS enforcement; prefer a shared platform layer and macOS before Windows.

## Non-negotiable working rules

- Make minimal, vertical changes consistent with the existing module dependency direction and generated settings schema.
- Keep all user-visible strings translatable in English and Russian; regenerate/check derived documentation and translations when applicable.
- Preserve software rendering, both themes, accessibility/contrast guarantees, network pinning/redaction rules and DRM isolation.
- Do not store OAuth tokens or proxy credentials in settings. Do not let curl or FFmpeg bypass Noctara's network/cache layers.
- Use the existing fakes and test utilities; avoid timing-sensitive polling and unsafe asynchronous ownership.
- Never run `clang-tidy -fix` across multiple files. Fix findings manually.
- Before handing off substantive changes, run the narrow relevant tests, then the applicable gates listed in `CLAUDE.md` (format, warning-clean build, tests, smoke, i18n, generated docs and lint).
- The desktop entry invokes `noctara --play %u` and advertises HTTP/HTTPS plus `soundcloud://` URL handling. XDG cannot domain-restrict HTTP(S) associations; do not make Noctara the user's general browser by default.
- Use isolated XDG directories when running the app during development so real user configuration, cache and state are untouched.
- Do not perform git mutations unless explicitly requested and confirmed.

## Requirement history to preserve

The original product direction and later user-review choices are captured in `docs/product-spec-full.md`, `docs/claude-prompt-history.md` and `docs/plan.md`. In particular: presets change colours only; Midnight/Frost names stay untranslated; Auto prefers Vulkan; blur and panel surface are separate settings; shuffle uses fresh randomness; settings are the single source of truth; all protected media remains inside the DRM helper; and polish/performance work should remain evidence-driven.

Additional inherited preferences and facts:

- Work autonomously within a task. Slice reports should state implementation, verification, deviations/limitations and next work; the later instruction allowed continuing to the next slice without waiting.
- Explain technical choices in simple human language, with technical terms in parentheses where useful. The user has requested final reports/questions in Russian in past sessions.
- Keep animations enabled unless the user asks otherwise. Optimisation must not remove features or change the user experience without approval.
- Grain must remain subtle and never cover text. Large corner radii require concentric/radius-safe insets. Do not add a font-changing option; the existing font pack was explicitly approved.
- Read-only `gh` use for CI runs, logs, PRs and issues is pre-authorised. Pushes, PR creation, comments, re-runs and other outward actions require confirmation each time.
- Installer facts as of 2026-11-08: source-based one-line install was tested end-to-end on Arch; other package-manager mappings are researched and filtered at runtime. Qt 6.12 is required by `ShapePath.strokeGradient`; guarding it could permit Qt 6.11. Release installs use `--march=native --release_werror=n`. The web one-liner requires a public GitHub repository unless `NOCTARA_REPO_URL` overrides it.
- Historical memory noted these audit items as potentially open: chained `.arg` on user text, `AudioCache::m_retired` erase, double→int64 UB in Parse/DrmBackend, and unwired Time format/profile import-export. Check current code before treating any as still broken.
