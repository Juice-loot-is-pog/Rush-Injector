import {validAlphaKey} from './download_auth.mjs';
import {patreonKeyMessage} from './patreon.mjs';
const encode = new TextEncoder();
const digest = async value => Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256', encode.encode(value))), b => b.toString(16).padStart(2,'0')).join('');
const json = (value, status = 200) => Response.json(value, {status, headers:{'Cache-Control':'no-store','X-Content-Type-Options':'nosniff'}});
const bearer = request => /^Bearer ([A-Za-z0-9_-]{32,128})$/.exec(request.headers.get('Authorization') || '')?.[1];
const randomKey = () => Array.from(crypto.getRandomValues(new Uint8Array(32)), b => b.toString(16).padStart(2,'0')).join('');
const usernameValid = value => typeof value === 'string' && /^[A-Za-z0-9_.-]{1,48}$/.test(value);
export async function activeBetaKey(request, env) {
  const key = bearer(request);
  const device=request.headers.get('X-Device-Hash');
  if (!key || !env.BETA_DB || !/^[a-f0-9]{64}$/.test(device||'')) return null;
  return env.BETA_DB.prepare(`SELECT k.id,k.username,k.expires_at FROM beta_keys k
    JOIN beta_accounts a ON a.username=k.username WHERE k.key_hash=? AND k.revoked_at IS NULL
    AND k.expires_at>? AND a.status='active'
    AND EXISTS(SELECT 1 FROM beta_devices d WHERE d.key_id=k.id AND d.device_hash=?)`).bind(await digest(key),Date.now(),device).first();
}
export async function betaRoutes(request, env) {
  const path = new URL(request.url).pathname;
  if(path==='/beta/client') return null;
  if (!path.startsWith('/admin/') && !path.startsWith('/beta/') && path!=='/injection') return null;
  if (new URL(request.url).protocol !== 'https:') return json({error:'HTTPS required'},400);
  if (!env.BETA_DB) return json({error:'Beta storage not configured'},503);
  try {
    if(path==='/beta/login' && request.method==='POST') {
      const key=bearer(request),username=request.headers.get('X-Beta-Username'),device=request.headers.get('X-Device-Hash');
      if(!key || !usernameValid(username) || !/^[a-f0-9]{64}$/.test(device||'')) return json({error:'Valid username, secret beta key and device identity required'},400);
      const account=await env.BETA_DB.prepare(`SELECT k.id FROM beta_keys k JOIN beta_accounts a ON a.username=k.username
        WHERE k.key_hash=? AND k.username=? AND k.revoked_at IS NULL AND k.expires_at>? AND a.status='active'`).bind(await digest(key),username,Date.now()).first();
      if(!account) return json({error:await patreonKeyMessage(env,key) || 'Username or key rejected, expired or suspended'},401);
      await env.BETA_DB.prepare('INSERT OR IGNORE INTO beta_devices(key_id,device_hash,bound_at) VALUES(?,?,?)').bind(account.id,device,Date.now()).run();
      const verified=await activeBetaKey(request,env);
      if(!verified) return json({error:'This key is bound to another device. Ask a developer to reset it.'},403);
      return json({...verified,status:'active',time_left_seconds:Math.max(0,Math.floor((verified.expires_at-Date.now())/1000))});
    }
    if(path==='/injection') {
      if(request.method!=='POST') return json({error:'Use POST'},405);
      const key=bearer(request); if(!key) return json({error:'Access denied'},401);
      const alpha=await validAlphaKey(key,env.DOWNLOAD_KEY_HASHES);
      const beta=alpha?null:await activeBetaKey(request,env);
      if(!alpha && !beta) return json({error:'Access denied'},401);
      const id=request.headers.get('Idempotency-Key');
      if(!id || !/^[a-f0-9-]{36}$/i.test(id)) return json({error:'Event UUID required'},400);
      const now=Date.now();
      if(beta) {
        await env.BETA_DB.batch([
          env.BETA_DB.prepare('INSERT OR IGNORE INTO client_reports(event_id,channel,reported_at) VALUES(?,?,?)').bind(id,'beta',now),
          env.BETA_DB.prepare('INSERT OR IGNORE INTO injection_events(event_id,key_id,reported_at) VALUES(?,?,?)').bind(id,beta.id,now)
        ]);
      } else {
        await env.BETA_DB.prepare('INSERT OR IGNORE INTO client_reports(event_id,channel,reported_at) VALUES(?,?,?)').bind(id,'alpha',now).run();
      }
      return json({ok:true});
    }
    if (path.startsWith('/admin/')) {
      // A separate high-entropy administrator credential is never shipped to clients.
      const key = bearer(request);
      if (!env.ADMIN_KEY_HASH || !env.ADMIN_USERNAME) return json({error:'Admin access not configured'},503);
      if (!key || request.headers.get('X-Admin-Username') !== env.ADMIN_USERNAME || await digest(key) !== env.ADMIN_KEY_HASH)
        return json({error:'Access denied'},401);
      if (path === '/admin/keys' && request.method === 'GET') {
        const now=Date.now();
        const {results} = await env.BETA_DB.prepare(`SELECT k.id,k.username,k.created_at,k.expires_at,k.revoked_at,a.status AS account_status,
          EXISTS(SELECT 1 FROM beta_devices d WHERE d.key_id=k.id) AS device_bound,
          (SELECT COUNT(*) FROM injection_events e WHERE e.key_id=k.id) AS injection_count
          FROM beta_keys k JOIN beta_accounts a ON a.username=k.username
          WHERE NOT EXISTS(SELECT 1 FROM removed_beta_keys r WHERE r.key_id=k.id)
          ORDER BY k.created_at DESC LIMIT 1000`).all();
        return json({keys:results.map(k=>({...k,time_left_seconds:Math.max(0,Math.floor((k.expires_at-now)/1000)),
          active:!k.revoked_at && k.expires_at>now && k.account_status==='active'})),limit:1000});
      }
      if (path === '/admin/stats' && request.method === 'GET') {
        const stats=await env.BETA_DB.prepare(`SELECT COUNT(*) AS all_time,
          COALESCE(SUM(CASE WHEN reported_at>=? THEN 1 ELSE 0 END),0) AS last_24h,
          COALESCE(SUM(CASE WHEN channel='beta' THEN 1 ELSE 0 END),0) AS beta_all_time,
          COALESCE(SUM(CASE WHEN channel='beta' AND reported_at>=? THEN 1 ELSE 0 END),0) AS beta_last_24h FROM client_reports`).bind(Date.now()-86400000,Date.now()-86400000).first();
        return json({...stats,measurement:'Client-reported Auto Inject successes; older injectors and manual injections are not counted.'});
      }
      if (request.method !== 'POST') return json({error:'Unknown endpoint or method'},404);
      if (Number(request.headers.get('Content-Length'))>4096) return json({error:'Request too large'},413);
      const raw=await request.text(); if (raw.length>4096) return json({error:'Request too large'},413);
      let data; try { data=JSON.parse(raw); } catch { return json({error:'Invalid JSON'},400); }
      if (path === '/admin/keys') {
        const days={day:1,week:7,month:30};
        const seconds=data.duration==='custom' ? data.seconds : days[data.duration]*86400;
        if (!usernameValid(data.username) || !Number.isSafeInteger(seconds) || seconds<60 || seconds>31536000)
          return json({error:'Use a valid username and day/week/month or custom seconds (60–31536000)'},400);
        const key=randomKey(), id=crypto.randomUUID(), now=Date.now(), expires=now+seconds*1000;
        await env.BETA_DB.batch([
          env.BETA_DB.prepare('INSERT OR IGNORE INTO beta_accounts(username,created_at) VALUES(?,?)').bind(data.username,now),
          env.BETA_DB.prepare('INSERT INTO beta_keys(id,key_hash,username,created_at,expires_at) VALUES(?,?,?,?,?)').bind(id,await digest(key),data.username,now,expires)
        ]);
        return json({id,username:data.username,key,expires_at:expires,notice:'Save this key now; it is shown once.'},201);
      }
      if (path === '/admin/revoke' && typeof data.id==='string') {
        await env.BETA_DB.prepare('UPDATE beta_keys SET revoked_at=? WHERE id=?').bind(Date.now(),data.id).run();
        return json({ok:true});
      }
      if(path==='/admin/remove' && typeof data.id==='string' && /^[a-f0-9-]{36}$/i.test(data.id)) {
        await env.BETA_DB.batch([
          env.BETA_DB.prepare('UPDATE beta_keys SET revoked_at=COALESCE(revoked_at,?) WHERE id=?').bind(Date.now(),data.id),
          env.BETA_DB.prepare('INSERT OR IGNORE INTO removed_beta_keys(key_id) SELECT id FROM beta_keys WHERE id=?').bind(data.id)
        ]);
        return json({ok:true});
      }
      if(path==='/admin/extend' && typeof data.id==='string' && /^[a-f0-9-]{36}$/i.test(data.id)) {
        if(!Number.isSafeInteger(data.seconds) || data.seconds<60 || data.seconds>31536000)
          return json({error:'Added time must be 60 to 31536000 seconds'},400);
        const result=await env.BETA_DB.prepare(`UPDATE beta_keys SET expires_at=MAX(expires_at,?)+?
          WHERE id=? AND revoked_at IS NULL
          AND NOT EXISTS(SELECT 1 FROM removed_beta_keys r WHERE r.key_id=beta_keys.id)`).bind(Date.now(),data.seconds*1000,data.id).run();
        if(!result.meta?.changes) return json({error:'Key not found or revoked'},404);
        return json({ok:true});
      }
      if(path==='/admin/reset-device' && typeof data.id==='string') {
        await env.BETA_DB.prepare('DELETE FROM beta_devices WHERE key_id=?').bind(data.id).run();
        return json({ok:true});
      }
      if (path === '/admin/account' && usernameValid(data.username) && ['active','suspended'].includes(data.status)) {
        await env.BETA_DB.prepare('UPDATE beta_accounts SET status=? WHERE username=?').bind(data.status,data.username).run();
        return json({ok:true});
      }
      return json({error:'Unknown endpoint or invalid input'},400);
    }
    const account=await activeBetaKey(request,env);
    if (!account) return json({error:await patreonKeyMessage(env,bearer(request)) || 'Beta key expired, revoked, suspended, or invalid'},401);
    if (path==='/beta/status' && request.method==='GET') return json({...account,status:'active',time_left_seconds:Math.max(0,Math.floor((account.expires_at-Date.now())/1000))});
    if (path==='/beta/injection' && request.method==='POST') {
      const id=request.headers.get('Idempotency-Key');
      if (!id || !/^[a-f0-9-]{36}$/i.test(id)) return json({error:'A UUID Idempotency-Key is required'},400);
      await env.BETA_DB.prepare('INSERT OR IGNORE INTO injection_events(event_id,key_id,reported_at) VALUES(?,?,?)').bind(id,account.id,Date.now()).run();
      return json({ok:true});
    }
    return json({error:'Unknown endpoint or method'},404);
  } catch { return json({error:'Beta service request failed'},503); }
}
