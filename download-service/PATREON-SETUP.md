# Patreon Beta keys

Patrons get their Beta username and key themselves: they click **Get my Beta key** on the website, sign in with Patreon, and the key appears. The code is `patreon.mjs`; tests are in `patreon.test.mjs`.

## How it behaves

- **First sign-in** by an active patron on a Beta tier creates a Beta account and key. The username comes from their Patreon vanity name or full name, with a suffix if it's taken. The key is shown once, because only its hash is stored.
- **Later sign-ins** refresh their access and show their username. **Make a new key** swaps in a new secret on the same key and clears the device lock (for a lost key or a new PC). The old secret stops working at once.
- **Expiry** is Patreon's next charge date plus 5 days, or 35 days from now if Patreon doesn't give a date. Webhooks move it forward on renewal and end it when the patron cancels, fails to pay, or leaves the Beta tier. Rejoining brings the same key back.
- **Bans stay bans.** Patreon only changes `expires_at`. A key that a developer revoked or removed, or an account they suspended, is never restored by Patreon.
- When a Patreon key runs out, the client shows a message saying to sign in with Patreon again, instead of the generic rejection.
- Patreon accounts in `PATREON_ALLOWED_USER_IDS` (for example yours) get a year of Beta without a membership.

## Setup

1. **Register a Patreon client** at https://www.patreon.com/portal/registration/register-clients (log in as the creator).
   - Redirect URI: `https://rush-download-service.juiceloot-github.workers.dev/patreon/callback`
   - Copy the **Client ID** and **Client Secret**.
2. **Find your ids.** In the same portal, the creator access token can call `https://www.patreon.com/api/oauth2/v2/campaigns?include=tiers&fields[tier]=title` (Authorization: Bearer <creator token>). That returns your campaign id and each tier's id and title. Note the ids of the tiers that include Beta.
3. **Add a webhook** at https://www.patreon.com/portal/registration/register-webhooks.
   - URL: `https://rush-download-service.juiceloot-github.workers.dev/patreon/webhook`
   - Triggers: `members:create`, `members:update`, `members:delete`, `members:pledge:create`, `members:pledge:update`, `members:pledge:delete`.
   - Copy the webhook **secret**.
4. **Fill in `wrangler.toml`**: `PATREON_CLIENT_ID`, `PATREON_CAMPAIGN_ID`, `PATREON_BETA_TIER_IDS` (comma-separated; empty means any paid tier), optionally `PATREON_ALLOWED_USER_IDS`, and `RUSH_SITE_URL` once the website is online.
5. **Add the two tables** to the live database. `schema.sql` only adds what's missing, so it's safe to run again:

   ```powershell
   npx wrangler d1 execute rush-beta --remote --file=schema.sql
   ```

6. **Add the secrets** (paste each one into the prompt, not into chat or files):

   ```powershell
   npx wrangler secret put PATREON_CLIENT_SECRET
   npx wrangler secret put PATREON_WEBHOOK_SECRET
   ```

7. **Check and deploy**:

   ```powershell
   node --test patreon.test.mjs beta.test.mjs worker.test.mjs
   npx wrangler deploy
   ```

   `wrangler deploy` also sets the other `[vars]` in `wrangler.toml` (the GitHub DLL paths). Make sure they match what's live before deploying.

8. **Try it**: open `https://rush-download-service.juiceloot-github.workers.dev/patreon/login`, sign in with a patron account (or your own account listed in `PATREON_ALLOWED_USER_IDS`), and sign in to Beta with the key it shows.

Until steps 1 to 6 are done, the Get my Beta key page says Patreon sign-in isn't set up yet. Nothing else on the service changes.
