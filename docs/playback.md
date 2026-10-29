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
 IPlaybackBackend ── NativeBackend (GUI thread) ─────────────── DrmBackend (slice i, Electron helper)
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
- **Protected (DRM) tracks** belong to the DRM backend (slice i). Until it exists they are reported ("needs the
  protected player") and skipped. The cache refuses them anyway (`NotCacheable`), and the backend test checks that
  no entry appears.

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
- Restoring the last queue at startup, the full-screen player and remappable shortcuts belong to slice (h).
