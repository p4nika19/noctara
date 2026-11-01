# Protected playback: the DRM helper

Many SoundCloud tracks (most major-label releases, also for free accounts) are streamed only as encrypted HLS.
Noctara plays them in a small background helper: castlabs Electron ("Electron for Content Security") with the
Widevine CDM, playing through the standard EME path. Noctara controls it over a pipe and never touches the media:
nothing of the stream, the keys or the licenses passes through Noctara, and nothing is stored. The project offers
no way to save protected content, and none may be added.

## Pieces

```
 PlaybackController ──► drm::DrmBackend (IPlaybackBackend) ──► drm::DrmHost ══ stdin/stdout JSON lines ══╗
   picks the backend      resolves {playlist URL,                 process, heartbeat,                     ║
   per track              license token} per load                 restarts, idle shutdown                 ║
                                                                                                          ▼
                              castlabs Electron (headless):  main.js ── preload.js ── player.html + player.js
                                                             network, allowlist      hls.js → MSE → <audio>,
                                                             protocol validation     Widevine (EME) license
```

- **`electron/`** is the helper app: `main.js` (process, sessions, network configuration, protocol),
  `preload.js` (the page's only bridge: receive commands, send events), `player.html`/`player.js` (one `<audio>`
  element fed by hls.js through Media Source Extensions; Widevine decrypts inside the CDM), `protocol.json` and
  `protocol.js` (the shared protocol definition, see [ipc.md](ipc.md)). hls.js is vendored at build time.
- **`drm::DrmHost`** owns the process (RAII). It starts it lazily on the first message, sends `hello` and the
  network configuration first, watches a heartbeat, restarts a crashed or hung helper while a track is loaded
  (backoff 0.5 s, doubling; after 4 crashes within a minute it gives up until the next request), and closes it after
  the idle timeout (*Close the protected player after*, 5 minutes by default; a paused track resumes where it was).
- **`drm::DrmBackend`** implements `IPlaybackBackend`. Every load resolves a fresh protected stream through the API
  (`endpoints::protectedStream`: the `ctr-encrypted-hls` transcoding, whose playlist carries Widevine and PlayReady
  keys; the `cbc` one has FairPlay only). It mirrors the helper's state and position, maps its errors, and resumes
  the track at its position after a crash, a restart for new network settings, or an idle shutdown. There is no
  gapless hand-over or crossfade between helper tracks; near the end of a regular track the helper is warmed up
  if the next one is protected.
- **`drm::DrmService`** wires host and backend to the `playback.protected.*` settings and, live, to the network
  settings, and provides the status line in *Settings → Playback → Protected tracks*.

## What the helper is allowed to do

- No window (`show: false`, `--ozone-platform=headless`), no devtools, no navigation or new windows, no GPU unless
  *Protected player uses the GPU* is on (`--disable-gpu` and `app.disableHardwareAcceleration()`), no renderer
  background throttling (audio must not be suspended), autoplay without a gesture.
- The player page runs sandboxed with context isolation; its Content-Security-Policy and a `webRequest` allowlist
  admit only its own files, `blob:`/`data:` and `https://*.media-streaming.soundcloud.cloud` (media playlists and
  segments on `playback.`, licenses on `license.`). Only the `mediaKeySystem` permission is granted.
- **No persistent cache:** the player uses an in-memory session partition (`noctara-drm`, no `persist:`) with the
  HTTP cache disabled; the default session's disk cache is limited to one byte (`--disk-cache-size=1
  --media-cache-size=1`: Chromium reads 0 as "default size"). Chromium buffers media in memory only. Spell checking
  is off in every session from its creation and the dictionary URL points nowhere: otherwise Chromium fetches a
  dictionary for the system language from Google at start (seen with a Russian locale).
  Licenses are requested with `persistentState: 'not-allowed'` and `distinctiveIdentifier: 'not-allowed'`.
- userData is `<cache>/electron` (`--noctara-user-data`; a second instance without the cache lock uses a
  private temporary directory instead): Chromium's profile files and the Widevine CDM component
  (about 21 MB, downloaded by Chromium's component updater on first start). Checked after playing a protected
  track in the app: `Cache/` holds only its empty index, `blob_storage/` and `Dictionaries/` are empty. *Storage → Clear all* stops an idle helper and
  empties the folder except `WidevineCdm/` (which would only be downloaded again). Uninstalling: remove
  `~/.cache/noctara` as a whole.

## Network

The helper follows Noctara's network settings (`drm::planNetwork`, pure and unit-tested); details and the limits are
in [network.md](network.md#the-protected-player). Proxy, host mappings, DNS over HTTPS and TLS/HTTP switches are
given at start (Chromium's own first request, the CDM component check, happens before any message), and the
`configure` message repeats them for the player's session together with the proxy credentials (never on the
command line). Changing a startup switch restarts the helper; the track resumes.

## Processes and shutdown

The helper runs in its own session and process group (`CreateNewSession`), with descriptors other than the pipes
closed and signal handlers reset, and `PR_SET_PDEATHSIG=SIGKILL` so that it dies with Noctara even when Noctara is
killed. Closing stdin also ends it (`main.js` quits on end of input). On idle shutdown and on exit the host sends
`shutdown`, closes stdin, waits up to 2 s and then kills the whole group (`kill(-pgid, SIGKILL)`), which takes
Chromium's zygote, renderer and utility processes with it. A test starts a mock helper that forks a child and checks
that both are gone after the host is destroyed.

Electron's own sandbox (`chrome-sandbox`) needs unprivileged user namespaces, or the SUID helper installed root-owned
with mode 4755. Noctara never passes `--no-sandbox`; on systems without user namespaces install the SUID helper.

## Getting the helper

Noctara sets the helper up by itself (`drm::HelperInstaller`), so nothing has to be done after installing or
building it:

- **When:** 30 s after start when no helper is found, in the background (a metered connection defers it), or at once
  when a protected track needs one (that track is skipped with "being set up"; the next ones play). *Set up the
  protected player automatically* (`playback.protected.auto_install`, on) turns this off.
- **What:** castlabs Electron **v44.9.3+wvcus** for linux-x64 from `github.com/castlabs/electron-releases` (the
  `+wvcus` builds are the ones with Widevine), SHA-256 `80cb5218…359fad` (matches castlabs' `SHASUMS256.txt`), and
  hls.js **1.6.15** (Apache-2.0) from the npm registry via jsDelivr, SHA-256 `413a83e2…383938`. Both are pinned once,
  in `electron/helper-release.json`, which the installer embeds and `xmake drm-helper` reads.
- **How:** downloaded through Noctara's own network stack, so the proxy and DNS settings apply; an interrupted
  download resumes with a `Range` request (the bytes on disk are hashed again first); a file that does not match its
  SHA-256 is deleted and reported. Electron is unpacked with libarchive (entry names outside the folder are refused,
  nothing is written through symlinks, permissions are kept). The helper app (`electron/*.js`, `player.html`,
  `protocol.json`) is compiled into Noctara and written out by the installer.
- **Where:** `<data>/drm-helper/` (`~/.local/share/noctara/drm-helper/`): `electron-<version>/` (the runtime, marked
  complete by `.complete`) and `app-<hash>/` (the app, named by a hash of its files, so a newer Noctara with a
  changed helper app writes a new folder without downloading the runtime again). Older folders and the downloads are
  removed afterwards.
- **Progress:** *Settings → Playback → Protected tracks* shows the download percentage, unpacking, or why it failed
  (it is tried again on the next protected track); a toast says when it is ready.

Noctara uses, in this order: *Protected player folder* (`playback.protected.helper`) when set (an unusable folder is
reported, not replaced), `<binary dir>/drm-helper` (so `build/noctara` finds `build/drm-helper`),
`<prefix>/lib/noctara/drm-helper`, then the one it set up. A folder qualifies when it has `electron/electron`
(executable) and `app/main.js`.

For development, `xmake drm-helper` assembles the same thing in `build/drm-helper` from the working tree's
`electron/` (downloads cached in `build/downloads`, re-verified every time).

### Packaging, signing and VMP

- Packages ship `drm-helper/` next to the binary or under `lib/noctara/`, with Electron's license files from the
  zip. `chrome-sandbox` must be installed root-owned with mode 4755 where user namespaces are unavailable.
- castlabs Electron fetches the Widevine CDM through Chromium's component updater on first start; nothing is
  bundled. Widevine on Linux has no VMP (Verified Media Path) requirement, so the build does not need castlabs'
  EVS signing there. On Windows and macOS, licence servers may demand VMP: the packaged app must be signed with
  castlabs EVS (`python3 -m castlabs_evs.vmp sign-pkg <dir>`) after code signing, and an unsigned helper gets
  lower security levels or refusals. Those platforms are not built yet.
- Updating: pick a newer `+wvcus` release, update version, URL, size and checksum in `electron/helper-release.json`,
  run the real check below. Installed copies switch to it on their next setup.

## Verification

- Unit and integration tests (`-ts="drm.*"`, `media.controller`): protocol validation, the network plan, the host
  against `noctara-mock-drm-helper` (handshake, heartbeat, hang detection, crash restarts and giving up, idle
  shutdown, restart for new switches, no orphans, missing helper, no Widevine, version mismatch), the backend
  (play, pause, seek, end, resume after a crash with a fresh stream, refused license, failed resolution, missing
  helper) and the controller routing protected tracks (never prefetched, warmed up, turned off).
- `noctara-tests --no-skip -tc="real drm*"` starts the real helper from `build/drm-helper` and waits for Widevine.
- Manually (2026-10-29): driving `build/drm-helper` directly over the protocol with a real `ctr-encrypted-hls`
  stream played audibly, reported the duration (323 788 ms), positions every 250 ms, and seeked.
- In the app (2026-10-31, release build, signed in): `build/noctara --play <protected link>` started the helper
  (ready 2.6 s after the start on a fresh profile, CDM download included), and PulseAudio showed an active,
  uncorked stream from `noctara-drm-helper` while it played. After exit no helper process remained, and the
  profile held no media, cache entries or dictionaries. Note that `build/noctara` (not the binary under
  `build/linux/…`) is the one that finds `build/drm-helper` next to it.
