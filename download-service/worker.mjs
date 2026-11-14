import {betaRoutes, activeBetaKey} from './beta.mjs';
import {patreonRoutes} from './patreon.mjs';
const MAX_BYTES = 32 * 1024 * 1024;
const HEADER_BYTES = 4096;          // PE headers must sit inside the first 4 KiB.
const HEADERS_TIMEOUT_MS = 30000;   // Upstream must start answering within this.
const HASH_TIMEOUT_MS = 60000;      // A full hashing pass must finish within this.
const KNOWN_LIMIT = 16;             // Remembered (source, ETag) digests per isolate.
const hex = bytes => Array.from(new Uint8Array(bytes), b => b.toString(16).padStart(2, '0')).join('');
export const sha256 = async bytes => hex(await crypto.subtle.digest('SHA-256', bytes));
const failure = (status, message) => {
  const category = message.includes('configured') || message.includes('configuration') ? 'configuration' :
    message === 'Client source unavailable' ? 'source-unavailable' :
    message === 'Client source is not an x64 DLL' ? 'source-invalid' :
    message === 'Client file too large' ? 'source-too-large' :
    message === 'Client download failed' ? 'upstream-failure' :
    status === 401 ? 'access-rejected' : message === 'Beta access unavailable' ? 'beta-unavailable' : 'request-rejected';
  return new Response(message, {status, headers: {'Cache-Control':'no-store','X-Rush-Error':category}});
};
const validDll = (header, length) => {
  const view = new DataView(header.buffer, header.byteOffset, header.byteLength);
  const pe = header.length >= 64 ? view.getUint32(0x3c, true) : 0;
  return length >= 64 && view.getUint16(0, true) === 0x5a4d && pe >= 64 && pe <= length - 24 && pe + 24 <= header.length &&
    view.getUint32(pe, true) === 0x4550 && view.getUint16(pe + 4, true) === 0x8664 && !!(view.getUint16(pe + 22, true) & 0x2000);
};

// Hashes a body as it streams past without holding it. The client is ~24 MiB; buffering
// it (per channel, plus growth copies) pushed the isolate past its memory limit (Cloudflare 1102).
async function digestBody(body) {
  const hasher = typeof crypto.DigestStream === 'function' ? new crypto.DigestStream('SHA-256') : null;
  const writer = hasher?.getWriter(), chunks = [];
  const header = new Uint8Array(HEADER_BYTES); let headerLength = 0, length = 0;
  const reader = body.getReader();
  while (true) {
    const {done, value} = await reader.read(); if (done) break;
    if (length + value.length > MAX_BYTES) { await reader.cancel(); return {rejected: failure(502, 'Client file too large')}; }
    if (headerLength < HEADER_BYTES) {
      const take = value.subarray(0, HEADER_BYTES - headerLength);
      header.set(take, headerLength); headerLength += take.length;
    }
    length += value.length;
    if (writer) await writer.write(value); else chunks.push(value.slice());
  }
  if (!validDll(header.subarray(0, headerLength), length)) return {rejected: failure(502, 'Client source is not an x64 DLL')};
  if (writer) { await writer.close(); return {digest: hex(await hasher.digest), length}; }
  // Runtimes without DigestStream (local Node tests) only.
  const joined = new Uint8Array(length); let offset = 0;
  for (const chunk of chunks) { joined.set(chunk, offset); offset += chunk.length; }
  return {digest: await sha256(joined), length};
}

export function createWorker(fetchUpstream = fetch) {
  const known = new Map(); // `${source}\n${etag}` -> {digest, length}: a few bytes per version.
  const remember = (key, value) => {
    known.delete(key); known.set(key, value);
    while (known.size > KNOWN_LIMIT) known.delete(known.keys().next().value);
  };
  const open = async (source, env) => {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), HEADERS_TIMEOUT_MS);
    try {
      const upstream = await fetchUpstream(source, {
        headers: {Authorization: `Bearer ${env.GITHUB_TOKEN}`, Accept: 'application/vnd.github.raw+json',
          'User-Agent': 'Rush-Download-Service', 'X-GitHub-Api-Version': '2022-11-28'},
        redirect: 'manual', signal: controller.signal
      });
      // Only the headers are timed here: a served body may stream as long as the client needs.
      return {upstream, abort: () => controller.abort()};
    } finally { clearTimeout(timer); }
  };
  // Never forward credentials to a redirect destination or return upstream errors/secrets.
  const unusable = upstream => {
    if (upstream.status !== 200 || !upstream.body) return failure(502, 'Client source unavailable');
    if ((Number(upstream.headers.get('Content-Length')) || 0) > MAX_BYTES) return failure(502, 'Client file too large');
  };
  const discard = upstream => { upstream.body?.cancel().catch(() => {}); };

  return {
    async fetch(request, env) {
      const patreonResponse = await patreonRoutes(request,env);
      if (patreonResponse) return patreonResponse;
      const betaResponse = await betaRoutes(request,env);
      if (betaResponse) return betaResponse;
      const betaDownload=new URL(request.url).pathname==='/beta/client';
      if (new URL(request.url).pathname !== '/client' && !betaDownload) return failure(404, 'Not found');
      if (!['GET','HEAD'].includes(request.method)) return failure(405, 'Use GET or HEAD');
      if (new URL(request.url).protocol !== 'https:') return failure(400, 'HTTPS required');
      if (!env.GITHUB_TOKEN) return failure(503, 'Download service not configured');
      if (betaDownload) {
        try { if (!await activeBetaKey(request,env)) return failure(401, 'Download key rejected'); }
        catch { return failure(503, 'Beta access unavailable'); }
      }

      // The client chooses neither the repository nor its path.
      const repo = env.GITHUB_REPO || 'Juice-loot-is-pog/Rush-Dll';
      const path = betaDownload ? env.GITHUB_BETA_PATH : (env.GITHUB_PATH || 'RushClient_Alpha_v3.0.dll');
      if(!path) return failure(503,'Beta client not configured');
      const ref = env.GITHUB_REF || 'main';
      if (!/^[\w.-]+\/[\w.-]+$/.test(repo)) return failure(503, 'Invalid source configuration');
      const source = `https://api.github.com/repos/${repo}/contents/${path.split('/').map(encodeURIComponent).join('/')}?ref=${encodeURIComponent(ref)}`;
      const headers = info => ({
        'Content-Type': 'application/octet-stream', 'Content-Disposition': 'attachment; filename="RushClient.dll"',
        'Content-Length': String(info.length), 'X-Content-SHA256': info.digest,
        'Cache-Control': 'private, no-store', 'X-Content-Type-Options': 'nosniff'
      });
      try {
        let untagged = null; // Digest of an ETag-less body hashed earlier in this request.
        // Round 0 serves a known version or hashes an unknown one; round 1 serves what was
        // hashed; round 2 covers one upload landing between those two passes.
        for (let round = 0; round < 3; ++round) {
          const {upstream, abort} = await open(source, env);
          const rejected = unusable(upstream); if (rejected) { discard(upstream); return rejected; }
          const etag = upstream.headers.get('ETag');
          const declared = Number(upstream.headers.get('Content-Length')) || 0;
          const candidate = etag ? known.get(`${source}\n${etag}`) : untagged;
          const info = candidate && (!declared || declared === candidate.length) ? candidate : null;
          if (info) {
            if (request.method === 'HEAD') { discard(upstream); return new Response(null, {headers: headers(info)}); }
            // Stream straight through. FixedLengthStream fails a body whose length disagrees
            // with the checksum, so the client retries instead of saving a mismatched file.
            let body = upstream.body;
            if (typeof FixedLengthStream === 'function') {
              const fixed = new FixedLengthStream(info.length);
              upstream.body.pipeTo(fixed.writable).catch(() => {}); body = fixed.readable;
            }
            return new Response(body, {headers: headers(info)});
          }
          if (round === 2) { discard(upstream); break; }
          // Unknown version: hash this body as it streams past, then fetch again to serve it.
          const deadline = setTimeout(abort, HASH_TIMEOUT_MS);
          let result;
          try { result = await digestBody(upstream.body); } finally { clearTimeout(deadline); }
          if (result.rejected) return result.rejected;
          if (etag) remember(`${source}\n${etag}`, result); else untagged = result;
          if (request.method === 'HEAD') return new Response(null, {headers: headers(result)});
        }
        return failure(502, 'Client download failed');
      } catch { return failure(502, 'Client download failed'); }
    }
  };
}
export default createWorker();
