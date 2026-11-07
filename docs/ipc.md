# Helper IPC protocol

Noctara talks to the protected-playback helper ([drm-helper.md](drm-helper.md)) over the helper's stdin and
stdout: one JSON object per line, UTF-8, each with a `"type"`. stderr carries Chromium's own diagnostics, which
Noctara logs at debug level only.

## One definition

The protocol is defined once, in [`electron/protocol.json`](../electron/protocol.json): the version and, per
direction, every message type with its fields (`string`, `number`, `boolean`, `object`; a trailing `?` marks an
optional field). The helper loads it through `protocol.js`; Noctara embeds the same file at build time
(`#embed` in `src/drm/Protocol.cpp`, tracked by the depfile). Both sides validate **every** message they send
and receive: an unknown type, a missing field or a wrong type is refused (sending) or ignored with a warning
(receiving). Lines longer than 1 MiB are dropped.

Version 1. `hello` carries Noctara's version; the helper answers `error` with code `protocol` on a mismatch, and
Noctara refuses a `ready` whose version differs from its own. Any change to fields or meanings bumps the version.

## Noctara → helper

| type | fields | meaning |
|---|---|---|
| `hello` | `version` | first message after start |
| `configure` | `proxy`, `dns` | network settings for the player's session; sent right after `hello` and on every live change (see below) |
| `load` | `id`, `url`, `licenseUrl`, `licenseToken`, `startMs`, `volume`, `autoplay` | replace the current track: the signed media playlist, SoundCloud's Widevine license server and the token it wants (`license_token` query parameter), start position, linear volume 0–1, play when ready |
| `play`, `pause`, `stop` | – | `stop` unloads |
| `seek` | `ms` | |
| `volume` | `value` | linear 0–1 (Noctara applies its loudness curve first) |
| `ping` | `seq` | heartbeat, every 5 s once ready |
| `shutdown` | – | quit now (closing stdin has the same effect) |

`configure`:

```json
{"type": "configure",
 "proxy": {"mode": "direct" | "system" | "manual",
           "rules": "http://proxy:3128", "bypass": "*.corp;<local>", "display": "http://<redacted>@proxy:3128",
           "username": "…", "password": "…"},
 "dns": {"mode": "system" | "doh", "template": "https://…/dns-query", "strict": true}}
```

`username`/`password` are present only for HTTP(S) proxies with credentials; they are used for proxy
authentication only (Electron's `login` event with `isProxy`). `load` and `configure` carry secrets: neither side
ever logs them.

## Helper → Noctara

| type | fields | meaning |
|---|---|---|
| `ready` | `version`, `widevine`, `electron`, `detail?` | handshake done; `widevine: false` (with `detail`) means the CDM is unavailable and Noctara stops the helper |
| `configured` | `proxy`, `dns` | a `configure` was applied (descriptions without credentials) |
| `state` | `id`, `state` | `loading`, `buffering`, `playing`, `paused`, `ended` |
| `position` | `id`, `ms`, `durationMs` | every 250 ms while playing, and after seeks |
| `error` | `id`, `code`, `detail` | `network` (kept; playing again retries), `drm` (license refused), `media`, `unsupported`, `protocol`; `id` is empty for errors outside a track |
| `pong` | `seq` | heartbeat answer |
| `log` | `level`, `message` | `debug`/`info`/`warning`/`error`, for Noctara's log |

Every track event carries the `id` of its `load`. Noctara gives each load a new id and drops events for older ones,
so a late event from a replaced track never moves the current one.

## Ordering, heartbeat, shutdown

- The helper processes commands strictly in order (a promise chain), so `configure` is in effect before a `load`
  that follows it. Commands for the player arriving before the page is ready are queued.
- Noctara may write before `ready`: the pipe buffers it. `ready` comes once the first `configure` is applied and
  the Widevine component is ready (on the very first start that includes downloading it, hence a start timeout of
  3 minutes).
- Heartbeat: once ready, Noctara sends `ping` every 5 s; when the helper has said nothing for 15 s it is considered
  hung and its process group is killed. Any message counts as a sign of life. The main process answers `ping`, so
  it watches the player page itself: when the page's renderer is gone it logs why and exits with code 1, which
  Noctara handles like any crash (restart, the track resumes).
- Clean shutdown: `shutdown`, close stdin, wait up to 2 s, then `SIGKILL` to the process group. See
  [drm-helper.md](drm-helper.md#processes-and-shutdown).

## The mock helper

`noctara-mock-drm-helper` (`tests/mock-drm-helper/main.cpp`, QtCore only) speaks the same protocol, validated with
the same embedded definition, and simulates playback with timers. Environment variables make it crash on load (or
once), hang, report another version or no Widevine, refuse `configure`, change the track length, or fork a child
into its process group; it echoes what it receives as `log` messages so tests can check what reached the helper. It
is a separate binary because anything linking the GUI stack spends seconds in GL driver initialisation under ASan.
