# Authentication and the session

Noctara signs in with the user's own SoundCloud session, held as an OAuth token. The token authorises the api-v2
client (see [api.md](api.md)); it is only ever sent to SoundCloud's API hosts over HTTPS, and it is never written to
the configuration file, logs or diagnostics. A pasted token passes through the QML text field once and is cleared
from it right after submission. The `auth` module depends only on `core` plus sqlite3, OpenSSL, libsecret and
QtDBus (KWallet). `ui::AccountController` (the QML `Account` singleton) connects browser import, manual sign-in,
secret storage, and the API session.

## Where the token lives

| Place | Lifetime | Notes |
|---|---|---|
| `api::Session` | process memory | the in-memory holder; the API client reads it per request |
| `auth::SecretServiceStore` | persistent | the desktop Secret Service through libsecret (behind `auth::IKeyring`) |
| `auth::EncryptedFileSecretStore` | persistent | fallback when the keyring is unavailable |
| `auth::MemorySecretStore` | process memory | last resort if neither persistent backend can be used |

The token is stored under `soundcloud.oauth_token`, labelled "Noctara SoundCloud session" in the keyring manager.
Proxy credentials use the same store (see [network.md](network.md)). All keyring and file operations run off the UI
thread, and `persistent()` / `keyringBacked()` let the Accounts page and the proxy credentials row describe the
active protection: keyring, encrypted local store (with a warning), or memory only.

`SecretServiceStore` keeps its tiers consistent:

- **read** asks the keyring; on a miss *or* a keyring error it asks the encrypted file, then memory. A secret saved
  while the keyring was unavailable is therefore still found in a later session once the keyring is back.
- **write** goes to the first tier that accepts it, then removes copies from the lower tiers so they cannot shadow
  the new value later. A secret found only in the encrypted file moves to the keyring with its next write.
- **remove** clears every tier and always asks the keyring, even after a keyring error in this session. Per-key
  epochs make sure a slow keyring write that fails *after* a remove (sign-out) does not land in a fallback.
- After a keyring error, reads and writes skip the keyring for the rest of the session, so a dismissed unlock prompt
  is not shown again for every request. One unreadable `.secret` file is logged and ignored; it does not disable the
  file tier.

The encrypted fallback lives under the platform state directory (`~/.local/state/noctara/secrets/` on Linux), not in
the config or cache. Each secret has its own versioned AES-256-GCM file (`NOCTSEC1`, 12-byte random nonce, 16-byte
tag) with the key name as authenticated data, so a file copied under another key's name does not decrypt. Files are
replaced atomically; directories are `0700` and files `0600`. A random 256-bit master key is stored in `master.key`
with owner-only permissions; it is created with `createFileAtomic` (`link()`, never replacing), so two instances
starting at once agree on one key. This protects data at rest, but it is **not equivalent to a desktop keyring**
against an attacker who can read the user's whole account. The UI displays this warning. If the encrypted store also
fails, memory-only storage is used and the UI says the session will be forgotten at exit.

## One verified sign-in path

```
pasted token ─────────────► AccountController::signInWithToken ── visible ASCII only (no CR/LF)
                                 │
consented browser import ───────►│ verifyCandidate
                                 ▼
                    GET /me with ApiRequest::candidateToken (cache bypassed)
                    api::Session is NOT touched while this runs
                               │             │
                              200           401/403/other
                               │             └► state returns to what it was (Expired stays Expired), error toast
                               ▼
        api::Session::setToken, applyUser, state = SignedIn, ISecretStore::write
```

Every token is **verified against `/me` before it is kept**. The candidate is sent only with its own request, so no
other request runs with it, and its `401` is not reported as a rejection of the working session. A bad import or
paste never disturbs a working session. On success the account username, display name, avatar, and profile URL are
shown.

Every sign-in, sign-out and expiry starts a new *generation* in `AccountController`; answers that belong to an older
one are dropped. This covers a slow keyring read at startup that answers after the user already signed in, a check
superseded by a newer one, and a save that finishes after sign-out.

At startup `AccountController` loads the stored token, sets it on the session immediately, and re-checks it in the
background with the normal `/me`. A `401` expires the session (state `Expired`, stored token removed, a toast with a
**Sign in** action that opens Settings → Accounts). Other failures (offline, timeout, server errors) keep the token
and retry with backoff (30 s, doubling, at most 10 min). A `401` on any later request (`api::Session::rejected`)
also expires the session. Sign out forgets the token in the session, keyring, encrypted fallback, and memory
fallback.

## Consented browser import

The Accounts page has two distinct actions:

1. **Find browser profiles** reads only browser profile metadata and displays non-secret browser/profile names. The
   first profile found is preselected.
2. **Import session** opens a consent dialog naming the selected browser, its installation (native, Flatpak, Snap)
   and profile. For Chromium-family browsers it also says that the browser's cookie password will be read from the
   desktop keyring or KWallet, which may ask to be unlocked. Only after confirmation does `AccountController` call
   `IBrowserCookieSource::importToken()` for that profile.

`auth::BrowserCookieSource` discovers Chrome, Chromium, Brave, Edge, Vivaldi, Opera, Firefox, and LibreWolf:

| Browser | Native | Flatpak | Snap |
|---|---|---|---|
| Chrome | `~/.config/google-chrome` | `com.google.Chrome` | — |
| Chromium | `~/.config/chromium` | `org.chromium.Chromium` | `~/snap/chromium/common/chromium` |
| Brave | `~/.config/BraveSoftware/Brave-Browser` | `com.brave.Browser` | `~/snap/brave/current/.config/…` |
| Edge | `~/.config/microsoft-edge` | `com.microsoft.Edge` | — |
| Vivaldi | `~/.config/vivaldi` | `com.vivaldi.Vivaldi` | `~/snap/vivaldi/current/.config/vivaldi` |
| Opera | `~/.config/opera` (profile-less) | `com.opera.Opera` | `~/snap/opera/current/.config/opera` |
| Firefox | `~/.mozilla/firefox`, `$XDG_CONFIG_HOME/mozilla/firefox` | `org.mozilla.firefox` | `~/snap/firefox/common/.mozilla/firefox` |
| LibreWolf | `~/.librewolf`, `$XDG_CONFIG_HOME/librewolf` | `io.gitlab.librewolf-community` | — |

Flatpak roots are `~/.var/app/<id>/…`. Chromium profiles come from `Local State` (or `Default` / `Profile *`), plus
the user-data directory itself when it holds `Cookies` (Opera has no `Default/`). Firefox-family profiles come from
`profiles.ini`, in numeric order. Discovery and import run on a private worker pool. The Brave, Vivaldi and Opera
Snap paths follow the Snap `$HOME` convention and have not been checked against installed snaps.

For an import, the reader snapshots the cookie DB and any `-wal`, `-shm` and `-journal` sidecars into a private
`0700` directory (on `$XDG_RUNTIME_DIR`, a per-user tmpfs, when available), opens only the snapshot with SQLite
read-only, and deletes it afterwards; a failed sidecar copy fails the import instead of reading an older state. It
never opens or modifies the browser's live database. The query is restricted to `oauth_token` on `soundcloud.com` or
its subdomains (`evilsoundcloud.com` does not match), most recently used first; expired, empty, control-character,
and oversized values are rejected. No other cookie is returned to the caller. Buffers holding the cookie password
and cookie values are wiped (`auth::wipe`, which skips implicitly shared buffers it does not own).

- **Firefox / LibreWolf:** the value from `moz_cookies` in `cookies.sqlite`. `expiry` is read as seconds, or as
  milliseconds when it is too large for seconds (recent Firefox versions).
- **Chromium family on Linux** (`components/os_crypt/sync/os_crypt_linux.cc`): AES-128-CBC with an IV of 16 spaces
  and a key from PBKDF2-HMAC-SHA1 with the salt `saltysalt` and **one** iteration (1003 is the macOS count).
  - `v10` values use the fixed password `peanuts`.
  - `v11` values use the browser's *Safe Storage* password from the desktop secret store. It is asked for only when a
    `v11` value is found, at most once per import.
  - Both fall back to the key of an empty password, which Chromium itself writes when its secret store misbehaves.
  - From cookie database version 24 (Chromium 130, `meta` table) the plaintext starts with `SHA-256(host_key)`;
    it is checked and stripped. Older databases have no prefix.
  - Values with an empty `encrypted_value` are read from the plain `value` column.
  - Other prefixes (for example Windows' app-bound `v20`) are reported as unsupported.

  The Safe Storage password (`auth::chromiumSafeStoragePassword`) comes from the Secret Service (libsecret; the item
  labelled `<name> Safe Storage` in the default collection; a locked collection hides its labels, so it is unlocked
  with the desktop's prompt before the scan) or from
  KWallet over D-Bus (`org.kde.kwalletd6`, else `kwalletd5`: entry `<name> Safe Storage` in folder `<name> Keys` of
  the network wallet; never created if missing). KWallet is asked first on KDE Plasma. `<name>` is `Chrome`
  (Chrome, Vivaldi), `Chromium` (Chromium, Edge, Opera) or `Brave`; the browser-specific names are macOS-only.

Unknown encryption versions, unavailable or locked keys, corrupt databases, and missing sessions produce typed errors
and translated, actionable UI messages; manual paste stays available. Windows DPAPI/app-bound encryption and macOS
Keychain are not implemented: a Windows or macOS reader would be another `IBrowserCookieSource`. On macOS the
`ChromiumPasswordProvider` seam fits (Keychain password + PBKDF2); Windows' DPAPI-wrapped AES-256-GCM key in
`Local State` needs its own decryption, not only a different password provider.

## Tests

- `tests/browser_auth_tests.cpp` builds SQLite fixtures with the real Chromium (version 24, `meta` table) and Firefox
  (`moz_cookies`) schemas and covers native, Flatpak, Snap and profile-less Opera discovery; `v10` with the built-in
  key even when a keyring password exists, `v11` with the keyring password, the empty-password fallback, wrong and
  missing passwords, locked keyrings, the domain hash (version 23 vs 24, wrong domain, dot hosts), the keyring being
  asked once and only for `v11`, most-recent-first selection, expiry (Chromium; Firefox in seconds and milliseconds),
  plain values, unrelated and look-alike hosts, unknown versions, and Chromium and Firefox WAL snapshots with the live
  files unchanged. Tests inject the Chromium password provider; only the opt-in `real browser profile import`
  (`--no-skip`) reads the current user's profiles and keyring.
- `tests/encrypted_secret_tests.cpp` covers round-trip/overwrite/remove, persistence across instances, owner-only
  permissions, absence of plaintext, rejection of flipped nonce, ciphertext and tag bytes, of a file moved to another
  key (AAD), of a wrong master key, and that the master key is never replaced.
- `tests/secret_chain_tests.cpp` drives `SecretServiceStore` with a fake `IKeyring`: keyring-only storage, fallback
  to the file and recovery in a later session, removal from every tier after a keyring error, a sign-out racing a
  slow failing keyring write, and memory-only operation.
- `tests/auth_tests.cpp` covers the memory store and the Accounts view-model against `tests/mock/FakeApi.hpp`: manual
  and browser sign-in, verification and persistence, empty/rejected/CR-LF tokens, a refused token while signed in
  (session, other requests and `Session::rejected` untouched), cache bypass for candidates, sign-out during a check,
  a slow startup keyring read, expiry at startup with the Sign in action, reader failure, and that discovery alone
  never imports a cookie.
