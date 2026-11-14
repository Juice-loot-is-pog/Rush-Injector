// Patreon -> Rush Beta keys.
//
//   GET  /patreon/login       Sends the visitor to "Log in with Patreon". ?new=1 asks for a new key.
//   GET  /patreon/callback    Patreon sends them back here. An active patron on a Beta tier gets
//                             a Beta account and key (shown once, since only its hash is stored).
//                             A returning patron has their access refreshed and sees their username.
//   POST /patreon/webhook     Patreon reports membership changes; the key's expiry follows them.
//
// Patreon never revokes or suspends anything. It only moves beta_keys.expires_at, so a key
// revoked, removed or suspended by a developer stays that way.
//
// Settings (wrangler.toml [vars] unless marked secret):
//   PATREON_CLIENT_ID         Patreon client id.
//   PATREON_CLIENT_SECRET     (secret) Patreon client secret.
//   PATREON_WEBHOOK_SECRET    (secret) Patreon webhook secret.
//   PATREON_CAMPAIGN_ID       Only memberships of this campaign count.
//   PATREON_BETA_TIER_IDS     Comma-separated tier ids that include Beta. Empty = any paid tier.
//   PATREON_ALLOWED_USER_IDS  Optional comma-separated Patreon user ids that always get Beta.
//   RUSH_SITE_URL             Optional. The website, linked from pages and client messages.

const PATREON = 'https://www.patreon.com';
const PATREON_PAGE = 'https://www.patreon.com/cw/RushUtilityMod';
const DAY = 86400000;
const STATE_TTL = 10 * 60 * 1000;
// Access lasts this long past Patreon's next charge date, so a late renewal webhook or a
// retried payment never locks out a paying patron.
const RENEWAL_GRACE = 5 * DAY;
// Used when Patreon gives no next charge date.
const DEFAULT_PERIOD = 35 * DAY;
// Accounts on PATREON_ALLOWED_USER_IDS.
const ALLOWED_PERIOD = 365 * DAY;

const encode = new TextEncoder();
const hex = bytes => Array.from(new Uint8Array(bytes), b => b.toString(16).padStart(2, '0')).join('');
const digest = async value => hex(await crypto.subtle.digest('SHA-256', encode.encode(value)));
const randomHex = n => hex(crypto.getRandomValues(new Uint8Array(n)));
const list = value => String(value || '').split(',').map(s => s.trim()).filter(Boolean);

function sameString(a, b) {
  if (typeof a !== 'string' || typeof b !== 'string' || a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

function cookie(request, name) {
  for (const part of (request.headers.get('Cookie') || '').split(';')) {
    const [k, ...v] = part.trim().split('=');
    if (k === name) return v.join('=');
  }
  return '';
}

const callbackUrl = request => new URL('/patreon/callback', request.url).toString();

// ------------------------------------------------------------------ membership rules

export function membershipGrantsBeta(env, member, tierIds) {
  if (!member) return {ok: false, reason: 'not-member'};
  if (member.patron_status !== 'active_patron') return {ok: false, reason: 'inactive'};
  const betaTiers = list(env.PATREON_BETA_TIER_IDS);
  if (betaTiers.length ? !tierIds.some(id => betaTiers.includes(String(id))) : !(member.currently_entitled_amount_cents > 0))
    return {ok: false, reason: 'wrong-tier'};
  return {ok: true};
}

export function accessUntil(member, now = Date.now()) {
  const next = Date.parse(member?.next_charge_date || '');
  return Number.isFinite(next) ? Math.max(next, now) + RENEWAL_GRACE : now + DEFAULT_PERIOD;
}

// This campaign's membership from an /identity response.
export function findMembership(env, identity) {
  const included = identity.included || [];
  for (const ref of identity.data?.relationships?.memberships?.data || []) {
    const m = included.find(x => x.type === 'member' && x.id === ref.id);
    if (!m) continue;
    if (env.PATREON_CAMPAIGN_ID && String(m.relationships?.campaign?.data?.id) !== String(env.PATREON_CAMPAIGN_ID)) continue;
    return {member: m.attributes || {}, tierIds: (m.relationships?.currently_entitled_tiers?.data || []).map(t => String(t.id))};
  }
  return {member: null, tierIds: []};
}

// ---------------------------------------------------------------------- database

function usernameCandidates(user) {
  const base = String(user.vanity || user.full_name || '').normalize('NFKD')
    .replace(/[^A-Za-z0-9_.-]+/g, '').slice(0, 40);
  const tail = String(user.id).slice(-4);
  const out = base ? [base, `${base}-${tail}`, `${base}-${randomHex(2)}`] : [];
  out.push(`patron${user.id}`.slice(0, 48), `patron-${randomHex(4)}`);
  return out;
}

async function linkedKey(env, patreonId) {
  return env.BETA_DB.prepare(`SELECT p.key_id, k.username, k.expires_at, k.revoked_at, a.status AS account_status,
      EXISTS(SELECT 1 FROM removed_beta_keys r WHERE r.key_id=k.id) AS removed
    FROM patreon_links p JOIN beta_keys k ON k.id=p.key_id JOIN beta_accounts a ON a.username=k.username
    WHERE p.patreon_id=?`).bind(patreonId).first();
}

const blocked = link => !!(link.revoked_at || link.removed || link.account_status !== 'active');

// Creates the patron's account and key, or refreshes an existing one. Returns
// {username, key?, expiresAt, created?, banned?}. `key` is only set when a new secret was made.
export async function claimBeta(env, user, until, newKey = false) {
  const now = Date.now();
  const existing = await linkedKey(env, user.id);
  if (existing) {
    if (blocked(existing)) return {username: existing.username, banned: true};
    const statements = [env.BETA_DB.prepare('UPDATE beta_keys SET expires_at=MAX(expires_at,?) WHERE id=?').bind(until, existing.key_id)];
    let key;
    if (newKey) {
      // A new secret replaces the old one on the same key, and the device lock is cleared so
      // it can be used on a new PC. The old secret stops working at once.
      key = randomHex(32);
      statements.push(env.BETA_DB.prepare('UPDATE beta_keys SET key_hash=? WHERE id=?').bind(await digest(key), existing.key_id));
      statements.push(env.BETA_DB.prepare('DELETE FROM beta_devices WHERE key_id=?').bind(existing.key_id));
    }
    await env.BETA_DB.batch(statements);
    return {username: existing.username, key, expiresAt: Math.max(existing.expires_at, until)};
  }

  for (const username of usernameCandidates(user)) {
    const key = randomHex(32), id = crypto.randomUUID();
    try {
      // One transaction: a taken username (manual account) or a parallel sign-in of the same
      // patron fails it as a whole, leaving nothing half made.
      await env.BETA_DB.batch([
        env.BETA_DB.prepare('INSERT INTO beta_accounts(username,created_at) VALUES(?,?)').bind(username, now),
        env.BETA_DB.prepare('INSERT INTO beta_keys(id,key_hash,username,created_at,expires_at) VALUES(?,?,?,?,?)')
          .bind(id, await digest(key), username, now, until),
        env.BETA_DB.prepare('INSERT INTO patreon_links(patreon_id,key_id,created_at) VALUES(?,?,?)').bind(user.id, id, now),
      ]);
      return {username, key, expiresAt: until, created: true};
    } catch {
      if (await linkedKey(env, user.id)) return claimBeta(env, user, until, newKey);
    }
  }
  throw new Error('no free username');
}

async function setExpiry(env, patreonId, sql, value) {
  await env.BETA_DB.prepare(`UPDATE beta_keys SET expires_at=${sql}(expires_at,?)
    WHERE id=(SELECT key_id FROM patreon_links WHERE patreon_id=?)`).bind(value, patreonId).run();
}

// For /beta/login and /beta/status: a clearer message when a Patreon key has run out.
export async function patreonKeyMessage(env, key) {
  if (!key || !env.BETA_DB) return null;
  try {
    const row = await env.BETA_DB.prepare(`SELECT k.expires_at, k.revoked_at FROM beta_keys k
      JOIN patreon_links p ON p.key_id=k.id WHERE k.key_hash=?`).bind(await digest(key)).first();
    if (!row || row.revoked_at || row.expires_at > Date.now()) return null;
    const where = env.RUSH_SITE_URL ? ` on ${env.RUSH_SITE_URL}` : '';
    return `Your Beta access from Patreon has ended. If you're still a patron on a Beta tier, click "Get my Beta key"${where} and sign in with Patreon to refresh it; your key stays the same.`;
  } catch { return null; }
}

// ------------------------------------------------------------------------- pages

const escapeHtml = s => String(s).replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
const formatDate = ms => new Date(ms).toLocaleDateString('en-US', {year: 'numeric', month: 'long', day: 'numeric', timeZone: 'UTC'});

function page(env, title, body, status = 200) {
  const back = env.RUSH_SITE_URL ? `<a class="back" href="${escapeHtml(env.RUSH_SITE_URL)}">← Back to Rush</a>` : '';
  return new Response(`<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><meta name="robots" content="noindex">
<title>${escapeHtml(title)} · Rush</title>
<style>
:root{--bg:#0b0606;--card:#1a0e0e;--border:#3a1a17;--text:#f4ebe9;--muted:#b39d99;--red:#e5372b;--orange:#ff7a1a}
*{box-sizing:border-box}body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px 16px;
background:radial-gradient(ellipse at top,#3a0d08,var(--bg) 60%);color:var(--text);font:16px/1.6 system-ui,sans-serif}
.card{width:100%;max-width:560px;background:var(--card);border:1px solid var(--border);border-radius:16px;padding:32px;box-shadow:0 0 60px rgba(229,55,43,.2)}
h1{margin:0 0 8px;font-size:26px}p{color:var(--muted);margin:0 0 16px}p strong{color:var(--text)}
.field{margin:16px 0}.label{font-size:13px;color:var(--muted);margin-bottom:6px}.row{display:flex;gap:8px}
.value{flex:1;min-width:0;padding:12px 14px;border-radius:10px;background:#0f0808;border:1px solid var(--border);font:600 15px ui-monospace,Consolas,monospace;overflow-wrap:anywhere;user-select:all}
button,.btn{border:0;cursor:pointer;padding:12px 18px;border-radius:10px;color:#fff;font:600 15px system-ui,sans-serif;text-decoration:none;background:linear-gradient(135deg,var(--red),var(--orange));display:inline-block}
.ghost{background:transparent;border:1px solid var(--red)}.warn{color:#ffb38a}
.steps{color:var(--muted);padding-left:20px;margin:20px 0 0}.steps li{margin:6px 0}.steps strong{color:var(--text)}.back{display:block;width:fit-content;margin-top:24px;color:var(--muted)}
</style></head><body><main class="card">${body}${back}</main>
<script>document.querySelectorAll("[data-copy]").forEach(b=>b.addEventListener("click",()=>navigator.clipboard.writeText(document.getElementById(b.dataset.copy).textContent).then(()=>{b.textContent="Copied";setTimeout(()=>b.textContent="Copy",1500)})))</script>
</body></html>`, {status, headers: {
    'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store', 'Referrer-Policy': 'no-referrer',
    'X-Frame-Options': 'DENY', 'X-Content-Type-Options': 'nosniff',
    'Set-Cookie': 'rush_oauth=; Path=/patreon; Max-Age=0; HttpOnly; Secure; SameSite=Lax',
  }});
}

const errorPage = (env, title, message, status = 400) =>
  page(env, title, `<h1>${escapeHtml(title)}</h1><p>${escapeHtml(message)}</p><a class="btn" href="/patreon/login">Try again</a>`, status);

function noAccessPage(env, reason) {
  const why = {
    'not-member': "This Patreon account isn't a member of Rush.",
    'inactive': "This Patreon account's membership isn't active (it was cancelled, or the last payment didn't go through).",
    'wrong-tier': "This Patreon account isn't on a tier that includes Beta.",
  }[reason] || "This Patreon account doesn't include Beta.";
  return page(env, 'No Beta access', `<h1>No Beta access yet</h1><p>${why}</p>
<p>Join a Beta tier on Patreon, then come back and click "Get my Beta key" again.</p>
<a class="btn" href="${PATREON_PAGE}" target="_blank" rel="noopener">Join on Patreon</a>`, 403);
}

const field = (label, id, value) => `<div class="field"><div class="label">${label}</div><div class="row">
<div class="value" id="${id}">${escapeHtml(value)}</div><button data-copy="${id}">Copy</button></div></div>`;

function resultPage(env, result) {
  if (result.banned) return page(env, 'Beta access turned off', `<h1>Beta access turned off</h1>
<p>Beta for this Patreon account (username <strong>${escapeHtml(result.username)}</strong>) was turned off by Rush staff. Contact us if you think this is a mistake.</p>`, 403);
  const until = `<p>Beta access is active until <strong>${formatDate(result.expiresAt)}</strong> and renews with your Patreon membership.</p>`;
  if (!result.key) return page(env, 'Your Beta access', `<h1>You're all set</h1>${until}
${field('Username', 'u', result.username)}
<p>Your key was shown when you first got it. If you lost it or moved to a new PC, make a new one. Your old key stops working straight away.</p>
<a class="btn ghost" href="/patreon/login?new=1">Make a new key</a>`);
  return page(env, 'Your Beta key', `<h1>${result.created ? 'Thanks for supporting Rush!' : 'Your new Beta key'}</h1>${until}
${field('Username', 'u', result.username)}${field('Beta key', 'k', result.key)}
<p class="warn"><strong>Save this key now.</strong> It's only shown once. If you lose it, come back and make a new one.</p>
<ol class="steps"><li>Open <strong>Rush Injector</strong> and choose Beta.</li>
<li>Enter this <strong>username</strong> and <strong>key</strong> and sign in.</li>
<li>Keep the key private. It works on one PC.</li></ol>`);
}

// ------------------------------------------------------------------------ routes

async function login(request, env) {
  if (!env.PATREON_CLIENT_ID || !env.PATREON_CLIENT_SECRET)
    return errorPage(env, 'Not set up yet', "Patreon sign-in isn't set up on the server yet.", 503);
  const now = Date.now(), state = randomHex(24);
  const newKey = new URL(request.url).searchParams.get('new') === '1' ? 1 : 0;
  await env.BETA_DB.batch([
    env.BETA_DB.prepare('DELETE FROM oauth_states WHERE expires_at<?').bind(now),
    env.BETA_DB.prepare('INSERT INTO oauth_states(state,new_key,expires_at) VALUES(?,?,?)').bind(state, newKey, now + STATE_TTL),
  ]);
  const url = new URL(`${PATREON}/oauth2/authorize`);
  url.search = new URLSearchParams({response_type: 'code', client_id: env.PATREON_CLIENT_ID,
    redirect_uri: callbackUrl(request), scope: 'identity identity.memberships', state}).toString();
  return new Response(null, {status: 302, headers: {Location: url.toString(), 'Cache-Control': 'no-store',
    'Set-Cookie': `rush_oauth=${state}; Path=/patreon; Max-Age=${STATE_TTL / 1000}; HttpOnly; Secure; SameSite=Lax`}});
}

async function callback(request, env) {
  const params = new URL(request.url).searchParams;
  if (params.get('error')) return errorPage(env, 'Sign-in cancelled', 'Patreon sign-in was cancelled, so nothing changed.');
  const code = params.get('code') || '', state = params.get('state') || '';
  if (!code || !/^[a-f0-9]{48}$/.test(state) || !sameString(state, cookie(request, 'rush_oauth')))
    return errorPage(env, 'Sign-in expired', 'That sign-in link expired or was opened in a different browser. Start again in this browser.');
  const saved = await env.BETA_DB.prepare('DELETE FROM oauth_states WHERE state=? RETURNING new_key,expires_at').bind(state).first();
  if (!saved || saved.expires_at < Date.now())
    return errorPage(env, 'Sign-in expired', 'That sign-in link expired. Start again.');

  const token = await fetch(`${PATREON}/api/oauth2/token`, {method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: new URLSearchParams({code, grant_type: 'authorization_code', client_id: env.PATREON_CLIENT_ID,
      client_secret: env.PATREON_CLIENT_SECRET, redirect_uri: callbackUrl(request)})});
  if (!token.ok) return errorPage(env, 'Patreon error', "Patreon didn't accept the sign-in. Please try again.", 502);
  const {access_token: accessToken} = await token.json();

  const identityUrl = new URL(`${PATREON}/api/oauth2/v2/identity`);
  identityUrl.search = new URLSearchParams({
    include: 'memberships,memberships.campaign,memberships.currently_entitled_tiers',
    'fields[user]': 'full_name,vanity',
    'fields[member]': 'patron_status,currently_entitled_amount_cents,next_charge_date,last_charge_status',
  }).toString();
  const identityResponse = await fetch(identityUrl, {headers: {Authorization: `Bearer ${accessToken}`}});
  if (!identityResponse.ok) return errorPage(env, 'Patreon error', "Couldn't read your Patreon membership. Please try again.", 502);
  const identity = await identityResponse.json();
  const user = {id: String(identity.data?.id || ''), ...(identity.data?.attributes || {})};
  if (!/^\d+$/.test(user.id)) return errorPage(env, 'Patreon error', "Patreon didn't return an account. Please try again.", 502);

  if (list(env.PATREON_ALLOWED_USER_IDS).includes(user.id))
    return resultPage(env, await claimBeta(env, user, Date.now() + ALLOWED_PERIOD, !!saved.new_key));
  const {member, tierIds} = findMembership(env, identity);
  const verdict = membershipGrantsBeta(env, member, tierIds);
  if (!verdict.ok) {
    await setExpiry(env, user.id, 'MIN', Date.now());
    return noAccessPage(env, verdict.reason);
  }
  return resultPage(env, await claimBeta(env, user, accessUntil(member), !!saved.new_key));
}

async function webhook(request, env) {
  if (Number(request.headers.get('Content-Length')) > 65536) return new Response('too large', {status: 413});
  const body = await request.text();
  const signature = (request.headers.get('X-Patreon-Signature') || '').toLowerCase();
  if (!env.PATREON_WEBHOOK_SECRET || !sameString(hmacMd5Hex(env.PATREON_WEBHOOK_SECRET, body), signature))
    return new Response('bad signature', {status: 401});
  let data;
  try { data = JSON.parse(body).data || {}; } catch { return new Response('bad json', {status: 400}); }
  const userId = String(data.relationships?.user?.data?.id || '');
  const campaignId = String(data.relationships?.campaign?.data?.id || '');
  if (!/^\d+$/.test(userId) || list(env.PATREON_ALLOWED_USER_IDS).includes(userId)) return new Response('ok');
  if (env.PATREON_CAMPAIGN_ID && campaignId && campaignId !== String(env.PATREON_CAMPAIGN_ID)) return new Response('ok');

  const member = data.attributes || {};
  const tierIds = (data.relationships?.currently_entitled_tiers?.data || []).map(t => String(t.id));
  const deleted = (request.headers.get('X-Patreon-Event') || '') === 'members:delete';
  if (!deleted && membershipGrantsBeta(env, member, tierIds).ok) await setExpiry(env, userId, 'MAX', accessUntil(member));
  else await setExpiry(env, userId, 'MIN', Date.now());
  return new Response('ok');
}

export async function patreonRoutes(request, env) {
  const url = new URL(request.url);
  if (!url.pathname.startsWith('/patreon/')) return null;
  if (url.protocol !== 'https:') return new Response('HTTPS required', {status: 400});
  if (!env.BETA_DB) return new Response('Beta storage not configured', {status: 503});
  try {
    if (url.pathname === '/patreon/login' && request.method === 'GET') return await login(request, env);
    if (url.pathname === '/patreon/callback' && request.method === 'GET') return await callback(request, env);
    if (url.pathname === '/patreon/webhook' && request.method === 'POST') return await webhook(request, env);
    return new Response('Not found', {status: 404});
  } catch {
    if (url.pathname === '/patreon/webhook') return new Response('error', {status: 500}); // Patreon retries
    return errorPage(env, 'Something went wrong', 'The Beta service had a problem. Please try again in a minute.', 503);
  }
}

// ------------------------------------------------------------------- HMAC-MD5
// Patreon signs webhooks with HMAC-MD5, which WebCrypto doesn't provide.

function md5(bytes) {
  const K = Array.from({length: 64}, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) | 0);
  const S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  const len = bytes.length, padded = new Uint8Array(((len + 8) >> 6 << 6) + 64);
  padded.set(bytes); padded[len] = 0x80;
  const view = new DataView(padded.buffer);
  view.setUint32(padded.length - 8, (len * 8) >>> 0, true);
  view.setUint32(padded.length - 4, Math.floor(len / 0x20000000), true);
  let a0 = 0x67452301, b0 = 0xefcdab89 | 0, c0 = 0x98badcfe | 0, d0 = 0x10325476;
  for (let off = 0; off < padded.length; off += 64) {
    let a = a0, b = b0, c = c0, d = d0;
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
      else { f = c ^ (b | ~d); g = (7 * i) % 16; }
      const s = S[(i >> 4) * 4 + (i % 4)];
      const x = (a + f + K[i] + view.getInt32(off + g * 4, true)) | 0;
      a = d; d = c; c = b; b = (b + ((x << s) | (x >>> (32 - s)))) | 0;
    }
    a0 = (a0 + a) | 0; b0 = (b0 + b) | 0; c0 = (c0 + c) | 0; d0 = (d0 + d) | 0;
  }
  const out = new DataView(new ArrayBuffer(16));
  [a0, b0, c0, d0].forEach((v, i) => out.setInt32(i * 4, v, true));
  return new Uint8Array(out.buffer);
}

export function hmacMd5Hex(secret, message) {
  let key = encode.encode(secret);
  if (key.length > 64) key = md5(key);
  const msg = encode.encode(message), inner = new Uint8Array(64 + msg.length), outer = new Uint8Array(80);
  for (let i = 0; i < 64; i++) { inner[i] = (key[i] || 0) ^ 0x36; outer[i] = (key[i] || 0) ^ 0x5c; }
  inner.set(msg, 64); outer.set(md5(inner), 64);
  return hex(md5(outer));
}
