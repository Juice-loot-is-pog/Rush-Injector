# Beta service

Deploy `schema.sql` to a Cloudflare D1 database and bind it as `BETA_DB`.
Set Worker secrets `ADMIN_USERNAME` and `ADMIN_KEY_HASH` (SHA-256 of a randomly generated 32-byte or longer administrator key).
Keep the plaintext administrator key outside the executable and repository. Admin calls use HTTPS with `X-Admin-Username` and `Authorization: Bearer <key>`.

Endpoints:

- `POST /admin/keys`: JSON `username`, `duration` (`day`, `week`, `month`, `lifetime`, `custom`), and `seconds` for custom duration. A month is 30 days. Expiration begins at creation; `lifetime` expires at the end of year 9999. Returns the plaintext key once.
- `POST /admin/extend`: JSON `id` plus `seconds` (60–31536000, added to the later of now and the current expiry) or `lifetime: true`. Revoked or removed keys are refused.
- `POST /admin/rotate`: JSON `id` replaces the key's secret and returns the new plaintext key once. The old secret stops working, the device binding is cleared, expiry and history stay. Revoked or removed keys are refused. Used by the Discord bot's `/beta newkey`.
- `POST /admin/key-info`: JSON `id`; one key's username, expiry, `lifetime`, revoked/removed, account status, device binding and `active`.
- `POST /admin/account-exists`: JSON `username`; `{exists}`. The Discord bot uses it to avoid giving a buyer someone else's username.
- `GET /admin/keys`: newest 1,000 keys, account status, expiry, remaining seconds and active status; never key hashes or secrets.
- `POST /admin/revoke`: JSON `id` permanently revokes a key.
- `POST /admin/reset-device`: JSON `id` clears device binding. The next successful username/secret login binds the key again.
- `POST /admin/account`: JSON `username`, `status` (`active` or `suspended`). Suspension blocks all account keys.
- `GET /admin/stats`: client-reported beta injection successes for 24 hours and all time.
- `GET /beta/status`: beta bearer key; current account and expiration.
- `POST /beta/login`: bearer secret key plus `X-Beta-Username` and `X-Device-Hash` headers. Atomically reserves the first device using the unique key ID; subsequent access requires that same hash. The Key ID is not a password.
- `POST /beta/injection`: beta bearer key and UUID `Idempotency-Key` header. Retrying the same event does not add another count.
- `GET /client`: Alpha artifact; accepts existing Alpha download credentials.
- `GET /beta/client`: private Beta artifact; requires an active, device-bound beta key. Checks expiry, revocation, account suspension and device hash before every cached or fresh download. Supports HEAD for the checksum.

Alpha download keys continue to work on the Alpha route and do not grant Beta access. `GITHUB_BETA_PATH` pins the Beta artifact independently, currently `RushClient_Beta_v1_MC26.60.dll` in Rush-Dll/main. The new Beta sign-in reveals an Inject Beta button, which downloads and validates the Beta DLL, prepares fonts, and makes one load attempt into the currently detected game. It does not enable Alpha Auto Inject. Beta files are cached separately under `beta-client`.
Beta access requires a matching `X-Device-Hash` on downloads, digest checks and injection reports, not only login. The Windows client sends a SHA-256 hash of a product-prefixed Windows MachineGuid; no raw identifier is transmitted. This identifies a Windows installation, not tamper-proof physical hardware, and can change on reinstall or be spoofed by modified software. Previously downloaded DLLs remain accessible; runtime licensing inside the supplied DLL has not been verified here. Beta login stores its verified secret in a separate Windows Credential Manager slot scoped to `/beta/client`, preserving the Alpha credential. Restart Minecraft before switching between Alpha and Beta.
Injection counts are reports from authenticated clients, not independent proof of injection, and can be falsified by a key holder. The new native build posts successful Auto Inject loads to `POST /injection` with a fresh UUID `Idempotency-Key`; the service records only channel and server timestamp, not usernames or process details. Historical/manual injections and older injector builds are not counted. Developer statistics use `client_reports`, not the older beta-only `injection_events` table.

The native suit-and-tie header button opens the developer panel inside the main window. Login verifies credentials at `/admin/stats`; every later request independently verifies them again. Credentials are retained only in process memory until sign-out or close, with no embedded secret or automatic credential-file loading. This is credential authentication, not a short-lived server session. The generated owner credential remains in its Windows-user-encrypted local credential file. Client publishing opens GitHub's authenticated upload workflow; the read-only service token has no publishing permission.
