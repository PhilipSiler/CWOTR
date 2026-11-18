// battle.mjs -- fights set-piece battles in the TypeScript engine with fixed simple choices (no combat cards,
// regulars lost first, attacker presses on, defender stands, never extend a siege, always advance) and prints the
// odds, for comparison with our engine (tools/oracle/battles.py). Usage: npx vite-node battle.mjs scenarios.json out.json
import fs from 'node:fs';
const ROOT = '/Users/philipsiler/Code/WOTR-RL/war-of-the-ring/';
const { createGame } = await import(ROOT + 'src/engine/setup.ts');
const { wotrAdapter, startGame } = await import(ROOT + 'src/adapter/wotrAdapter.ts');
const SC = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const FPN = ['dwarves', 'elves', 'gondor', 'north', 'rohan'];
const count = (box, side) => !box ? 0 : Object.entries(box.units ?? {}).filter(([n]) => FPN.includes(n) === (side === 'fp')).reduce((a, [, u]) => a + u.regular + u.elite, 0);
const out = SC.map((sc) => {
  const base = startGame(createGame({ seed: 1 }));
  for (const r of Object.values(base.regions)) { r.units = {}; r.leaders = 0; r.nazgul = 0; r.characters = []; r.besieged = false; delete r.siegeBox; }
  for (const n of Object.values(base.nations)) { n.step = 0; n.active = true; }
  const def = sc.att === 'fp' ? 'shadow' : 'fp';
  const A = base.regions[sc.from], D = base.regions[sc.to];
  const put = (box, u, ldr, naz) => { for (const [n, [r, e]] of Object.entries(u)) box.units[n] = { regular: r, elite: e }; box.leaders = ldr ?? 0; box.nazgul = naz ?? 0; };
  if (sc.siege) { D.besieged = true; D.siegeBox = { units: {}, leaders: 0, nazgul: 0, characters: [] }; put(D.siegeBox, sc.defenders, sc.def_leaders, sc.def_nazgul); put(D, sc.attackers, sc.att_leaders, sc.att_nazgul); }
  else { put(A, sc.attackers, sc.att_leaders, sc.att_nazgul); put(D, sc.defenders, sc.def_leaders, sc.def_nazgul); }
  base.phase = 'actionResolution'; base.currentPlayer = sc.att; base.dice = { fp: [], shadow: [] }; base.dice[sc.att] = ['army', 'army'];
  base.cards.fp.hand = []; base.cards.shadow.hand = []; base.pendingChoice = null; base.pendingCombat = null; base.log = [];
  const a0 = count(sc.siege ? D : A, sc.att), d0 = count(sc.siege ? D.siegeBox : D, def), ctrl0 = D.control;
  let attw = 0, defw = 0, al = 0, dl = 0, cap = 0, sieged = 0, err = null, n = 0;
  for (let i = 0; i < sc.trials && !err; i++) {
    let s = JSON.parse(JSON.stringify(base)); s.rngState = (i + 1) * 7919 + 13;
    let r = wotrAdapter.tryApplyAction(s, { kind: 'attack', from: sc.from, to: sc.to }, sc.att);
    if (!r.ok) { err = r.reason; break; }
    s = r.state;
    for (let g = 0; g < 400 && (s.pendingChoice || s.pendingCombat) && !s.winner; g++) {
      const actor = wotrAdapter.currentActor(s); const L = wotrAdapter.legalActions(s, actor);
      const k = L[0].kind; let a = L[0];
      if (k === 'playCombatCard') a = L.find((x) => x.cardId === null);
      else if (k === 'casualtyStep') a = L.find((x) => x.step === 'removeRegular') ?? L.find((x) => x.step === 'reduceElite') ?? L[0];
      else if (k === 'combatContinue') a = L.find((x) => x.cont);
      else if (k === 'combatRetreat') a = L.find((x) => !x.retreat);
      else if (k === 'siegeWithdraw') a = L.find((x) => x.withdraw === !!sc.withdraw);
      else if (k === 'siegeExtend') a = L.find((x) => !x.extend);
      else if (k === 'advanceChoice' || k === 'besiegerAdvance' || k === 'relieveAdvance') a = L.find((x) => x.advance);
      else if (k === 'whiteRider') a = L.find((x) => !x.forfeit);
      const rr = wotrAdapter.tryApplyAction(s, a ?? L[0], actor);
      if (!rr.ok) { err = k + ': ' + rr.reason; break; }
      s = rr.state;
    }
    const A2 = s.regions[sc.from], D2 = s.regions[sc.to];
    const au = sc.siege ? count(D2, sc.att) : count(A2, sc.att) + count(D2, sc.att);
    const du = count(D2, def) + count(D2.siegeBox, def);
    attw += au === 0; defw += du === 0; al += a0 - au; dl += d0 - du; cap += D2.control !== ctrl0; sieged += !!D2.besieged && !sc.siege; n++;
  }
  return { name: sc.name, err, n, attw: attw / n, defw: defw / n, al: al / n, dl: dl / n, cap: cap / n, sieged: sieged / n };
});
fs.writeFileSync(process.argv[3], JSON.stringify(out));
