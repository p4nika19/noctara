# Authentication and the session

Noctara signs in with the user's own SoundCloud session, held as an OAuth token. The token authorises the api-v2
client (see [api.md](api.md)); it is only ever sent to SoundCloud's API hosts over HTTPS, and it is never written to
the configuration file, logs, diagnostics, or QML. The `auth` module depends only on `core` plus sqlite3, OpenSSL,
and libsecret. `ui::AccountController` (the QML `Account` singleton) connects browser import, manual sign-in, secret
storage, and the API session.

## Where the token lives

| Place | Lifetime | Notes |
|---|---|---|
| `api::Session` | process memory | the in-memory holder; the API client reads it per request |
| `auth::SecretServiceStore` | persistent | the desktop Secret Service through libsecret |
| `auth::EncryptedFileSecretStore` | persistent | fallback when Secret Service is unavailable |
| `auth::MemorySecretStore` | process memory | last resort if neither persistent backend can be used |

The token is stored under `soundcloud.oauth_token`, labelled "Noctara SoundCloud session" in the keyring manager.
Proxy credentials use the same store (see [network.md](network.md)). All keyring and file operations run off the UI
thread, and `persistent()` / `keyringBacked()` let the Accounts page describe the active protection accurately.

The encrypted fallback lives under the platform state directory (`~/.local/state/noctara/secrets/` on Linux), not in
the config or cache. Each secret has its own versioned AES-256-GCM file with a random nonce and the key name as
authenticated data. Files are replaced atomically; directories are `0700` and files `0600`. A random 256-bit master
key is stored separately in `master.key` with owner-only permissions. This protects data at rest, but it is **not
equivalent to a desktop keyring** against an attacker who can read the user's whole account. The UI displays this
warning. If the encrypted store also fails, memory-only storage is used and the UI says the session will be forgotten
at exit.

## One verified sign-in path

```
pasted token ───────────────┐
                            ▼
consented browser import ─► AccountController::signInWithToken
                                 │ set candidate on api::Session
                                 ▼
                            GET /me (Auth::Required)
                               │             │
                              200           401/403
                               │             └► restore previous session, show error
                               ▼
                    applyUser, store in ISecretStore, state = SignedIn
```

Every token is **verified against `/me` before it is kept**. A bad import or paste never replaces a working session.
On success the account username, display name, avatar, and profile URL are shown. Manual paste remains available as
a fallback for unsupported browsers and cookie encryption versions.

At startup `AccountController` loads the stored token, sets it on the session immediately, and re-checks it in the
background. A `401` downgrades to `Expired` and clears the stored token; a network error leaves it in place to retry.
A `401` on any later request (`api::Session::rejected`) also expires the session. Sign out forgets the token in the
session, keyring, encrypted fallback, and memory fallback.

## Consented browser import

The Accounts page has two distinct actions:

1. **Find browser profiles** reads only browser profile metadata and displays non-secret browser/profile names.
2. **Import session** opens a consent dialog naming the selected browser and profile. Only after confirmation does
   `AccountController` call `IBrowserCookieSource::importToken()` for that profile.

`auth::BrowserCookieSource` discovers Chrome, Chromium, Brave, Edge, Vivaldi, Opera, Firefox, and LibreWolf in their
native Linux, Flatpak, and Snap profile locations. Firefox and LibreWolf are also found under the XDG configuration
directory (`$XDG_CONFIG_HOME/mozilla/firefox` and `$XDG_CONFIG_HOME/librewolf`, defaulting to `~/.config`). Chromium
profiles come from `Local State`; Firefox-family profiles come from `profiles.ini`. Discovery and import run on a
private worker pool.

For an import, the reader creates a private temporary snapshot of the cookie DB and any WAL/SHM sidecars, opens only
the snapshot with SQLite read-only, and deletes the snapshot afterwards. It never opens or modifies the browser's
live database. The query is restricted to `oauth_token` on `soundcloud.com` or its subdomains; expired, empty,
control-character, and oversized values are rejected. No other cookie is returned to the caller.

- **Firefox / LibreWolf:** read the cookie value from `moz_cookies` in `cookies.sqlite`.
- **Chromium family on Linux:** decrypt `v10` values using Chromium's documented PBKDF2-HMAC-SHA1 (`saltysalt`, 1003
  iterations) and AES-128-CBC format. The Safe Storage password is looked up through libsecret with the browser's
  application name; Chromium's documented `peanuts` fallback is used when no Safe Storage item exists. Strict padding
  and token validation prevent malformed plaintext or ciphertext from reaching the API.

Unknown encryption versions, unavailable keys, corrupt databases, and missing sessions produce typed errors and
translated, actionable UI messages. Windows DPAPI/app-bound encryption and macOS Keychain are not implemented yet;
the platform seam is `ChromiumPasswordProvider` and the profile family API. On Linux, newer non-`v10` formats are
reported as unsupported rather than silently mishandled.

## Tests

- `tests/browser_auth_tests.cpp` creates temporary SQLite/OpenSSL fixtures and covers native, Flatpak, and Snap
  discovery, Chromium and Firefox extraction, consent-independent metadata discovery, wrong hosts, unsupported
  encryption, wrong passwords, WAL snapshots, and proof that source files remain unchanged. Tests inject the Chromium
  password provider and never touch real profiles or keyrings.
- `tests/encrypted_secret_tests.cpp` covers round-trip/overwrite/remove, persistence across instances, owner-only
  permissions, absence of plaintext, authentication-tamper rejection, and wrong-master-key rejection.
- `tests/auth_tests.cpp` covers the memory store and Accounts view-model against `tests/mock/FakeApi.hpp`: manual and
  browser sign-in, verification and persistence, empty/rejected tokens, reader failure, sign-out, expiry, startup
  loading, and that discovery alone never imports a cookie.
