# Rush private downloads

Alpha /client GET and HEAD are public and require no download key. Beta /beta/client still requires an active private key, account, expiry and matching device binding, checked before every fresh or cached response. The server hashes the presented Beta secret itself; a stored hash, key ID or device identity is not a password. The old embedded Alpha key remains rejected for authenticated injection reports. Historical Alpha key provisioning instructions below apply only to custom services or legacy authenticated reports, not public Alpha downloads.

This Cloudflare Worker serves the private repository file `Juice-loot-is-pog/Rush-Dll`, branch `main`, path `RushClient_Alpha_v3.0.dll`. It is deployed and configured at `https://rush-download-service.juiceloot-github.workers.dev/client`. A real private DLL download passed native checksum and x64 validation on October 26, 2026. A browser request without a download key was rejected.

The GitHub token stays in a Worker secret. Users receive separate Rush download keys. The service stores only SHA-256 hashes of those keys and checks them before returning either a fresh or a cached DLL. Removing a user's hash revokes future downloads, including cached responses. A user can still copy their key or extract the DLL after downloading it; this is access control, not binary copy protection.

The owner PC's download key is saved in Windows Credential Manager, its hash is installed as `DOWNLOAD_KEY_HASHES`, and a token restricted to Rush-Dll Contents read-only is installed as `GITHUB_TOKEN`. The GitHub token expires November 25, 2026; rotate that Worker secret before expiry. Neither the owner key nor the GitHub token is stored in this source tree. `provision_download_key.exe` reuses an existing owner key and emits only its hash array; replacing the server's hash array with that output revokes any other customer keys, so preserve their hashes if customers have already been added.

## Deploy

Requires your Cloudflare account and a GitHub fine-grained token restricted to **Rush-Dll**, with **Contents: read-only**. Enter secrets directly into Wrangler's prompts or the Cloudflare dashboard. Keep tokens out of chat, source files, configuration files, and the executable.

From this directory:

```powershell
npx wrangler login
npx wrangler deploy
npx wrangler secret put GITHUB_TOKEN
```

Generate a unique key for each customer locally:

```powershell
node issue_key.mjs
```

Keep each user key private and give it only to that user. Add the server hashes as a JSON array, for example `["HASH_FOR_USER_ONE","HASH_FOR_USER_TWO"]`, in the `DOWNLOAD_KEY_HASHES` secret prompt:

```powershell
npx wrangler secret put DOWNLOAD_KEY_HASHES
```

The public download endpoint is `https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev/client`. The URL contains no secret. The service returns 503 until both secrets are configured. GitHub responses are limited to 32 MiB, checked for x64 DLL headers, and returned with a SHA-256 checksum. The Worker hashes each new upstream version (by GitHub ETag) once as it streams past, remembers only that digest, and then streams the file straight through, so it never holds the DLL in memory (holding it caused Cloudflare error 1102). Redirects are rejected so GitHub credentials are never forwarded to another host. GitHub errors are replaced by generic messages. The key hash list is intended for a small distribution; larger user populations need a proper licensing database and throttling.

## Use in Rush Injector

Click **Auto Inject**, enter the HTTPS service address and your Rush key, then save. Rush downloads and verifies the DLL and waits for Minecraft. The key is stored in Windows Credential Manager, scoped to that service URL. Downloaded DLLs are cached under the app's local application data `client` folder and remain recoverable from that machine. The manual DLL field does not reveal or change to the cached path. An authentication failure removes the saved key so another can be entered.

To distribute an executable with the public service address already filled in, configure the existing CMake build with `-DRUSH_DOWNLOAD_URL=https://YOUR-WORKER.YOUR-SUBDOMAIN.workers.dev/client`, then rebuild. No GitHub token or user key belongs in that setting.

Manual **Choose DLL** and **Inject client** still operate on the locally selected DLL. Auto Inject starts disabled and downloads the current private client once when enabled; a game restart reuses that verified download until Auto Inject is toggled again. A failed download never enables automatic injection or falls back to an older cached client.

The six embedded TTF files are placed under `<DLL directory>/fonts/` with their original filenames, matching the client's own module-relative font loader. Auto Inject prepares them after downloading, and both injection modes verify/repair them before loading the DLL. Folder and font-file read/execute permissions are granted to All Application Packages for Minecraft access. No service update or separate font download is needed.

## Validation

```powershell
node --test worker.test.mjs
```

Tests cover invalid and revoked keys, authentication before cache access, repository pinning, checksums, invalid binaries, size limits, redirect rejection, and non-disclosure of upstream error details. Native tests also cover HTTPS URL validation, header-injection rejection, SHA-256, malformed/altered DLLs, and a Credential Manager round trip with a disposable fixture key.

References: [Worker secrets](https://developers.cloudflare.com/workers/configuration/secrets/), [GitHub repository contents API](https://docs.github.com/en/rest/repos/contents).
