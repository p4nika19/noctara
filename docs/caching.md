# Caching

Noctara keeps three caches: audio of regular tracks, images, and API responses. All of them are disposable: deleting any file under the cache directory, or all of it, never breaks the app; whatever is missing is fetched again. Everything lives in the `cache` module (`src/cache/`), which depends on `core`, `settings` and `net`. The Storage & Cache settings page shows the usage and offers the Clear actions.

## Where the caches live

`QStandardPaths::CacheLocation`, i.e. `~/.cache/noctara/` on Linux (`$XDG_CACHE_HOME` respected). A custom location (`cache.dir`) gets a `noctara` folder inside it, so Clear and the version check never touch anything else in the chosen folder. A location that is not writable falls back to the standard one, and the page says so.

```
<cache>/
  layout-version     # "1"; another number wipes audio/, images/, http/ (never other files)
  CACHEDIR.TAG       # backup tools skip the directory
  lock               # flock: one Noctara instance owns the caches
  audio/
    index            # advisory: size, last access, completeness, covers per entry (rebuilt if lost)
    covers/<sha256>  # cover art, stored once per unique image
    <aa>/<name>/     # one entry; <name> = XXH3-128 of (track id, transcoding), never the URL
      meta.json      # the sidecar, written before the first audio byte
      map            # which pieces are stored, with an XXH3 checksum each
      data.part      # progressive stream, partial (sparse file, pieces at their offsets)
      data           # progressive stream, complete (renamed from data.part, then tagged)
      playlist.m3u8  # HLS: normalized playlist (local piece names, no signed URLs)
      seg/00000…     # HLS: init segment (if any) and media segments
  images/<aa>/<hash> # encoded images (DiskLru), index alongside
  http/<aa>/<hash>   # stored API responses (DiskLru), index alongside
  electron/          # DRM helper profile (slice i): non-media data only
```

All directories are created with mode 0700 and files with 0600. A second instance cannot take the lock. It then runs with the RAM tiers only and the page explains why. The GPU probe and the pipeline cache from `gfx` also live in the cache root, and they are kept when the cache layout changes.

## Audio: two tiers

| | Tier 1: RAM (`MemoryTier`) | Tier 2: disk (`AudioCache`) |
|---|---|---|
| Unit | encoded pieces: 64 KiB chunks of a progressive stream, or whole HLS segments | the same pieces in the entry directory |
| Budget | `cache.ram_mb` (128 MB); pieces of the current and prefetched tracks are evicted last | `cache.disk_mb` (2 GB), 0 = off; free-space floor `cache.min_free_mb` (1 GB) |
| Eviction | LRU, inactive entries first; trimmed to a quarter on kernel memory pressure (PSI trigger, event-driven) | LRU by last access; open entries are never evicted |
| Sharing | pieces are implicitly shared `QByteArray`s, so a hit hands out the buffer without copying | `pread` per piece, verified against its checksum, then promoted to RAM |

Decoded PCM is never cached. Pieces are 64 KiB: small enough that the first audio arrives after one piece, and large enough that a 10 MB track needs only 160 map entries.

### Entries

- **Key.** An entry is keyed by the track id and the transcoding preset, never by the signed stream URL. A `UrlProvider` supplies the URL when bytes are needed and is asked again (`refresh = true`) when the server answers 401/403/404/410, because signed URLs expire. A partial entry resumes from whatever URL is current.
- **Sidecar first.** When an entry is created, `meta.json` is written atomically before any audio byte. It holds the track id, title, artist, uploader, album (only a real one), playlist context, genre, duration, dates, permalink, artwork and waveform URLs, codec, bitrate and quality, content length, the source's `updated_at` and media ETag, cached-at, completeness, cover hashes and the payload hash. Any entry on disk, partial or complete, resolves to correct metadata offline (`AudioCache::cachedTracks()`, `coverFor(url)`). A directory without a valid sidecar is not ours and is deleted.
- **Ranges.** Progressive streams are fetched with `Range` requests for exactly the missing run of pieces, and each run is resumed after a dropped connection. A server that ignores ranges is handled by skipping bytes. HLS entries fetch the playlist first, then segment by segment.
- **Single flight.** One fetch per entry serves every reader. A reader within 16 pieces ahead of the running fetch waits for it. A reader that seeks farther away restarts the fetch at its position, and the gap is filled later. While any playback reader is open the whole entry is downloaded, so a played track ends up complete on disk.
- **Validation.** Every piece carries an XXH3-64 checksum. Pieces that fail it on read, or that a truncated file cannot hold, are dropped and fetched again. A damaged `map` makes the entry start over. A different content length or media ETag from the server (or a new ETag from the API at open) means the media changed: the entry is reset. Readers of the old data get `ContentChanged` and reopen.
- **Finalization.** When all pieces are present they are verified once more. Then `data.part` is renamed to `data` (atomic, same inode, so open readers continue) and the sidecar records completeness and the payload hash.
- **Generations.** Readers are bound to the generation of the file they opened. Tagging rewrites the file (the tags change its length), so it creates a new generation. Readers opened earlier keep reading the old file through their open descriptor, and RAM pieces are keyed by generation, so the bytes under a running decoder never shift.
- **Full disk.** When `ENOSPC` or the free-space floor is hit, the entry first evicts its own least-recently-used entries and retries once. If writing still fails, data stays in RAM only: such pieces are never recorded as stored, playback is unaffected, and background downloads stop until there is room again. Nothing crashes and nothing blocks.
- **Index.** The index loads on the cache thread 0.5 s after start, so large caches never stall startup. When it is missing it is rebuilt from the sidecars. It is advisory: entries opened before it loads still work, and only eviction waits for it.

### Covers

With an entry, the large cover and the thumbnail are fetched at once. Each is stored once per unique image, named by the SHA-256 of its content, and linked from the sidecar. A failed request, or a response that is not a decodable image, stores nothing, so a placeholder is never persisted as the real cover. The entry is created regardless, and the cover is retried the next time the entry is opened. Covers no longer referenced by any entry are deleted with their last entry. Offline, the image cache asks the audio cache for a stored cover by its URL.

### Tags

With `cache.embed_tags` on (the default), a finished progressive entry is tagged with TagLib on a thread-pool worker. MP3 gets ID3v2.4 with UTF-8 frames (other tags stripped), M4A gets MP4 atoms, and Opus/Vorbis get Vorbis comments with a FLAC picture block. The fields are title, artist, album (only if real), genre, date, permalink (`WOAF`, or `WEBSITE`, or an iTunes freeform atom on MP4) and the front cover. The cover is embedded exactly as received: the large one if neither side exceeds 1200 px, else the thumbnail. Text is UTF-8 throughout with control characters stripped. Cyrillic, CJK and emoji with joiners round-trip (tested).

Tagging works on `data.tagging`, a copy of the file. It then checks that the audio payload hash is unchanged (and equals the hash recorded at finalization) and that duration, sample rate and channels match, and only then renames the copy over `data`. The payload hash covers the MPEG frames between ID3v2 and ID3v1/APE, the contents of every MP4 `mdat` box, or the Ogg audio packets: page sequence numbers and CRCs may change, packets may not. A crash or `ENOSPC` at any point leaves the previous `data` intact, and a leftover `.tagging` file is removed when the entry is next opened. A changed `updated_at` (or cover) refreshes the sidecar and re-tags lazily, the next time the entry is opened. Partial entries and HLS entries are never tagged; HLS keeps the sidecar only, because tagging it would mean remuxing.

This is an integrity and metadata feature of an internal cache. The layout stays opaque (hashed names), and there is no export or "save as".

### Prefetch

`Prefetcher::setQueue` warms the next queue items as background requests (skipped on metered connections) and drops the previous prefetch when the queue changes. The count is `cache.prefetch` (2): none on a metered connection, at most one while the disk cache is off (then only the first 1 MiB, in RAM). Playback (slice g) feeds it the queue.

## DRM-protected tracks

They are never cached, in any tier. `AudioCache::open` refuses `TrackInfo::drm` with `ErrorCode::NotCacheable` before anything is created: no directory, no sidecar, no URL request, no tags. An HLS playlist with `EXT-X-KEY` other than `NONE` is treated the same way, even when the track was not flagged; the entry made for it is deleted. The prefetcher skips such tracks. The Electron helper (slice i) uses no persistent media or HTTP cache; its profile under `electron/` holds non-media data only and is removed by Clear all. Tests cover all of this, including at the integration level over the real network stack.

## Images

`ImageCache` keeps decoded `QImage`s in a RAM LRU (`cache.images.ram_mb`, 64 MB) keyed by URL and decode size, so a thumbnail and the full-screen cover are separate entries. The encoded bytes go into a disk LRU (`cache.images.disk_mb`, 512 MB). Concurrent requests for one URL share one download, and decoding runs on the thread pool: the image is scaled to fit the requested box, never enlarged. QML uses `image://cover/<percent-encoded URL>` (an async image provider).

## API responses (HTTP cache)

`CachingTransport` wraps the network transport and is what the API client uses (`CacheService::transport()`). It follows RFC 9111 for `GET`:

- Responses with `no-store` are never stored, and a stored copy is removed when one arrives. Range requests, streamed bodies and `CacheMode::Bypass` pass straight through. Bodies over 4 MiB are not stored.
- **Freshness:** `max-age`, else `Expires − Date`, else the request's `cacheTtl`: short for volatile endpoints (feed, likes), long for immutable ones (track metadata, waveforms). `no-cache` always revalidates.
- **Stale entries** are revalidated with `If-None-Match` / `If-Modified-Since`. A 304 refreshes the stored headers and serves the stored body.
- When the network fails (offline mode, outage, 5xx), a stale copy is served with `Response::stale` set. `CacheMode::OnlyIfCached` never touches the network.
- Keys include a digest of the `Authorization` header, so one account never sees another's data. `Vary` is honoured, and a successful `POST`/`PUT`/`DELETE` invalidates the URL.
- The disk budget is `cache.metadata.disk_mb` (128 MB). There is no RAM tier: lookups run on the cache thread anyway, and the page cache keeps hot files in memory.

## Threading

`CacheService` owns a `noctara-cache` thread on which the audio cache, the image cache's disk side and the HTTP cache live. It applies the `cache.*` settings live: budgets, the enable switch and tagging. The location switches live too, because the old layout is closed and the new one opened. All cache I/O runs on that thread, or on the reader's thread for `pread`. Tagging and image decoding run on the thread pool. The GUI thread never does any of it. `AudioReader::read` blocks (the decoder's thread), while `tryRead` never waits. `open()`, `request()` and `usage()` deliver their results through `CallbackChannel` to the caller's thread. Handles that may outlive the cache post through a `Mailbox`, which drops posts once the cache is gone.

## Limits and open items

- Decoded-output checks after tagging use TagLib's audio properties (duration, rate, channels); a full decode comparison with FFmpeg arrives with the native backend (slice g).
- SoundCloud's current transcodings (progressive MP3, HLS MP3/AAC/Opus, encrypted HLS for DRM) are verified in slice (e); the cache handles all of them generically.
- The HTTP cache has no RAM tier; add one if profiling of the API client (slice e) shows a need.

## Measurements

See [performance.md](performance.md#slice-d-caches).
