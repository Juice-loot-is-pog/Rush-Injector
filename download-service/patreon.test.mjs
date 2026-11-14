import {test, beforeEach} from 'node:test';
import assert from 'node:assert/strict';
import {createHmac} from 'node:crypto';
import {readFileSync} from 'node:fs';
import {DatabaseSync} from 'node:sqlite';
import worker from './worker.mjs';
import {hmacMd5Hex} from './patreon.mjs';

// A D1-shaped wrapper over real SQLite, so every query runs against the real schema.
function d1(db) {
  const statement = (sql, args = []) => ({
    sql, args,
    bind: (...values) => statement(sql, values),
    async first() { return db.prepare(sql).get(...args) ?? null; },
    async all() { return {results: db.prepare(sql).all(...args)}; },
    async run() { const r = db.prepare(sql).run(...args); return {success: true, meta: {changes: Number(r.changes)}}; },
  });
  return {
    prepare: sql => statement(sql),
    async batch(list) {
      db.exec('BEGIN');
      try { const out = list.map(s => (db.prepare(s.sql).run(...s.args), {success: true})); db.exec('COMMIT'); return out; }
      catch (e) { db.exec('ROLLBACK'); throw e; }
    },
  };
}

const BASE = 'https://rush-download-service.example.test';
const DAY = 86400000;
let db, env, patron;

beforeEach(() => {
  db = new DatabaseSync(':memory:');
  db.exec('PRAGMA foreign_keys=ON');
  db.exec(readFileSync(new URL('./schema.sql', import.meta.url), 'utf8'));
  env = {
    BETA_DB: d1(db), PATREON_CLIENT_ID: 'cid', PATREON_CLIENT_SECRET: 'csecret', PATREON_WEBHOOK_SECRET: 'whsecret',
    PATREON_CAMPAIGN_ID: '999', PATREON_BETA_TIER_IDS: 'tier-beta', RUSH_SITE_URL: 'https://rush.example',
  };
  patron = {id: '4242', vanity: 'CoolGuy', status: 'active_patron', tiers: ['tier-beta'], cents: 500,
    next: new Date(Date.now() + 20 * DAY).toISOString()};
  globalThis.fetch = async (url, init) => {
    url = String(url);
    if (url === 'https://www.patreon.com/api/oauth2/token') {
      const p = new URLSearchParams(init.body);
      if (p.get('code') !== 'good' || p.get('client_secret') !== 'csecret' || p.get('redirect_uri') !== BASE + '/patreon/callback')
        return new Response('{}', {status: 401});
      return Response.json({access_token: 'token'});
    }
    if (url.startsWith('https://www.patreon.com/api/oauth2/v2/identity')) {
      assert.equal(init.headers.Authorization, 'Bearer token');
      return Response.json({
        data: {id: patron.id, type: 'user', attributes: {vanity: patron.vanity, full_name: 'Cool Guy'},
          relationships: {memberships: {data: patron.status ? [{id: 'm1', type: 'member'}] : []}}},
        included: patron.status ? [{id: 'm1', type: 'member',
          attributes: {patron_status: patron.status, currently_entitled_amount_cents: patron.cents, next_charge_date: patron.next},
          relationships: {campaign: {data: {id: '999'}}, currently_entitled_tiers: {data: patron.tiers.map(id => ({id}))}}}] : [],
      });
    }
    throw new Error('unexpected fetch ' + url);
  };
});

const call = (path, init) => worker.fetch(new Request(BASE + path, init), env);

async function signIn(path = '/patreon/login') {
  const start = await call(path);
  const state = new URL(start.headers.get('Location')).searchParams.get('state');
  const cookie = start.headers.get('Set-Cookie').split(';')[0];
  const response = await call(`/patreon/callback?code=good&state=${state}`, {headers: {Cookie: cookie}});
  const html = await response.text();
  return {status: response.status, html, username: html.match(/id="u">([^<]+)</)?.[1], key: html.match(/id="k">([^<]+)</)?.[1]};
}

const device = 'd'.repeat(64);
const betaLogin = (key, username, deviceHash = device) => call('/beta/login', {method: 'POST',
  headers: {Authorization: `Bearer ${key}`, 'X-Beta-Username': username, 'X-Device-Hash': deviceHash}});
const betaStatus = key => call('/beta/status', {headers: {Authorization: `Bearer ${key}`, 'X-Device-Hash': device}});

function webhook(event, attributes, tiers = [], {userId = patron.id, secret = 'whsecret'} = {}) {
  const body = JSON.stringify({data: {type: 'member', attributes, relationships: {
    user: {data: {id: userId}}, campaign: {data: {id: '999'}}, currently_entitled_tiers: {data: tiers.map(id => ({id}))}}}});
  return call('/patreon/webhook', {method: 'POST', body, headers: {
    'X-Patreon-Event': event, 'X-Patreon-Signature': createHmac('md5', secret).update(body).digest('hex')}});
}
const expiresAt = () => db.prepare('SELECT expires_at FROM beta_keys').get().expires_at;

test('HMAC-MD5 matches Node for short, long, unicode and long-key input', () => {
  for (const [secret, message] of [['s', ''], ['whsecret', '{"a":1}'], ['k'.repeat(100), 'x'.repeat(5000)], ['ünï', 'héllo 🎉']])
    assert.equal(hmacMd5Hex(secret, message), createHmac('md5', secret).update(message).digest('hex'));
});

test('login sends the visitor to Patreon with a one-use state cookie', async () => {
  const response = await call('/patreon/login');
  assert.equal(response.status, 302);
  const location = new URL(response.headers.get('Location'));
  assert.equal(location.origin + location.pathname, 'https://www.patreon.com/oauth2/authorize');
  assert.equal(location.searchParams.get('scope'), 'identity identity.memberships');
  assert.match(response.headers.get('Set-Cookie'), /^rush_oauth=[a-f0-9]{48}; Path=\/patreon; .*HttpOnly; Secure; SameSite=Lax$/);
  assert.equal(db.prepare('SELECT COUNT(*) AS n FROM oauth_states').get().n, 1);
});

test('login is refused until Patreon is configured', async () => {
  delete env.PATREON_CLIENT_SECRET;
  assert.equal((await call('/patreon/login')).status, 503);
});

test('callback needs the matching cookie and a state that has not been used', async () => {
  const start = await call('/patreon/login');
  const state = new URL(start.headers.get('Location')).searchParams.get('state');
  assert.equal((await call(`/patreon/callback?code=good&state=${state}`)).status, 400);
  const cookie = start.headers.get('Set-Cookie').split(';')[0];
  assert.equal((await call(`/patreon/callback?code=good&state=${state}`, {headers: {Cookie: cookie}})).status, 200);
  assert.equal((await call(`/patreon/callback?code=good&state=${state}`, {headers: {Cookie: cookie}})).status, 400);
});

test('a Beta patron gets an account and a key that works in the client', async () => {
  const first = await signIn();
  assert.equal(first.status, 200);
  assert.equal(first.username, 'CoolGuy');
  assert.match(first.key, /^[a-f0-9]{64}$/);
  const stored = db.prepare('SELECT key_hash, expires_at FROM beta_keys').get();
  assert.notEqual(stored.key_hash, first.key);
  assert.ok(Math.abs(stored.expires_at - (Date.parse(patron.next) + 5 * DAY)) < 1000);
  assert.equal((await betaLogin(first.key, 'CoolGuy')).status, 200);
  assert.equal((await betaStatus(first.key)).status, 200);
  assert.equal((await betaLogin(first.key, 'CoolGuy', 'e'.repeat(64))).status, 403, 'device lock still applies');
});

test('signing in again refreshes access without showing or changing the key', async () => {
  const first = await signIn();
  await betaLogin(first.key, first.username);
  patron.next = new Date(Date.now() + 40 * DAY).toISOString();
  const again = await signIn();
  assert.equal(again.status, 200);
  assert.equal(again.key, undefined);
  assert.equal(again.username, 'CoolGuy');
  assert.match(again.html, /Make a new key/);
  assert.ok(Math.abs(expiresAt() - (Date.parse(patron.next) + 5 * DAY)) < 1000);
  assert.equal((await betaStatus(first.key)).status, 200);
  assert.equal(db.prepare('SELECT COUNT(*) AS n FROM beta_keys').get().n, 1);
});

test('making a new key stops the old one and frees the device lock', async () => {
  const first = await signIn();
  await betaLogin(first.key, first.username);
  const second = await signIn('/patreon/login?new=1');
  assert.match(second.key, /^[a-f0-9]{64}$/);
  assert.notEqual(second.key, first.key);
  assert.equal(second.username, first.username);
  assert.equal((await betaStatus(first.key)).status, 401);
  assert.equal((await betaLogin(second.key, second.username, 'e'.repeat(64))).status, 200, 'new PC can bind');
  assert.equal(db.prepare('SELECT COUNT(*) AS n FROM beta_keys').get().n, 1);
});

test('a taken username gets a suffix instead of joining someone else\'s account', async () => {
  db.prepare("INSERT INTO beta_accounts(username,created_at) VALUES('CoolGuy',1)").run();
  const result = await signIn();
  assert.equal(result.username, 'CoolGuy-4242');
  patron.vanity = '';
  patron.id = '777';
  db.prepare("INSERT INTO beta_accounts(username,created_at) VALUES('CoolGuy-0777',1)").run();
  const other = await signIn();
  assert.match(other.username, /^CoolGuy(-0777)?-?/); // full_name fallback, suffixed when taken
  assert.notEqual(other.username, 'CoolGuy');
});

test('odd Patreon names become valid usernames', async () => {
  patron.vanity = '<script>Ünïcødé name!';
  const result = await signIn();
  assert.match(result.username, /^[A-Za-z0-9_.-]{1,48}$/);
  assert.doesNotMatch(result.html, /<script>Ü/);
});

test('people who are not Beta patrons get no account', async () => {
  for (const change of [{status: null}, {status: 'former_patron'}, {status: 'declined_patron'}, {tiers: ['tier-cheap']}]) {
    Object.assign(patron, {status: 'active_patron', tiers: ['tier-beta']}, change);
    const result = await signIn();
    assert.equal(result.status, 403);
    assert.equal(result.key, undefined);
  }
  assert.equal(db.prepare('SELECT COUNT(*) AS n FROM beta_accounts').get().n, 0);
});

test('without tier ids any paid membership counts', async () => {
  env.PATREON_BETA_TIER_IDS = '';
  patron.tiers = ['anything'];
  assert.equal((await signIn()).status, 200);
});

test('signing in after leaving the Beta tier ends access, rejoining brings the same key back', async () => {
  const first = await signIn();
  await betaLogin(first.key, first.username);
  patron.tiers = ['tier-cheap'];
  assert.equal((await signIn()).status, 403);
  const blocked = await betaStatus(first.key);
  assert.equal(blocked.status, 401);
  assert.match((await blocked.json()).error, /Patreon has ended.*https:\/\/rush\.example/);
  patron.tiers = ['tier-beta'];
  await signIn();
  assert.equal((await betaStatus(first.key)).status, 200);
});

test('webhooks need a valid signature', async () => {
  await signIn();
  const before = expiresAt();
  assert.equal((await webhook('members:update', {patron_status: 'former_patron'}, [], {secret: 'wrong'})).status, 401);
  assert.equal(expiresAt(), before);
});

test('webhooks end and restore access as the membership changes', async () => {
  const {key, username} = await signIn();
  await betaLogin(key, username);
  assert.equal((await webhook('members:update', {patron_status: 'declined_patron'})).status, 200);
  const login = await betaLogin(key, username);
  assert.equal(login.status, 401);
  assert.match((await login.json()).error, /Patreon has ended/);

  const next = new Date(Date.now() + 30 * DAY).toISOString();
  await webhook('members:pledge:create', {patron_status: 'active_patron', currently_entitled_amount_cents: 500, next_charge_date: next}, ['tier-beta']);
  assert.equal((await betaStatus(key)).status, 200);
  assert.ok(Math.abs(expiresAt() - (Date.parse(next) + 5 * DAY)) < 1000);

  await webhook('members:update', {patron_status: 'active_patron', currently_entitled_amount_cents: 100}, ['tier-cheap']);
  assert.equal((await betaStatus(key)).status, 401);
  await webhook('members:update', {patron_status: 'active_patron', currently_entitled_amount_cents: 500, next_charge_date: next}, ['tier-beta']);
  await webhook('members:delete', {patron_status: 'active_patron'}, ['tier-beta']);
  assert.equal((await betaStatus(key)).status, 401);
});

test('webhooks for people without a key change nothing', async () => {
  assert.equal((await webhook('members:create', {patron_status: 'active_patron'}, ['tier-beta'], {userId: '1'})).status, 200);
  assert.equal(db.prepare('SELECT COUNT(*) AS n FROM beta_keys').get().n, 0);
});

test('Patreon never undoes a revoke, removal or suspension', async () => {
  for (const ban of [
    "UPDATE beta_keys SET revoked_at=1",
    // Same as POST /admin/remove: revoke and hide.
    "UPDATE beta_keys SET revoked_at=1; INSERT INTO removed_beta_keys(key_id) SELECT id FROM beta_keys",
    "UPDATE beta_accounts SET status='suspended'",
  ]) {
    db.exec('DELETE FROM patreon_links; DELETE FROM beta_devices; DELETE FROM removed_beta_keys; DELETE FROM beta_keys; DELETE FROM beta_accounts');
    const {key, username} = await signIn();
    db.exec(ban);
    const again = await signIn('/patreon/login?new=1');
    assert.equal(again.status, 403);
    assert.equal(again.key, undefined);
    await webhook('members:update', {patron_status: 'active_patron', currently_entitled_amount_cents: 500}, ['tier-beta']);
    const login = await betaLogin(key, username);
    assert.equal(login.status, 401);
    assert.doesNotMatch((await login.json()).error, /Patreon/);
  }
});

test('allowed Patreon accounts get Beta without a membership', async () => {
  env.PATREON_ALLOWED_USER_IDS = '4242';
  patron.status = null;
  const result = await signIn();
  assert.equal(result.status, 200);
  assert.match(result.key, /^[a-f0-9]{64}$/);
  assert.ok(expiresAt() > Date.now() + 360 * DAY);
});

test('Patreon routes require HTTPS and ignore unknown paths', async () => {
  assert.equal((await worker.fetch(new Request('http://x.test/patreon/login'), env)).status, 400);
  assert.equal((await call('/patreon/other')).status, 404);
  assert.equal((await call('/patreon/webhook')).status, 404, 'GET webhook');
});
