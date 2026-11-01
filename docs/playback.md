# Playback

Slice (g): regular SoundCloud tracks play natively, every byte coming through the audio cache and the network
transport. The module is `media` (depends on `core`, `settings`, `cache`, `api`, `models`); `ui` adds the waveform
item, the tray icon and notifications; QML shows the now-playing bar and the queue drawer.

## Pipeline

```
 PlaybackController (QML "Player", GUI thread)
   queue · shuffle/repeat · autoplay · stream choice · waveform · prefetch · session state
        │ load / setNext / seek / gain
        ▼
 IPlaybackBackend ── NativeBackend (GUI thread) ─────────────── drm::DrmBackend (protected tracks, Electron helper)
        │ one Voice per source (current + next)
        ▼
 Voice thread:  AudioCache::open → AudioReader ─► StreamSource ─► Decoder (FFmpeg) ─► PcmRing
                (RAM → disk → network)            (custom AVIOContext; never a URL)   (float, device rate)
                                                                                         │
 audio thread:  QAudioSink pull callback ◄── Mixer (current voice, next voice, gains) ◄──┘
```

- **StreamSource** reads one cache entry. Progressive entries are a random-access file (the demuxer seeks through
  `AVSEEK_SIZE`/`SEEK_*`). HLS entries are read as one stream: the init segment (`EXT-X-MAP`), if any, then the media
  segments. MP3, ADTS, Ogg and fragmented-MP4 segments concatenate into a valid stream, so FFmpeg's HLS demuxer
  (which would open URLs itself) is never used.
- **Decoder** wraps FFmpeg with RAII (format, codec, resampler, packet, frame, AVIO buffer). Probing is limited to
  256 KiB / 0.5 s so the first audio is quick. Output is interleaved float at the device's rate and channel count
  (at most stereo), resampled by libswresample.
- **Seeking** restarts decoding: progressive entries seek in the demuxer, HLS entries open a new source at the
  segment that holds the target. In both cases audio before the target is decoded and dropped, so the position is
  exact. While the new decoder starts, the ring is on hold and the mixer outputs silence (the backend reports
  "buffering"), then fades in over 15 ms.
- **PcmRing** sits between a voice's thread and the audio callback. The producer waits while it is full; the
  consumer only takes a short lock. The ring knows the stream frame of what it hands out, which is the played
  position (minus the device latency).
- **Mixer** runs in the audio callback. When the current voice runs out, the armed next voice continues in the
  same callback: gapless. With a crossfade set, the mixer stops reading the current voice where the fade must begin
  and blends both with equal-power gains over the remaining frames. Volume, pause and resume are ramped (15 ms) so
  nothing clicks; while paused and silent nothing is consumed and the backend suspends the device.
- **NativeBackend** opens sources in the cache, owns the voices, and checks the mixer every 100 ms while active
  (position, buffering, a gapless advance, the end). The device is opened on the first track, suspended while paused
  or ended, and follows the system's default output; a device with another sample rate makes the voices decode
  again from the current position.

## What plays

- `api::pickTranscoding` chooses the stream: never an encrypted one, never an adaptive master playlist (`abr_*`),
  a preview only when nothing else is available. *Audio quality* (Playback) picks the highest bitrate, or the
  smallest one with *Data saver* or with *Auto* on a metered connection.
- As of October 2026 SoundCloud serves many tracks as HLS only (`aac_160k`, `aac_96k`, `abr_sq`): fragmented MP4 with an
  init segment. That is the path real playback takes most of the time.
- Stream URLs are resolved only when the cache asks for one (first byte, or a refused, expired signature), through
  the API client with the track's authorization. The cache entry is keyed by track and transcoding, never by the
  signed URL.
- **Protected (DRM) tracks** play in `drm::DrmBackend`, the castlabs Electron helper ([drm-helper.md](drm-helper.md)).
  The controller picks the backend per track and stops the other one when it switches; only the current backend's
  signals count. A protected item that follows a regular one is not prepared gaplessly: the regular backend ends,
  and the helper (warmed up when the next item came near) starts it. Protected tracks are never prefetched, and the
  cache refuses them anyway (`NotCacheable`; a backend test checks that no entry appears). With *Play protected
  tracks* off, or without a protected backend, they are reported and skipped.

## Queue

`PlaybackQueue` (pure, unit-tested) keeps items in playing order. Shuffling reorders only what comes after the
current item and remembers the original order to return to; *play next* lands after the current item in both
orders. The shuffle is a plain uniform Fisher–Yates shuffle (`std::shuffle`), with no weighting by artist or
similarity, and it draws fresh randomness from `QRandomGenerator::global()` every time: when shuffle is turned on
and when a new list starts while it is on. That generator is seeded from the operating system's entropy source
(`getrandom` on Linux); nothing about the order is stored or repeatable between shuffles or sessions. *Repeat one* repeats on its own but a skip moves on; *Previous* restarts the track after 3 s.

The controller prepares the following item (opens it and decodes ahead) only in the last 30 s plus the crossfade,
so queue edits and stops do not download tracks that never play. Further items are prefetched into the cache by
`cache::Prefetcher` (`cache.prefetch`, none on a metered connection). Playlist tracks that are only ids are fetched
through coalesced `/tracks?ids=` lookups before they play. When the queue ends, *Autoplay related tracks* appends
tracks related to the last one.

A queue started from a list (`playCollection`, *Play*, *Shuffle*) **follows that list**: tracks it gains later (a
synced list such as Likes still loading, or found new by a sync) join the queue, appended in order or, while
shuffling, each at a uniformly random place among the upcoming items (the same distribution as shuffling the rest
again together with them); tracks the list loses leave the queue, except the one playing. *Shuffle* on Likes
therefore covers every like, even before all of them have loaded. Starting another queue or clearing the upcoming
items stops following, and so does the list being repointed to other contents (`CollectionModel::sourceReset`: a
new search, another chart genre, a paged list refreshed from page one, another account). Tracks from a saved snapshot are looked up again right before they play.

With *Restore the queue* (on by default) the queue (up to 500 track ids around the current one), the current item,
its position and the context are saved too: on track changes, queue edits, pause and quit. At the next start the
tracks are looked up again by id and the queue comes back paused at that position; nothing is fetched or played
until Play. A shuffled queue comes back in its shuffled order.

Volume, mute, shuffle and repeat live in `<state>/session.json` (written debounced, off the GUI thread). The volume
slider maps to gain with a cubic curve, like the system's own slider.

## Desktop integration

- **MPRIS 2** (`org.mpris.MediaPlayer2.noctara`, `.instance<pid>` for a second instance): play/pause/next/previous/
  stop/seek/set position, volume, shuffle, loop status, metadata and `Seeked`. Media keys reach Noctara this way on
  GNOME, KDE and most other desktops. Covers are handed over as local files written from the image cache, so the
  desktop never fetches artwork itself (that would bypass Noctara's DNS and proxy settings). Setting *Media keys and
  system controls*, live.
- **Tray icon** (StatusNotifierItem through Qt, only where the desktop has a tray): play/pause, previous, next, show,
  quit; middle click toggles playback. Setting *Tray icon*, live.
- **Notifications** (`org.freedesktop.Notifications`), off by default: one per new track while the window is not in
  front, replacing the previous one.
- In-window keys: Space (play/pause), Ctrl+←/→ (previous/next), Shift+←/→ (seek 5 s), Ctrl+↑/↓ (volume), Ctrl+M
  (mute), Ctrl+Shift+Q (queue), and the media keys while the window has focus. Remapping arrives with slice (h).

## Full-screen player and context menus

Clicking the bar's cover (or the *Full-screen player* shortcut) opens the current track large: the cover flies from
the bar into place (a shared-element transition driven by one progress value), over the track's own pre-blurred
artwork; title, artist (a link to their page), the waveform or a plain progress line, transport, volume, and the
queue beside it when the window is wide enough (*Layout → Full-screen player*). F11 or the corner button also puts
the window into full screen; Escape leaves both.

Right-click (or the menu key) opens one shared glass menu with what fits the element: tracks (play, play next, add
to queue, go to artist, open on SoundCloud, copy link), playlists (open, play, queue, go to owner, links), people
(open profile, links), queue items (play, play next, remove, go to artist, links, clear upcoming), the player bar
and the full-screen cover (play/pause, next, previous, shuffle, repeat, full-screen player, queue, go to artist,
links, link at the current time), and the waveform (play from here, copy a link that starts there). Links open only
`http(s)` URLs.

## Waveform

`WaveformItem` draws SoundCloud's waveform as bars. With hardware rendering the whole waveform is one
`QSGGeometryNode` with a custom material (`waveform.vert/.frag`): played, stored, unplayed and hovered bars are
coloured from uniforms, so playing and hovering never rebuild the geometry. Under the software renderer it is two
cached images (played and not yet played) shown side by side; playback only moves the split. Both are rebuilt only
when the size, samples, stored ranges or colours change. The stored parts of the track come from the cache
(`AudioReader::cachedFractions`: bytes for progressive entries, segment time for HLS).

## Errors

Playback errors are `ErrorCode`s (`NotPlayable`, `DecodeFailed`, `AudioOutputFailed`, `ProtectedPlayback`, plus
the network and cache codes) shown as translated toasts. A network failure keeps the track; pressing play retries
from where it stopped. Undecodable or unavailable tracks are skipped, never in circles through a queue where
nothing plays.

## Not done (yet)

- Loudness normalization: SoundCloud publishes no loudness data for its streams, and measuring it would mean
  decoding a track before it plays. Left out rather than faked.
- Output device selection: Noctara follows the system's default output.
- (Slice h added queue restore, the full-screen player and remappable shortcuts; see above and
  [architecture.md](architecture.md).)
