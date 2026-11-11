// This credential shipped in public injector builds and must never authorize again.
const exposedKeyHash = '3984422dc7286ec0b81561a1bbe27e46f4430aca288ee62d2ba8eb2a25a385b1';
export async function validAlphaKey(key, encodedHashes) {
  if (typeof key !== 'string' || !/^[A-Za-z0-9_-]{32,128}$/.test(key)) return false;
  let hashes;
  try { hashes = JSON.parse(encodedHashes); } catch { return false; }
  if (!Array.isArray(hashes) || !hashes.length ||
      hashes.some(hash => typeof hash !== 'string' || !/^[a-f0-9]{64}$/.test(hash))) return false;
  const bytes = await crypto.subtle.digest('SHA-256',new TextEncoder().encode(key));
  const hash = Array.from(new Uint8Array(bytes),b=>b.toString(16).padStart(2,'0')).join('');
  return hash !== exposedKeyHash && hashes.includes(hash);
}
