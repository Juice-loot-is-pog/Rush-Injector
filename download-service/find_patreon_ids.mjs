// Prints your Patreon campaign id and each tier's id, for wrangler.toml.
// Asks for the Creator's Access Token (from the Patreon client page) without echoing it,
// sends it only to patreon.com, and saves nothing.
//
//   node find_patreon_ids.mjs

import {createInterface} from 'node:readline';

function askHidden(question) {
  return new Promise(resolve => {
    const rl = createInterface({input: process.stdin, output: process.stdout, terminal: true});
    rl._writeToOutput = text => { if (text.includes(question)) process.stdout.write(text); };
    rl.question(question, answer => { rl.close(); process.stdout.write('\n'); resolve(answer.trim()); });
  });
}

const token = await askHidden("Paste the Creator's Access Token, then press Enter (it won't show): ");
if (!token) { console.error('No token entered.'); process.exit(1); }

const url = new URL('https://www.patreon.com/api/oauth2/v2/campaigns');
url.search = new URLSearchParams({
  include: 'tiers',
  'fields[campaign]': 'vanity,creation_name,patron_count',
  'fields[tier]': 'title,amount_cents,published',
}).toString();

const response = await fetch(url, {headers: {Authorization: `Bearer ${token}`, 'User-Agent': 'Rush-Setup'}});
if (!response.ok) {
  console.error(`Patreon said ${response.status}. ${response.status === 401
    ? "The token wasn't accepted. Copy the Creator's Access Token again from the client page."
    : 'Try again in a minute.'}`);
  process.exit(1);
}
const body = await response.json();
const tiers = new Map((body.included || []).filter(x => x.type === 'tier').map(t => [t.id, t.attributes || {}]));

if (!body.data?.length) { console.log('No campaign found on this account.'); process.exit(0); }
for (const campaign of body.data) {
  const a = campaign.attributes || {};
  console.log(`\nCampaign: ${a.vanity || a.creation_name || '(no name)'}`);
  console.log(`  PATREON_CAMPAIGN_ID = "${campaign.id}"`);
  const ids = (campaign.relationships?.tiers?.data || []).map(t => t.id);
  if (!ids.length) console.log('  No tiers yet.');
  else console.log('  Tiers:');
  for (const id of ids) {
    const t = tiers.get(id) || {};
    const price = typeof t.amount_cents === 'number' ? `$${(t.amount_cents / 100).toFixed(2)}` : '';
    console.log(`    ${id.padEnd(10)} ${(t.title || '(untitled)').padEnd(30)} ${price}${t.published === false ? '  (unpublished)' : ''}`);
  }
}
console.log('\nTell Claude the campaign id and which tiers should include Beta. Do not share the token.');
