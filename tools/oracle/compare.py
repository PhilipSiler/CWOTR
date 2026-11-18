#!/usr/bin/env python3
"""compare.py -- differential test of legal move enumeration.

Reads positions recorded from the independent TypeScript engine (record.mjs), loads each into our engine with
bin/inject, and compares the two sets of legal top-level actions. Usage: compare.py positions.jsonl [examples per category]
"""
import json, os, subprocess, sys, collections
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.join(HERE, '..', '..'); A = os.path.join(ROOT, '..', 'war-of-the-ring', 'assets')
REGS = sorted(json.load(open(os.path.join(A, 'map.json')))['regions'].keys()); RI = {r: i for i, r in enumerate(REGS)}
CARDS = [c['id'] for c in json.load(open(os.path.join(A, 'event-cards.json')))['cards']]; CI = {c: i for i, c in enumerate(CARDS)}
NAT = ['dwarves', 'elves', 'gondor', 'north', 'rohan', 'isengard', 'sauron', 'southrons']; NI = {n: i for i, n in enumerate(NAT)}
FACE = ['character', 'army', 'muster', 'event', 'armyMuster']          # index 5: will (FP) / eye (Shadow)
COMP = ['gandalf', 'strider', 'boromir', 'legolas', 'gimli', 'meriadoc', 'peregrin']
CID = {'gandalf-grey': 0, 'gandalf-white': 0, 'strider': 1, 'aragorn': 1, 'boromir': 2, 'legolas': 3, 'gimli': 4, 'meriadoc': 5, 'peregrin': 6}
MIN = ['witch-king', 'saruman', 'mouth-of-sauron']
LEVEL = [3, 3, 2, 2, 2, 1, 1]
NOEFFECT = set()      # (position, key) of card plays that change nothing in our engine
LOC_FSP, LOC_DEAD, LOC_NONE = 200, 201, 202
(M_CHOICE, M_PASS, M_SKIP, M_RING, M_FSP_MOVE, M_FSP_HIDE, M_SEPARATE, M_COMPS, M_ARMY, M_ATTACK, M_PLAY, M_DRAW, M_DIPLO, M_MUSTER_ELITE,
 M_MUSTER, M_GANDALF_WHITE, M_ARAGORN, M_COMP_POLITICS, M_DISCARD_TABLE, M_NAZGUL, M_MINION, M_VOICE, M_COMP_MOVE, M_NAZ_MOVE, M_DONE, M_NAZ_PLAN) = range(26)

def face(i, side): return FACE[i] if i < 5 else ('will' if side == 0 else 'eye')

def position_text(p):
    s = p['state']; side = 0 if p['actor'] == 'fp' else 1; o = []
    for rid, r in s['regions'].items():
        ri = RI[rid]
        for box in (r, r.get('siegeBox') or {}):
            fpn = None
            for n, u in (box.get('units') or {}).items():
                if u['regular'] or u['elite']:
                    o.append(f"u {ri} {NI[n]} {u['regular']} {u['elite']}")
                    if NI[n] < 5 and fpn is None: fpn = NI[n]
            if box.get('leaders'): o.append(f"l {ri} {fpn if fpn is not None else 2} {box['leaders']}")
            if box.get('nazgul'): o.append(f"z {ri} {box['nazgul']}")
        if r['control']: o.append(f"c {ri} {0 if r['control'] == 'fp' else 1}")
        if r['besieged']: o.append(f"g {ri}")
    ch = s['characters']; fs = s['fellowship']
    loc = [LOC_NONE] * 7; white = aragorn = 0
    for c in fs['companions']:
        if c != 'gollum': loc[CID[c]] = LOC_FSP
    for c in ch['eliminated']:
        if c in CID: loc[CID[c]] = LOC_DEAD; white |= c == 'gandalf-white'; aragorn |= c == 'aragorn'
    for c, rid in ch['inPlay'].items():
        if c in CID: loc[CID[c]] = RI[rid]; white |= c == 'gandalf-white'; aragorn |= c == 'aragorn'
    for i in range(7): o.append(f"comp {i} {loc[i]}")
    for i, m in enumerate(MIN):
        o.append(f"min {i} {RI[ch['inPlay'][m]] if m in ch['inPlay'] else LOC_DEAD if m in ch['eliminated'] else LOC_NONE}")
    seen = int(any(m in ch['entered'] for m in MIN))
    o.append(f"flags {int(white)} {int(aragorn)} {seen} {int(bool(s['flags'].get('mouthMusterUsedThisTurn')))}")
    for n, r in s['reinforcements'].items():
        o.append(f"pool {NI[n]} {r['regular']} {r['elite']} {r.get('nazgul', 0) if n == 'sauron' else r.get('leader', 0)}")
    for n, v in s['nations'].items(): o.append(f"pol {NI[n]} {v['step']} {int(v['active'])}")
    o.append(f"fsp {RI[fs['location']]} {fs['progress']} {int(not fs['hidden'])} {fs['corruption']}")
    guide = 7 if fs['guide'] == 'gollum' else CID[fs['guide']]
    o.append(f"fsp2 {guide} {-1 if fs['mordor'] is None else fs['mordor']} {int(s['flags']['fellowshipDeclaredOrMovedThisTurn'])}")
    o.append(f"hunt {s['hunt']['box']} {s['hunt']['fpDiceInBox']}")
    for sd, name in ((0, 'fp'), (1, 'shadow')):
        cnt = collections.Counter(s['dice'][name])
        for f, k in cnt.items(): o.append(f"die {sd} {FACE.index(f) if f in FACE else 5} {k}")
        used = s['flags']['fpUsedElvenRingThisTurn' if sd == 0 else 'shadowUsedElvenRingThisTurn']
        o.append(f"ring {sd} {s['elvenRings'].count(name)} {int(used)}")
        cp = s['cards'][name]
        for c in cp['hand']: o.append(f"card {CI[c]} 1")
        for c in cp['table']: o.append(f"card {CI[c]} 2")
        for c in cp['discard']['character'] + cp['discard']['strategy'] + (cp.get('discardFaceDown') or []): o.append(f"card {CI[c]} 3")
    o.append(f"turn {s['turn']}"); o.append(f"go {side}")
    return '\n'.join(o) + '\n'

def ours_keys(lines, side, p=None):
    """Our moves as comparison keys; each key maps to the TypeScript action(s) that would express it."""
    keys = {}
    st = p['state'] if p else None
    def tsid(i):       # the TypeScript id of companion slot i in this position
        if not st: return COMP[i]
        alt = {0: ('gandalf-white', 'gandalf-grey'), 1: ('aragorn', 'strider')}.get(i)
        if alt: return alt[0] if alt[0] in st['characters']['inPlay'] else alt[1]
        return COMP[i]
    def where(c): return st['characters']['inPlay'].get(c) if st else None
    f = lambda i: face(i, side)
    for ln in lines:
        sub = ln.startswith('  '); t, die, a, b, c, d, e, _ = map(int, ln.split()[:8])
        if 'noeffect' in ln: NOEFFECT.add((id(p), f"play:{CARDS[a]}"))
        if sub:
            if t == M_COMP_MOVE:
                grp = [i for i in range(7) if a >> i & 1]; lead = min(grp, key=lambda i: (-LEVEL[i], i))      # a group travels at its highest Level
                act = {'kind': 'moveCharacter', 'char': tsid(lead), 'from': where(tsid(lead)), 'to': REGS[b]}
                if len(grp) > 1: act['chars'] = [tsid(i) for i in grp]
                keys[f"moveChar:{'+'.join(COMP[i] for i in grp)}>{REGS[b]}"] = [act]
            elif t == M_NAZ_MOVE:
                ch = ['nazgul', 'witch-king', 'mouth-of-sauron'][d]
                keys[f"moveChar:{ch}:{REGS[a]}>{REGS[b]}"] = [{'kind': 'moveCharacter', 'char': ch, 'from': REGS[a], 'to': REGS[b]}]
            continue
        if t == M_PASS: keys['pass'] = [{'kind': 'pass'}]
        elif t == M_SKIP: keys[f"skipDie:{f(die & 0x7F)}"] = [{'kind': 'skipDie', 'face': f(die & 0x7F)}]
        elif t == M_RING: keys[f"ring:{f(a)}>{f(b)}"] = [{'kind': 'useElvenRing', 'from': f(a), 'to': f(b)}]
        elif t == M_FSP_MOVE: keys['moveFellowship'] = [{'kind': 'moveFellowship'}]
        elif t == M_FSP_HIDE: keys['hideFellowship'] = [{'kind': 'hideFellowship'}]
        elif t == M_SEPARATE:
            for i in range(7):
                if a >> i & 1: keys[f"separate:{COMP[i]}"] = [{'kind': 'separateCompanion', 'companion': tsid(i)}]
        elif t == M_ARMY: keys[f"moveArmy:{REGS[a]}>{REGS[b]}"] = [{'kind': 'moveArmy', 'from': REGS[a], 'to': REGS[b], 'die': f(die & 0x7F)}, {'kind': 'moveArmy', 'from': REGS[a], 'to': REGS[b]}]
        elif t == M_ATTACK: keys[f"attack:{REGS[a]}>{REGS[b]}"] = [{'kind': 'attack', 'from': REGS[a], 'to': REGS[b], 'die': f(die & 0x7F)}, {'kind': 'attack', 'from': REGS[a], 'to': REGS[b]}]
        elif t == M_PLAY: keys[f"play:{CARDS[a]}"] = [{'kind': 'playEvent', 'cardId': CARDS[a], 'die': f(die & 0x7F)}, {'kind': 'playEvent', 'cardId': CARDS[a]}]
        elif t == M_DRAW: keys[f"draw:{['character', 'strategy'][a]}"] = [{'kind': 'drawEvent', 'deck': ['character', 'strategy'][a]}]
        elif t == M_DIPLO: keys[f"diplo:{NAT[a]}"] = [{'kind': 'diplomaticAction', 'nation': NAT[a]}]
        elif t in (M_MUSTER_ELITE, M_MUSTER):
            fig = 'elite' if t == M_MUSTER_ELITE else 'regular' if not b else 'leader' if side == 0 else 'nazgul'
            nat = json_nation(REGS[a])
            act = {'kind': 'recruitUnit', 'nation': nat, 'region': REGS[a], 'regular': int(fig == 'regular'), 'elite': int(fig == 'elite')}
            if fig in ('leader', 'nazgul'): act[fig] = 1
            keys[f"recruit:{REGS[a]}:{fig}"] = [act]
        elif t == M_GANDALF_WHITE: keys['upgrade:gandalf-white'] = [{'kind': 'bringUpgrade', 'which': 'gandalf-white'}]
        elif t == M_ARAGORN: keys['upgrade:aragorn'] = [{'kind': 'bringUpgrade', 'which': 'aragorn'}]
        elif t == M_COMP_POLITICS:
            keys[f"compMuster:{NAT[a]}"] = [{'kind': 'companionMuster', 'companion': {'gondor': 'boromir', 'elves': 'legolas', 'dwarves': 'gimli'}.get(NAT[a], 'boromir'), 'nation': NAT[a]}]
        elif t == M_DISCARD_TABLE:
            tries = []
            if side == 0: tries = [{'kind': 'forceDiscardCard', 'cardId': CARDS[a], 'via': v} for v in ('will', 'ring', 'die')]
            elif st:
                h = st['cards']['shadow']['hand']
                for x in h:
                    for y in h:
                        if '-str-' in x and '-char-' in y: tries.append({'kind': 'forceDiscardCard', 'cardId': CARDS[a], 'via': 'cards', 'discardStrategy': x, 'discardCharacter': y})
            keys[f"forceDiscard:{CARDS[a]}"] = tries[:4]
        elif t == M_MINION: keys[f"minion:{MIN[a]}:{REGS[b]}"] = [{'kind': 'bringMinion', 'minion': MIN[a], 'region': REGS[b]}]
        elif t == M_VOICE: keys[f"voice:{'upgrade' if a else 'recruit'}"] = [{'kind': 'sarumanMuster', 'mode': 'upgrade' if a else 'recruit'}]
    return keys

_MAP = json.load(open(os.path.join(A, 'map.json')))['regions']
def json_nation(rid): return _MAP[rid]['nation']

def theirs_keys(legal):
    keys = set()
    for a in legal:
        k = a['kind']
        if k == 'pass': keys.add('pass')
        elif k == 'skipDie': keys.add(f"skipDie:{a['face']}")
        elif k == 'useElvenRing': keys.add(f"ring:{a['from']}>{a['to']}")
        elif k in ('moveFellowship', 'hideFellowship'): keys.add(k)
        elif k == 'separateCompanion': keys.add(f"separate:{COMP[CID[a['companion']]]}")
        elif k == 'moveCharacter':
            ch = a['char']
            if ch in CID: keys.add(f"moveChar:{'+'.join(COMP[i] for i in sorted(CID[c] for c in (a.get('chars') or [ch])))}>{a['to']}")
            else: keys.add(f"moveChar:{ch}:{a['from']}>{a['to']}")
        elif k == 'moveArmy': keys.add(f"moveArmy:{a['from']}>{a['to']}")
        elif k == 'attack': keys.add(f"attack:{a['from']}>{a['to']}")
        elif k == 'playEvent': keys.add(f"play:{a['cardId']}")
        elif k == 'drawEvent': keys.add(f"draw:{a['deck']}")
        elif k == 'diplomaticAction': keys.add(f"diplo:{a['nation']}")
        elif k == 'recruitUnit': keys.add(f"recruit:{a['region']}:{'elite' if a.get('elite') else 'leader' if a.get('leader') else 'nazgul' if a.get('nazgul') else 'regular'}")
        elif k == 'bringUpgrade': keys.add(f"upgrade:{a['which']}")
        elif k == 'companionMuster': keys.add(f"compMuster:{a['nation']}")
        elif k == 'forceDiscardCard': keys.add(f"forceDiscard:{a['cardId']}")
        elif k == 'bringMinion': keys.add(f"minion:{a['minion']}:{a['region']}")
        elif k == 'sarumanMuster': keys.add(f"voice:{a['mode']}")
        else: keys.add('UNMAPPED:' + k)
    return keys

def explained(p, k, ours):
    """Differences that are deliberate in our full generator, checked against the position itself."""
    s = p['state']; side = 'fp' if p['actor'] == 'fp' else 'shadow'
    if k.startswith('moveArmy:'):
        a, b = k[9:].split('>'); mine = (NAT[:5] if side == 'fp' else NAT[5:])
        cnt = lambda r: sum(u['regular'] + u['elite'] for n, u in s['regions'][r]['units'].items() if n in mine)
        if cnt(a) + cnt(b) > 10: return 'army move that overfills the destination (units would be removed); we offer the partial moves only'
    if k.startswith('ring:'):
        frm, to = k[5:].split('>')
        if frm == 'will': return 'Elven Ring spent on a Will of the West die (which can already do anything)'
        if frm == 'armyMuster' and to in ('army', 'muster'): return 'Elven Ring to an Army or Muster face (we offer the Army/Muster face, which does both)'
        if to in ('army', 'muster') and f"ring:{frm}>armyMuster" in ours: return 'Elven Ring to an Army or Muster face (we offer the Army/Muster face, which does both)'
    return None

def main():
    path = sys.argv[1]; nex = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    lines = [l for l in open(path) if json.loads(l)['state']['turn'] <= 30]      # our game is a draw after turn 30
    P = [json.loads(l) for l in lines]
    path = os.path.join(HERE, '.positions.jsonl'); open(path, 'w').writelines(lines)
    out = subprocess.run([os.path.join(ROOT, 'bin', 'inject')], input=''.join(position_text(p) for p in P), capture_output=True, text=True).stdout
    blocks = out.split('end\n')[:len(P)]
    assert len(blocks) == len(P), (len(blocks), len(P))
    diffs = []; queries = []; tot_o = tot_t = 0; ours_by_pos = {}
    for i, (p, blk) in enumerate(zip(P, blocks)):
        side = 0 if p['actor'] == 'fp' else 1
        o = ours_keys([l for l in blk.split('\n') if l.strip()], side, p); t = theirs_keys(p['legal'])
        tot_o += len(o); tot_t += len(t)
        d = {'ours': sorted(set(o) - t), 'theirs': sorted(t - set(o)), 'verdict': {}}
        for k in d['ours']: queries.append({'i': i, 'key': k, 'tries': o[k]})
        ours_by_pos[id(p)] = set(o)
        diffs.append(d)
    # their legalActions samples some move families, so ask their validator about everything only we list
    if queries:
        qf = os.path.join(HERE, '.queries.json'); rf = os.path.join(HERE, '.results.json')
        json.dump(queries, open(qf, 'w'))
        subprocess.run(['npx', 'vite-node', 'validate.mjs', os.path.abspath(path), qf, rf], cwd=HERE, check=True, capture_output=True)
        for q, r in zip(queries, json.load(open(rf))): diffs[q['i']]['verdict'][q['key']] = r
        os.remove(qf); os.remove(rf)
    os.remove(path)
    cat = collections.Counter(); known = collections.Counter(); ex = collections.defaultdict(list); clean = accepted = 0
    for p, d in zip(P, diffs):
        bad = 0
        for k in d['ours']:
            v = d['verdict'][k]
            if v['ok']: accepted += 1; continue
            st = p['state']
            if k == 'play:sh-char-22' and st['nations']['rohan']['active']: NOEFFECT.add((id(p), k))        # Wormtongue once Rohan is active
            if k == 'play:sh-str-24' and not st['reinforcements']['sauron']['regular']: NOEFFECT.add((id(p), k))   # Pits of Mordor, nothing to recruit
            if (id(p), k) in NOEFFECT: known['card play that would have no effect (they forbid it, we allow it)'] += 1; continue
            bad = 1; c = f"we list, they reject: {k.split(':')[0]} -- {v['reason'][:90]}"; cat[c] += 1
            if len(ex[c]) < nex: ex[c].append((p['seed'], p['n'], p['actor'], k))
        for k in d['theirs']:
            why = explained(p, k, ours_by_pos[id(p)])
            if why: known[why] += 1; continue
            bad = 1; c = 'they list, we do not: ' + k.split(':')[0]; cat[c] += 1
            if len(ex[c]) < nex: ex[c].append((p['seed'], p['n'], p['actor'], k))
        clean += not bad
    print(f"{len(P)} positions; no disagreement in {clean} ({100.0 * clean / len(P):.1f}%); options ours {tot_o}, theirs listed {tot_t}; "
          f"{accepted} of ours not in their list but accepted by their validator")
    if len(sys.argv) > 3:       # break one family down by exact option
        det = collections.Counter()
        for p, d in zip(P, diffs):
            for k in d['ours']:
                if k.startswith(sys.argv[3]) and not d['verdict'][k]['ok']: det['we list, they reject: ' + k + ' -- ' + d['verdict'][k]['reason'][:70]] += 1
            for k in d['theirs']:
                if k.startswith(sys.argv[3]): det['they list, we do not: ' + k] += 1
        for k, n in det.most_common(60): print(f"  {n:5d} {k}")
        return
    for c, n in known.most_common(): print(f"  {n:6d}  understood: {c}")
    for c, n in cat.most_common():
        print(f"  {n:6d}  {c}")
        for e in ex[c]: print(f"            seed {e[0]} step {e[1]} {e[2]}: {e[3]}")

if __name__ == '__main__': main()
