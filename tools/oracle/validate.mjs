// validate.mjs -- asks the TypeScript engine's own command validation whether it accepts actions our engine lists
// but its (partly sampled) legalActions does not. Input: positions.jsonl and a JSON file
// [{i: position index, key, tries: [action, ...]}]; an entry passes if any of its tries is accepted.
// Usage: npx vite-node validate.mjs positions.jsonl queries.json out.json
import fs from 'node:fs';
const ROOT = '/Users/philipsiler/Code/WOTR-RL/war-of-the-ring/';
const { wotrAdapter } = await import(ROOT + 'src/adapter/wotrAdapter.ts');
const P = fs.readFileSync(process.argv[2], 'utf8').split('\n').filter(Boolean);
const Q = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
const cache = new Map();
const out = Q.map((q) => {
  let p = cache.get(q.i);
  if (!p) { p = JSON.parse(P[q.i]); p.state.log = []; cache.clear(); cache.set(q.i, p); }
  let reason = 'no translation';
  for (const a of q.tries) {
    const r = wotrAdapter.tryApplyAction(p.state, a, p.actor);
    if (r.ok) return { ok: true };
    reason = r.reason;
  }
  return { ok: false, reason };
});
fs.writeFileSync(process.argv[4], JSON.stringify(out));
