import { randomBytes, createHash } from 'node:crypto';

const key = randomBytes(32).toString('base64url');
const hash = createHash('sha256').update(key, 'utf8').digest('hex');

console.log(`User key: ${key}`);
console.log(`Server hash: ${hash}`);
console.log(`Add to DOWNLOAD_KEY_HASHES: ["${hash}"]`);
