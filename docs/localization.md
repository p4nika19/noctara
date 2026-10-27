# Localization

Shipped languages are English (the source language) and Russian. The setting `general.language` can be `system`, `en` or `ru`. `system` matches `QLocale::system().uiLanguages()` and falls back to English. Language names are always shown as endonyms ("English", "Русский").

## How it works

- Every user-visible string goes through `qsTr()` (QML), `tr()` (QObject C++), `QCoreApplication::translate("Context", …)`, or `QT_TRANSLATE_NOOP` for strings stored in tables such as the settings schema (context `Settings`).
- Lower layers return typed `ErrorCode`s. `i18n::errorMessage()` maps each one to a translated, actionable message (context `Errors`). Log messages stay in English.
- `LanguageManager` installs `:/i18n/noctara_<lang>.qm`, plus Qt's own `qtbase_<lang>.qm`, and emits `languageChanged`. The app connects that signal to `QQmlEngine::retranslate()`, which re-evaluates every `qsTr` binding. C++ models (settings rows) translate inside `data()` and refresh on the same signal. **No restart is needed.**
- `QLocale::setDefault` follows the chosen language. When the language matches the system language, the system locale is kept, so regional number and date formats are preserved.
- The English catalogue is loaded as well, because it carries the plural forms of `%n` strings.

## Formatting (`Format` singleton / `i18n::Formatter`)

| Function | en | ru |
|---|---|---|
| `count(1234)` | 1.2K | 1,2 тыс. |
| `bytes(1536)` | 1.5 KB | 1,5 КБ |
| `relativeTime(-3 days)` | 3 days ago | 3 дня назад |
| `duration(185000)` | 3:05 | 3:05 |

Durations are locale-independent and use tabular figures (JetBrains Mono with `tnum`). `general.formatting.time_format` and `compact_numbers` override the locale defaults.

## Plurals

Use `%n` with `QCoreApplication::translate(ctx, "…%n…", nullptr, n)` or `qsTr("%n …", "", n)`. Russian has three forms (one / few / many). Unit tests check 0, 1, 2, 5, 11, 21, 22, 25 and 101. Never build sentences from fragments; use positional `%1`, `%2`.

## Workflow

```sh
xmake i18n-update     # lupdate: scan src/ and tools/ into translations/*.ts
linguist translations/noctara_ru.ts
xmake i18n-check      # CI gate: sources in sync with .ts, no unfinished/obsolete ru strings
xmake                 # .ts → .qm (lrelease) is part of the build and embedded in resources
```

## Adding a language

1. `cp translations/noctara_en.ts translations/noctara_xx.ts`, set `language="xx"`, then run `xmake i18n-update`.
2. Add `{"xx", "Endonym"}` to `kLanguages` in `src/i18n/LanguageManager.cpp`.
3. Add the option `{"xx", "Endonym", false}` to `general.language` in `src/settings/BuiltinSchema.cpp`.

## Layout rules

Russian text runs 20–30% longer than English. Rows wrap labels and descriptions, and a control moves below the text when it would take more than 45% of the row width. Nothing uses fixed-width text containers. Truncated text elides. Layouts use anchors, so RTL mirroring can be added later.
