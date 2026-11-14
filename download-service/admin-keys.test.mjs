import {test, beforeEach} from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {DatabaseSync} from 'node:sqlite';
import {betaRoutes, LIFETIME_EXPIRES_AT} from './beta.mjs';

// Real SQLite with the real schema, wrapped in the D1 calls beta.mjs uses.
function d1(db) {
  const statement = (sql, args = []) => ({
    sql, args, bind: (...values) => statement(sql, values),
    async first() { return db.prepare(sql).get(...args) ?? null; },
    async all() { return {results: db.prepare(sql).all(...args)}; },
    async run() { const r = db.prepare(sql).run(...args); return {success: true, meta: {changes: Number(r.changes)}}; },
  });
  return {prepare: sql => statement(sql), async batch(list) {
    db.exec('BEGIN');
    try { const out = list.map(s => (db.prepare(s.sql).run(...s.args), {success: true})); db.exec('COMMIT'); return out; }
    catch (e) { db.exec('ROLLBACK'); throw e; }
  }};
}

const adminKey = 'f'.repeat(64);
const adminHash = Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', new TextEncoder().encode(adminKey))),
  b => b.toString(16).padStart(2, '0')).join('');
let db, env;
beforeEach(() => {
  db = new DatabaseSync(':memory:');
  db.exec('PRAGMA foreign_keys=ON');
  db.exec(readFileSync(new URL('./schema.sql', import.meta.url), 'utf8'));
  env = {BETA_DB: d1(db), ADMIN_USERNAME: 'developer', ADMIN_KEY_HASH: adminHash};
});

const admin = (path, body) => betaRoutes(new Request('https://x.test' + path, {method: 'POST', body: JSON.stringify(body),
  headers: {Authorization: `Bearer ${adminKey}`, 'X-Admin-Username': 'developer'}}), env);
const device = 'd'.repeat(64);
const login = (key, username, dev = device) => betaRoutes(new Request('https://x.test/beta/login', {method: 'POST',
  headers: {Authorization: `Bearer ${key}`, 'X-Beta-Username': username, 'X-Device-Hash': dev}}), env);

test('lifetime keys are created with the far-future expiry and work', async () => {
  const r = await admin('/admin/keys', {username: 'buyer', duration: 'lifetime'});
  assert.equal(r.status, 201);
  const {id, key, expires_at} = await r.json();
  assert.equal(expires_at, LIFETIME_EXPIRES_AT);
  assert.equal((await login(key, 'buyer')).status, 200);
  const info = await (await admin('/admin/key-info', {id})).json();
  assert.equal(info.lifetime, true);
  assert.equal(info.active, true);
  assert.equal(info.device_bound, 1);
});

test('week and month still have their normal lengths', async () => {
  for (const [duration, days] of [['week', 7], ['month', 30]]) {
    const {expires_at} = await (await admin('/admin/keys', {username: 'u' + days, duration})).json();
    assert.ok(Math.abs(expires_at - (Date.now() + days * 86400000)) < 2000);
  }
  assert.equal((await admin('/admin/keys', {username: 'x', duration: 'forever'})).status, 400);
});

test('a key can be extended to lifetime, but not once revoked', async () => {
  const {id} = await (await admin('/admin/keys', {username: 'buyer', duration: 'week'})).json();
  assert.equal((await admin('/admin/extend', {id, lifetime: true})).status, 200);
  assert.equal((await (await admin('/admin/key-info', {id})).json()).lifetime, true);
  await admin('/admin/revoke', {id});
  assert.equal((await admin('/admin/extend', {id, lifetime: true})).status, 404);
});

test('rotate gives a new secret, stops the old one and frees the device lock', async () => {
  const {id, key} = await (await admin('/admin/keys', {username: 'buyer', duration: 'month'})).json();
  assert.equal((await login(key, 'buyer')).status, 200);
  const before = (await (await admin('/admin/key-info', {id})).json()).expires_at;
  const rotated = await admin('/admin/rotate', {id});
  assert.equal(rotated.status, 200);
  const {key: newKey} = await rotated.json();
  assert.match(newKey, /^[a-f0-9]{64}$/);
  assert.notEqual(newKey, key);
  assert.equal((await login(key, 'buyer')).status, 401);
  assert.equal((await login(newKey, 'buyer', 'e'.repeat(64))).status, 200, 'new PC can bind');
  const info = await (await admin('/admin/key-info', {id})).json();
  assert.equal(info.expires_at, before);
});

test('rotate refuses revoked and removed keys', async () => {
  const {id} = await (await admin('/admin/keys', {username: 'buyer', duration: 'month'})).json();
  await admin('/admin/remove', {id});
  assert.equal((await admin('/admin/rotate', {id})).status, 404);
  const info = await (await admin('/admin/key-info', {id})).json();
  assert.equal(info.active, false);
  assert.equal(info.removed, 1);
});

test('key-info and account-exists answer for unknown values', async () => {
  assert.equal((await admin('/admin/key-info', {id: crypto.randomUUID()})).status, 404);
  assert.deepEqual(await (await admin('/admin/account-exists', {username: 'nobody'})).json(), {exists: false});
  await admin('/admin/keys', {username: 'somebody', duration: 'week'});
  assert.deepEqual(await (await admin('/admin/account-exists', {username: 'somebody'})).json(), {exists: true});
});

test('the new admin calls need the admin key', async () => {
  for (const path of ['/admin/rotate', '/admin/key-info', '/admin/account-exists']) {
    const r = await betaRoutes(new Request('https://x.test' + path, {method: 'POST', body: '{}',
      headers: {Authorization: `Bearer ${'a'.repeat(64)}`, 'X-Admin-Username': 'developer'}}), env);
    assert.equal(r.status, 401, path);
  }
});
