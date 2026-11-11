import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createWorker, sha256} from './worker.mjs';
const key = 'fixture_user_key_abcdefghijklmnopqrstuvwxyz0123456789';
const bytes = new Uint8Array(128); const view = new DataView(bytes.buffer);
view.setUint16(0,0x5a4d,true); view.setUint32(0x3c,64,true); view.setUint32(64,0x4550,true);
view.setUint16(68,0x8664,true); view.setUint16(86,0x2000,true);
const env = {GITHUB_TOKEN:'fixture-server-only-token',DOWNLOAD_KEY_HASHES:JSON.stringify([await sha256(new TextEncoder().encode(key))])};
const req = (token = key) => new Request('https://fixture.example/client', {headers:{Authorization:`Bearer ${token}`}});
test('public Alpha GET and HEAD need no key, including cached responses and stale credentials',async()=>{
  let calls=0;
  const worker=createWorker(async()=>{++calls;return new Response(bytes,{headers:{ETag:'"v1"'}});});
  const exposed='2RKwmq4GINmCHfBGtzc78gHKYrxajQJyGtcRjUUsp4I';
  const hash=await sha256(new TextEncoder().encode(key));
  const exposedHash=await sha256(new TextEncoder().encode(exposed));
  const configured={...env,DOWNLOAD_KEY_HASHES:JSON.stringify([hash,exposedHash])};
  for(const method of ['GET','HEAD']) {
    assert.equal((await worker.fetch(new Request(req(),{method}),configured)).status,200);
    const before=calls;
    for(const value of [null,'Bearer wrong','Bearer '+crypto.randomUUID(),'Bearer '+hash,
        'Bearer '+exposed,'Bearer '+key+', Bearer '+key]) {
      const headers={'X-Device-Hash':'a'.repeat(64)};
      if(value!==null) headers.Authorization=value;
      const response=await worker.fetch(new Request('https://fixture.example/client',{method,headers}),configured);
      assert.equal(response.status,200);
      assert.equal(response.headers.get('X-Content-SHA256'),await sha256(bytes));
    }
    assert.equal(calls,before+6); // Known version: one streamed upstream call each.
  }
});

test('beta GET and HEAD require secret and matching bound device even with warm cache',async()=>{
  let calls=0;
  const device='a'.repeat(64),hash=await sha256(new TextEncoder().encode(key));
  let allowed=true;
  const betaEnv={...env,GITHUB_BETA_PATH:'beta.dll',BETA_DB:{prepare(){return {
    bind(...args){this.args=args;return this;},
    async first(){return allowed && this.args[0]===hash && this.args[2]===device ? {id:'verified'} : null;}
  };}}};
  const worker=createWorker(async()=>{++calls;return new Response(bytes);});
  for(const method of ['GET','HEAD']) {
    const request=(token,identity)=>new Request('https://fixture.example/beta/client',{method,
      headers:{...(token?{Authorization:'Bearer '+token}:{}),...(identity?{'X-Device-Hash':identity}:{})}});
    allowed=true;
    assert.equal((await worker.fetch(request(key,device),betaEnv)).status,200);
    const before=calls;
    for(const [token,identity] of [[null,device],[hash,device],[crypto.randomUUID(),device],[key,null],[key,'b'.repeat(64)]])
      assert.equal((await worker.fetch(request(token,identity),betaEnv)).status,401);
    allowed=false;
    assert.equal((await worker.fetch(request(key,device),betaEnv)).status,401);
    assert.equal(calls,before);
  }
});

test('beta channel rejects alpha credentials and pins its separate artifact',async()=>{
  let valid=false,calls=0;
  const betaEnv={...env,GITHUB_BETA_PATH:'RushClient_Beta_v1_MC26.60.dll',BETA_DB:{prepare(){return {bind(){return this;},async first(){return valid?{id:'beta-key'}:null;}};}}};
  const worker=createWorker(async(url)=>{++calls;assert.match(url,/contents\/RushClient_Beta_v1_MC26.60.dll/);return new Response(bytes,{headers:{ETag:'"b1"'}});});
  const request=()=>new Request('https://fixture.example/beta/client',{headers:{Authorization:`Bearer ${key}`,'X-Device-Hash':'a'.repeat(64)}});
  assert.equal((await worker.fetch(request(),betaEnv)).status,401);assert.equal(calls,0);
  valid=true;assert.equal((await worker.fetch(request(),betaEnv)).status,200);assert.equal(calls,2);
  valid=false;assert.equal((await worker.fetch(request(),betaEnv)).status,401);assert.equal(calls,2);
});
test('rejects unauthenticated Beta callers before contacting GitHub',async()=>{
  const worker=createWorker(()=>{throw Error('must not fetch');});
  assert.equal((await worker.fetch(new Request('https://fixture.example/beta/client'),env)).status,401);
});
test('public Alpha HEAD returns digest without downloading the body',async()=>{
  const worker=createWorker(async()=>new Response(bytes));
  const head = token => new Request('https://fixture.example/client',{method:'HEAD',headers:{Authorization:`Bearer ${token}`}});
  assert.equal((await worker.fetch(new Request('https://fixture.example/client',{method:'HEAD'}),env)).status,200);
  const response=await worker.fetch(head(key),env);
  assert.equal(response.status,200);
  assert.equal(response.headers.get('X-Content-SHA256'),await sha256(bytes));
  assert.equal((await response.arrayBuffer()).byteLength,0);
});
test('public Alpha downloads the pinned repository file and reuses its cache',async()=>{
  let calls=0;
  const worker=createWorker(async(url,options)=>{
    ++calls; assert.match(url,/Juice-loot-is-pog\/Rush-Dll\/contents\/RushClient_Alpha_v3.0.dll/);
    assert.equal(options.headers.Authorization,`Bearer ${env.GITHUB_TOKEN}`); assert.equal(options.redirect,'manual');
    return new Response(bytes,{headers:{ETag:'"v1"'}});
  });
  const response=await worker.fetch(req(),env); assert.equal(response.status,200);
  assert.equal(response.headers.get('X-Content-SHA256'),await sha256(bytes));
  assert.deepEqual(new Uint8Array(await response.arrayBuffer()),bytes);
  assert.equal(calls,2);
  assert.equal((await worker.fetch(req(),env)).status,200); assert.equal(calls,3);
  assert.equal((await worker.fetch(new Request('https://fixture.example/client'),{GITHUB_TOKEN:env.GITHUB_TOKEN})).status,200);
});
test('does not disclose GitHub errors or follow redirects',async()=>{
  const worker=createWorker(async()=>new Response(env.GITHUB_TOKEN,{status:302,headers:{Location:'https://elsewhere.example'}}));
  const response=await worker.fetch(req(),env); assert.equal(response.status,502);
  assert.equal(response.headers.get('X-Rush-Error'),'source-unavailable');
  assert.ok(!(await response.text()).includes(env.GITHUB_TOKEN));
});
test('rejects a non-DLL response and oversized content',async()=>{
  assert.equal((await createWorker(async()=>new Response('html')).fetch(req(),env)).status,502);
  assert.equal((await createWorker(async()=>new Response(bytes,{headers:{'Content-Length':String(33*1024*1024)}})).fetch(req(),env)).status,502);
});
test('rejects missing configuration, unknown paths, and plaintext HTTP',async()=>{
  const worker=createWorker(); assert.equal((await worker.fetch(req(),{})).status,503);
  assert.equal((await worker.fetch(req(),{})).headers.get('X-Rush-Error'),'configuration');
  assert.equal((await worker.fetch(new Request('https://fixture.example/other'),env)).status,404);
  assert.equal((await worker.fetch(new Request('http://fixture.example/client'),env)).status,400);
});

const anonymous = () => new Request('https://fixture.example/client');
const versioned = state => async () => new Response(state.bytes,{headers:{ETag:`"${state.tag}"`,'Content-Length':String(state.bytes.length)}});

test('a known version streams with one upstream call and no re-hash',async()=>{
  const state={bytes,tag:'v1'}; let calls=0; const upstream=versioned(state);
  const worker=createWorker(async(...args)=>{++calls;return upstream(...args);});
  assert.equal((await worker.fetch(anonymous(),env)).status,200); assert.equal(calls,2);
  const responses=await Promise.all(Array.from({length:6},()=>worker.fetch(anonymous(),env)));
  assert.deepEqual(responses.map(response=>response.status),Array(6).fill(200));
  for(const response of responses) assert.deepEqual(new Uint8Array(await response.arrayBuffer()),bytes);
  assert.equal(calls,8);
});

test('HEAD of an unknown version hashes once and returns no body',async()=>{
  const state={bytes,tag:'v1'}; let calls=0; const upstream=versioned(state);
  const worker=createWorker(async(...args)=>{++calls;return upstream(...args);});
  const response=await worker.fetch(new Request('https://fixture.example/client',{method:'HEAD'}),env);
  assert.equal(response.headers.get('X-Content-SHA256'),await sha256(bytes));
  assert.equal(response.headers.get('Content-Length'),String(bytes.length));
  assert.equal(calls,1);
});

test('changed upstream is hashed again and served with its own checksum',async()=>{
  const changed=bytes.slice(); changed[100]=7;
  const state={bytes,tag:'v1'}; const worker=createWorker(versioned(state));
  assert.equal((await worker.fetch(anonymous(),env)).headers.get('X-Content-SHA256'),await sha256(bytes));
  state.bytes=changed; state.tag='v2';
  const response=await worker.fetch(anonymous(),env);
  assert.equal(response.headers.get('X-Content-SHA256'),await sha256(changed));
  assert.deepEqual(new Uint8Array(await response.arrayBuffer()),changed);
});

test('an upload between the hashing and serving passes is hashed before serving',async()=>{
  const changed=bytes.slice(); changed[100]=7; let calls=0;
  const worker=createWorker(async()=>{++calls;
    return new Response(calls===1?bytes:changed,{headers:{ETag:calls===1?'"v1"':'"v2"'}});});
  const response=await worker.fetch(anonymous(),env);
  assert.equal(response.headers.get('X-Content-SHA256'),await sha256(changed));
  assert.deepEqual(new Uint8Array(await response.arrayBuffer()),changed);
});

test('a source that never settles fails cleanly instead of serving a mismatched checksum',async()=>{
  let calls=0;
  const worker=createWorker(async()=>new Response(bytes,{headers:{ETag:`"v${++calls}"`}}));
  const response=await worker.fetch(anonymous(),env);
  assert.equal(response.status,502); assert.equal(response.headers.get('X-Rush-Error'),'upstream-failure');
});

test('an untagged upstream is hashed per request and still served correctly',async()=>{
  let calls=0;
  const worker=createWorker(async()=>{++calls;return new Response(bytes);});
  const response=await worker.fetch(anonymous(),env);
  assert.equal(response.headers.get('X-Content-SHA256'),await sha256(bytes));
  assert.deepEqual(new Uint8Array(await response.arrayBuffer()),bytes); assert.equal(calls,2);
});

test('a failed upstream answer is not remembered',async()=>{
  let calls=0;
  const worker=createWorker(async()=>++calls===1?new Response('gone',{status:500}):new Response(bytes,{headers:{ETag:'"v1"'}}));
  assert.equal((await worker.fetch(anonymous(),env)).status,502);
  assert.equal((await worker.fetch(anonymous(),env)).status,200);
});

test('declared and undeclared lengths both produce the exact client',async()=>{
  const large=new Uint8Array(3*1024*1024); large.set(bytes); large[large.length-1]=9;
  for(const headers of [{'Content-Length':String(large.length)},{}]) {
    const worker=createWorker(async()=>new Response(new ReadableStream({start(controller){
      for(let offset=0;offset<large.length;offset+=700000) controller.enqueue(large.subarray(offset,offset+700000));
      controller.close();
    }}),{headers}));
    const response=await worker.fetch(anonymous(),env);
    assert.equal(response.status,200);
    assert.equal(response.headers.get('X-Content-SHA256'),await sha256(large));
  }
});
