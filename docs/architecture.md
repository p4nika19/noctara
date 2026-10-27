# Architecture

This document grows with each slice. It currently covers slice (a).

```
            ┌──────────────── app/main.cpp (composition root) ────────────────┐
            │ CLI parse → SettingsStore layers → graphics bootstrap → QML     │
            └──────┬───────────────┬───────────────┬──────────────┬───────────┘
                   │               │               │              │
   core ◄── settings ◄── i18n      theme           gfx            ui ──► qml/
 (errors,  (schema,     (Language- (OKLCH,        (policy,       (SettingsModel/
  logging,  store,       Manager,   Palette,       GpuProbe,      Controller,
  atomic    TOML,        Formatter, Theme,         CrashSentinel, AppController,
  file I/O) ConfigFile,  Error-     FontLoader)    Relaunch)      IconProvider)
            overrides)   Messages)
```

Static libraries per module (`noctara_<module>`); the executable only wires them together. Dependencies point downward only: `ui` may use everything, and `core` uses nothing but Qt Core.

## Settings flow

```
BuiltinSchema.cpp ──► Schema ──┬──► SettingsStore layers: Default < System < User < Env < CLI
                               │        ▲            ▲                 ▲      ▲
                               │   ConfigFile (TOML, hot reload,       │      │
                               │   debounced atomic save on an I/O     │      │
                               │   thread, unknown keys preserved)     │      │
                               │                         collectEnvironment  collectCommandLine
                               ├──► SettingsController/SettingsModel ──► generated QML rows
                               ├──► schemaMarkdown ──► docs/configuration.md (xmake docs-config)
                               └──► --print-config (effective values with sources, redacted)
```

UI edits go through `SettingsStore::setUserValue` (validated; refused when pinned by env/CLI) → `userLayerEdited` → `ConfigFile` saves after 400 ms. External edits come in through the file watcher, are parsed off-thread and applied atomically. A syntax error keeps the whole last good state; an invalid key keeps that key's previous value; both show a toast. Each change notifies listeners (`valueChanged`), so the theme, language and other consumers react live.

## Graphics bootstrap flow

See [gpu.md](gpu.md). The order in `main()` is QGuiApplication → settings → `bootstrapGraphics` (sentinel, cached probe, decision, `setGraphicsApi`) → theme/i18n → QML → `sceneGraphError` handler → disarm the sentinel on the first frame.

## i18n flow

The setting change goes to `LanguageManager`, which swaps translators and sets `QLocale::setDefault`, then emits `languageChanged`. `QQmlEngine::retranslate()` re-evaluates `qsTr` bindings, C++ models emit `dataChanged`, and `Formatter` bumps its revision.

## Theme flow

Settings, `QStyleHints::colorScheme` and the artwork seed feed `generatePalette()`, which is pure and enforces contrast. `Theme` then crossfades in OKLab and emits `Theme.colors` (one gadget). The glass style is resolved from the requested style, GPU availability, Reduce transparency and the governor cap.

## Ownership and threading

- Every object is owned by RAII or a QObject parent. Objects handed to QML from invokables are explicitly set to `CppOwnership`; ASan caught the case where they weren't.
- Config I/O runs on a dedicated single-thread pool, so writes stay in order. Settings reads are thread-safe (`shared_mutex`). Mutations happen on the GUI thread.
- Icons are rasterized on Qt Quick's async image threads.
