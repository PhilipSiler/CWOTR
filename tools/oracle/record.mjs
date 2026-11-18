// record.mjs -- plays games in the independent TypeScript engine (../../../war-of-the-ring) and writes, for a sample
// of top-level action decisions, the full position and that engine's legal actions (one JSON object per line).
// Usage: npx vite-node record.mjs <games> <first seed> <out.jsonl> [sample probability]
import fs from 'node:fs';
import { Rng } from 'digital-boardgame-framework';
const ROOT = '/Users/philipsiler/Code/WOTR-RL/war-of-the-ring/';
const { createGame } = await import(ROOT + 'src/engine/setup.ts');
const { wotrAdapter, startGame } = await import(ROOT + 'src/adapter/wotrAdapter.ts');
const { chooseAction } = await import(ROOT + 'src/ai/wotrAI.ts');
const [games, seed0, out, prob] = [Number(process.argv[2] ?? 10), Number(process.argv[3] ?? 1), process.argv[4] ?? 'positions.jsonl', Number(process.argv[5] ?? 0.25)];
const fd = fs.openSync(out, 'w');
let npos = 0, ndec = 0, crashed = 0;
for (let g = 0; g < games; g++) {
  const seed = seed0 + g;
  let state = startGame(createGame({ seed }));
  const ai = new Rng(seed * 1000 + 7);
  const prand = [0.15, 0.5, 1.0][g % 3];           // how often a uniformly random legal action is played
  for (let n = 0; n < 20000 && !state.winner; n++) {
    const actor = wotrAdapter.currentActor(state);
    if (!actor) break;
    const legal = wotrAdapter.legalActions(state, actor);
    if (!legal.length) break;
    ndec++;
    if (state.phase === 'actionResolution' && !state.pendingChoice && !state.pendingCombat && ai.next() < prob) {
      const { log, notices, lastBattle, recentBattles, ...st } = state; if (st.hunt) st.hunt = { ...st.hunt, draws: [] };        // rngState stays: validate.mjs re-applies actions
      fs.writeSync(fd, JSON.stringify({ seed, n, actor, state: st, legal }) + '\n');
      npos++;
    }
    let a;
    try { a = ai.next() < prand ? ai.pick(legal) : chooseAction(state, actor, legal, ai); } catch (e) { a = ai.pick(legal); }
    const r = wotrAdapter.tryApplyAction(state, a, actor);
    if (!r.ok) { const r2 = wotrAdapter.tryApplyAction(state, ai.pick(legal), actor); if (!r2.ok) { crashed++; break; } state = r2.state; }
    else state = r.state;
  }
}
fs.closeSync(fd);
console.log(`games ${games}, decisions ${ndec}, positions written ${npos}, games abandoned ${crashed}`);
