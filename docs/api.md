# SoundCloud API client

Noctara talks to **api-v2**, the web API that soundcloud.com itself uses. It authorizes with the user's own session
(the OAuth token, slice f) and the web player's public `client_id`. Everything lives in `src/api/` (requests,
parsing, discovery) and `src/models/` (list models for QML).

## Why separate modules

The original plan put the API client under `net/`. It is a module of its own (`api`, depending on `core`,
`settings`, `net` and `cache`) instead, for two reasons. `net` stays protocol-only (DNS, proxies, TLS, transport) and
knows nothing about SoundCloud. The API maps tracks to the audio cache's self-describing `cache::TrackInfo`, so it has
to sit above `cache`. `models` sits on top of `api` and is the only part that knows about Qt's item models.

```
 QML ── Catalog (ui::CatalogController) ── models::CollectionModel ──┐ fetch / lookupTracks
                                                                    ▼
 GUI thread          api::ApiService (owns the thread, follows soundcloud.* settings)
 ─────────────────────────────────────────────────────────────────────────────────────────
 noctara-api thread  api::ApiClient ── dedup, pause on 429, 401 handling, lookup coalescing
                       │      └── ClientIdProvider ── override │ stored │ discovered from soundcloud.com
                       │ parse (simdjson → structs) on this thread
                       ▼
 cache thread        cache::CachingTransport (RFC 9111 HTTP cache: ETag, TTL, stale-if-offline)
 net thread          net::CurlTransport (DNS/DoH, proxy, TLS, HTTP/2, limits, retries)
```

The API thread does the JSON parsing, so the GUI thread only ever receives finished structs. Every request goes
through the caching transport and, behind it, the one network transport. DNS, proxy, TLS, timeouts, connection
limits and transport-level retries therefore come from the Network settings. There is no ad-hoc HTTP code in the
API layer.

## client_id

api-v2 rejects requests without the `client_id` of SoundCloud's own web player. The id is public (it is in the
site's JavaScript) but it rotates. All knowledge of how soundcloud.com is built lives in `api/ClientId.{hpp,cpp}`, so a
site change needs a fix in one file only.

Order of preference:

1. **Override.** `soundcloud.client_id` (Advanced → SoundCloud) or `--set soundcloud.client_id=…`.
2. **Stored.** The last id found, kept in `$XDG_STATE_HOME/noctara/soundcloud.json`. It is not a secret.
3. **Discovered.** `GET https://soundcloud.com/` checks the hydration data first
   (`{"hydratable":"apiClient","data":{"id":"…"}}`). Then come the page's scripts on `*.sndcdn.com`, last first and at
   most 8, searched for `client_id:"…"`, `client_id="…"` or `client_id=…`. Ids are 24–64 letters and digits.

Discovery is single-flight: requests that need an id wait for the one discovery in progress. After the site
answered but held no id, discovery is throttled for 30 s and requests fail at once with `ClientIdUnavailable`.
Network errors are not throttled, because they are not the site's fault. A 401 on an anonymous request marks the
id as rejected; it is never used again. A new one is discovered and the request repeated, up to 3 attempts in all.
A rejected override cannot be fixed automatically, and the error message points the user to the setting.

## Requests

`IApiClient::send(ApiRequest, context, Decoder)` is thread-safe. The decoder runs on the API thread and returns the
closure that runs on `context`'s thread. `fetch(client, endpoint, context, done)` wraps this for typed endpoints
(`Endpoint<T>` = request + parser), so callers simply receive a `Result<T>`.

| Behaviour | Rule |
|---|---|
| Credentials | `Authorization: OAuth <token>` and `client_id` are sent **only** to `https://api-v2.soundcloud.com` and `https://api.soundcloud.com` (port 443). Media hosts (waveforms) get neither, and other hosts are refused with `InvalidArgument`. A `next_href` that points elsewhere is dropped. |
| Deduplication | Identical GETs in flight (same URL, same account, same cache mode) share one transfer, and each caller gets its own parsed copy. |
| Coalescing | `lookupTracks(ids)` calls arriving within 10 ms are merged into `/tracks?ids=` requests of up to 50 distinct ids. Ids SoundCloud no longer has are left out. Playlists list their later tracks as stubs, so this is how they are filled in. |
| 429 | Every request pauses until `Retry-After` has passed (10 s without the header). The request is repeated if the pause is ≤ 20 s; longer pauses fail with `RateLimited`, as do requests arriving during them. The transport has already retried with exponential backoff before this. |
| 401 | With a token: the session reports `rejected()` once per token (the UI asks to sign in again). Public endpoints are repeated anonymously; signed-in ones fail with `Unauthorized`. With an `ApiRequest::candidateToken` (sign-in checks a token before keeping it, `endpoints::verifyToken`): only `Unauthorized`, the session is not reported and the request is not repeated. Without a token: the client_id expired (see above). |
| 403 / 404 / 410 | `Forbidden` (private or geo-blocked) and `NotFound`. |
| Signed-in endpoints | Fail with `SignInRequired` before any request when nobody is signed in. |
| Cancellation | `RequestHandle::cancel()` delivers `Cancelled`; destroying the context drops the answer. A request whose callers all gave up before it was sent is not sent. |
| HTTP cache | Freshness when SoundCloud sends no `Cache-Control`: 30 s for the feed, likes, history and `/me`; 5 min for search, user listings and charts; 1 h for a track, user or playlist; 30 days for waveforms. Stale answers are revalidated with ETags and served while offline (`Payload::stale`). Stream URLs bypass the cache: their signatures expire. |

## Endpoints

| Function | Path | Auth |
|---|---|---|
| `resolve(permalink)` | `/resolve?url=` | optional |
| `track(id)`, `tracks(ids)` | `/tracks/{id}`, `/tracks?ids=` | optional |
| `user(id)`, `me()` | `/users/{id}`, `/me` | optional, required |
| `playlist(id)` | `/playlists/{id}` | optional |
| `search(q, kind)` | `/search`, `/search/tracks`, `/search/users`, `/search/playlists_without_albums`, `/search/albums` | optional |
| `userTracks`, `userPlaylists`, `userLikes` | `/users/{id}/tracks`, `/playlists`, `/track_likes` | optional |
| `related(id)` | `/tracks/{id}/related` | optional |
| `stream()` | `/stream` (the feed) | required |
| `history()` | `/me/play-history/tracks` | required |
| `trending(genre)` | `/system-playlists/soundcloud:system-playlists:trending-by-genre:<genre>` | optional |
| `followings(user)` | `/users/{id}/followings` | optional |
| `library()` | `/me/library/all` | required |
| `waveform(url)` | the track's `waveform_url` (wave.sndcdn.com) | none |
| `streamUrl(transcoding, track_authorization)` | the transcoding's URL → `{"url": signed media URL}` | optional |

Lists ask for `limit=<library.loading.page_size>&linked_partitioning=1` and follow `next_href`.

## Parsing

`api/Parse.cpp` is the only file that includes simdjson. It uses the DOM parser, one per thread, and fills compact
structs (`Track`, `Playlist`, `User`, `Entry`, `Page<T>`, `Waveform`) directly; no JSON tree survives the call.
A body with `SIMDJSON_PADDING` bytes of spare capacity is parsed in place; anything else is copied once. Collections
unwrap likes (`{"kind":"like","track":…}`), history (`played_at` in ms), feed reposts (`type: "track-repost"`,
`user` = reposter) and charts (`{"track":…, "score":…}`). Objects of unknown kinds are skipped instead of failing
the page. System playlists (string URNs) are skipped too. A missing id or malformed JSON is `InvalidResponse`.
Counts the API hides are `-1`. URLs stay text (`QString`), because building a `QUrl` costs more than parsing the JSON
around it; requests build `QUrl`s when they need them.

## Streams and DRM

`pickTranscoding(track, Best | DataSaver)` never returns an encrypted transcoding (`*-encrypted-hls`: those play in
the DRM helper; `pickProtectedTranscoding` picks those, `ctr-encrypted-hls` only, see [drm-helper.md](drm-helper.md)). It returns a preview (`snipped`) only when nothing whole is available to the account. It
ranks by estimated bitrate (`aac_160k` → 160, mp3 → 128, opus → 64), and progressive breaks ties because it is
simpler to cache by range. `Track::drm()` is true when the whole track exists only encrypted. Lists mark such
tracks "Not cacheable", and `toTrackInfo()` sets `drm`, which makes the audio cache refuse them. `toTrackInfo()`
builds the cache's sidecar record: the key is the track id plus `protocol/preset/quality`, never a URL.

## Models

`models::CollectionModel` (QML type `CollectionModel`) holds `Entry` rows of tracks, playlists and users. Views call
`fetchMore()` near the end, which follows `next_href`. Items repeated on a later page are dropped (keyed by kind and
id), and an error stops paging until `reload()`. Reloading keeps the old rows until the first page arrives. Stub
tracks are hydrated when a view first asks for their data: the rows asked for in one event-loop turn become one
coalesced lookup, and tracks SoundCloud no longer has are removed. Roles: `kind`, `itemId`, `title`, `subtitle`,
`artwork` (300 px), `thumbnail` (67 px), `duration`, `plays`, `likes`, `reposts`, `followers`, `trackCount`,
`genre`, `permalink`, `created`, `at`, `repostedBy`, `protectedMedia`, `preview`, `verified`, `album`, `pending`.

A synced model (`setSyncedSource`: likes, the library) loads every page by itself, shows its saved snapshot
meanwhile and applies the fresh list row by row (see [caching.md](caching.md#saved-lists-likes-library-playlists)).
Playlists that a list carries without artwork or tracks (`/me/library/all`) get their first track's cover: the rows a
view shows fetch `/playlists/{id}` (cached, four at a time, each once), and `Playlist::artwork()` never falls back
to the owner's avatar.

`Catalog` (`ui::CatalogController`) owns the search model. Typing searches after a 350 ms pause, and Enter or a
change of kind searches at once.

## Verification status

The endpoint shapes, `client_id` patterns and transcoding protocols above are taken from the site's behaviour as
known in 2025–2026. The automated tests use recorded-style fixtures and a scripted transport (`tests/mock/FakeApi.hpp`),
not the live service. The live check is a test that is skipped by default and needs internet access:

```sh
./build/linux/x86_64/debug/noctara-tests --no-skip -tc="real api*"
```

It discovers the client_id, searches, resolves a permalink and resolves a stream URL. If SoundCloud changes the site,
start in `api/ClientId.cpp` (discovery) and `api/Parse.cpp` (field names). Known unknowns:

- `/charts` was retired by SoundCloud (404 as of October 2026); the Charts page now uses the "trending by genre"
  system playlists the web player shows. The endpoint stays isolated, and the UI will hide charts if it
  fails (slice h).
- Whether api-v2 sends `Cache-Control` on listings is not known; the TTLs above apply when it does not.
- Write endpoints (like, follow, repost) are not part of this slice; they arrive with the pages in slice (h).
