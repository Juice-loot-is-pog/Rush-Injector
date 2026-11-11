import {test} from 'node:test';
import assert from 'node:assert/strict';
import {betaRoutes,activeBetaKey} from './beta.mjs';
const key='a'.repeat(64);
const hash=Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',new TextEncoder().encode(key))),b=>b.toString(16).padStart(2,'0')).join('');
const request=(path,method='GET',body,admin=true)=>new Request('https://example.test'+path,{method,headers:{Authorization:`Bearer ${key}`,'X-Device-Hash':'b'.repeat(64),'X-Admin-Username':admin?'developer':'wrong'},body:body?JSON.stringify(body):undefined});
function fixture() {
  const calls=[];
  const db={prepare(sql){return {bind(...args){calls.push({sql,args});return this;},async first(){return null;},async all(){return {results:[]};},async run(){return {success:true};}};},async batch(statements){return statements.map(()=>({success:true}));}};
  return {calls,env:{BETA_DB:db,ADMIN_USERNAME:'developer',ADMIN_KEY_HASH:hash}};
}
test('admin rejects incorrect username before any database operation',async()=>{
  const {calls,env}=fixture(); assert.equal((await betaRoutes(request('/admin/keys','GET',null,false),env)).status,401); assert.equal(calls.length,0);
});
test('key creation returns a unique secret but stores only its hash',async()=>{
  const {calls,env}=fixture();
  const response=await betaRoutes(request('/admin/keys','POST',{username:'tester',duration:'week'}),env);
  assert.equal(response.status,201); const data=await response.json(); assert.match(data.key,/^[a-f0-9]{64}$/);
  const insert=calls.find(c=>c.sql.startsWith('INSERT INTO beta_keys'));
  assert.ok(insert); assert.notEqual(insert.args[1],data.key); assert.equal(insert.args[4]-insert.args[3],7*86400000);
});
test('invalid duration never creates keys',async()=>{
  const {calls,env}=fixture(); assert.equal((await betaRoutes(request('/admin/keys','POST',{username:'tester',duration:'custom',seconds:-1}),env)).status,400); assert.equal(calls.length,0);
});
test('access query enforces account, expiry and revocation',async()=>{
  const {calls,env}=fixture(); assert.equal(await activeBetaKey(request('/client'),env),null);
  assert.match(calls[0].sql,/revoked_at IS NULL/); assert.match(calls[0].sql,/expires_at>\?/); assert.match(calls[0].sql,/status='active'/);
});
test('beta endpoints reject invalid keys and plaintext transport',async()=>{
  const {env}=fixture(); assert.equal((await betaRoutes(request('/beta/status'),env)).status,401);
  assert.equal((await betaRoutes(new Request('http://example.test/admin/keys'),env)).status,400);
});
test('injection reports require valid credentials and an event ID',async()=>{
  const {env,calls}=fixture();env.DOWNLOAD_KEY_HASHES=JSON.stringify([hash]);
  assert.equal((await betaRoutes(request('/injection','POST',{}),env)).status,400);
  const report=request('/injection','POST',{});report.headers.set('Idempotency-Key',crypto.randomUUID());
  assert.equal((await betaRoutes(report,env)).status,200);
  assert.match(calls.at(-1).sql,/INSERT OR IGNORE INTO client_reports/);
  assert.equal(calls.at(-1).args[1],'alpha');
});

test('exposed shared Alpha key cannot authorize injection reports',async()=>{
  const {env,calls}=fixture();
  const exposed='2RKwmq4GINmCHfBGtzc78gHKYrxajQJyGtcRjUUsp4I';
  env.DOWNLOAD_KEY_HASHES=JSON.stringify([await crypto.subtle.digest('SHA-256',
    new TextEncoder().encode(exposed)).then(bytes=>Array.from(new Uint8Array(bytes),b=>b.toString(16).padStart(2,'0')).join(''))]);
  const report=request('/injection','POST',{});
  report.headers.set('Authorization','Bearer '+exposed);
  report.headers.set('Idempotency-Key',crypto.randomUUID());
  assert.equal((await betaRoutes(report,env)).status,401);
  assert.ok(calls.every(call=>!call.sql.startsWith('INSERT')));
});
test('beta success reports are attributed to the authenticated key atomically',async()=>{
  const {env,calls}=fixture();
  const prepare=env.BETA_DB.prepare;
  env.BETA_DB.prepare=sql=>{
    const statement=prepare(sql);
    statement.first=async()=>({id:'verified-key'});
    return statement;
  };
  const report=request('/injection','POST',{});
  const id=crypto.randomUUID();report.headers.set('Idempotency-Key',id);
  assert.equal((await betaRoutes(report,env)).status,200);
  const event=calls.find(c=>c.sql.startsWith('INSERT OR IGNORE INTO injection_events'));
  assert.deepEqual(event.args.slice(0,2),[id,'verified-key']);
  assert.equal(calls.find(c=>c.sql.startsWith('INSERT OR IGNORE INTO client_reports')).args[1],'beta');
});
test('key listing returns per-key injection totals',async()=>{
  const {env}=fixture();
  const prepare=env.BETA_DB.prepare;
  env.BETA_DB.prepare=sql=>{
    assert.match(sql,/COUNT\(\*\) FROM injection_events e WHERE e.key_id=k.id/);
    assert.match(sql,/NOT EXISTS\(SELECT 1 FROM removed_beta_keys/);
    const statement=prepare(sql);
    statement.all=async()=>({results:[{id:'key-one',injection_count:7},{id:'key-two',injection_count:0}]});
    return statement;
  };
  const result=await (await betaRoutes(request('/admin/keys'),env)).json();
  assert.deepEqual(result.keys.map(k=>k.injection_count),[7,0]);
});
test('removing a key revokes and hides it while preserving injection events',async()=>{
  const {env,calls}=fixture();
  const id=crypto.randomUUID();
  assert.equal((await betaRoutes(request('/admin/remove','POST',{id}),env)).status,200);
  assert.match(calls[0].sql,/UPDATE beta_keys SET revoked_at/);
  assert.match(calls[1].sql,/INSERT OR IGNORE INTO removed_beta_keys/);
  assert.ok(calls.every(c=>!c.sql.includes('DELETE')));
  assert.equal((await betaRoutes(request('/admin/remove','POST',{id:'invalid'}),env)).status,400);
});
test('adding key time validates duration and preserves revoked and removed state',async()=>{
  const {env,calls}=fixture();
  const prepare=env.BETA_DB.prepare;
  env.BETA_DB.prepare=sql=>{const statement=prepare(sql);statement.run=async()=>({meta:{changes:1}});return statement;};
  const id=crypto.randomUUID();
  assert.equal((await betaRoutes(request('/admin/extend','POST',{id,seconds:0}),env)).status,400);
  assert.equal(calls.length,0);
  assert.equal((await betaRoutes(request('/admin/extend','POST',{id,seconds:86400}),env)).status,200);
  assert.match(calls[0].sql,/MAX\(expires_at,\?\)\+\?/);
  assert.match(calls[0].sql,/revoked_at IS NULL/);
  assert.match(calls[0].sql,/NOT EXISTS/);
  assert.equal(calls[0].args[1],86400000);
  env.BETA_DB.prepare=sql=>{const statement=prepare(sql);statement.run=async()=>({meta:{changes:0}});return statement;};
  assert.equal((await betaRoutes(request('/admin/extend','POST',{id,seconds:86400}),env)).status,404);
});
