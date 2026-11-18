#!/usr/bin/env python3
"""Write what the human players DID, action by action, as lessons for the move-suggesting network.

Usage: moves.py <decoded dir> <out file> [all]
For every action die a player spent, the board just before it (rebuilt by following every piece through the game's
events, as decode.py does for the end of each turn) is written in the position language of wotr_set (src/util.c),
with both sides' unspent dice, followed by one line saying what the player did with the die:
  act <side 0 FP / 1 Shadow> army <from> <to> <regulars> <elites>     an army moved or attacked (first move of the die)
  act <side> muster <region> <0 regular / 1 leader or Nazgul / 2 elite / 3 minion> <minion>
  act <side> attack <region attacked>          act <side> diplo <nation>          act <side> draw <0 character / 1 strategy deck>          act <side> fsp
and closed by "end <game> <turn> <turns the game lasted> <F|S|?: who won> <1 if the game had a handicap>".
Card plays, companion and Nazgul-only moves are left out (the logs do not show hands). `wotr imitate` reads the file.
Games with a handicap are left out unless "all" is given.
"""
import json, os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode as D
import positions as P
from starts import won

FACE = {'Character': 0, 'Army': 1, 'Muster': 2, 'Event': 3, 'Muster/Army': 4, 'Eye': 5, 'Will of the West': 5}
LETTER = {'A': 'Army', 'C': 'Character', 'M': 'Muster', 'H': 'Muster/Army', 'P': 'Event', 'E': 'Eye', 'W': 'Will of the West'}
POOL = {'FP': 'FP Dice', 'SH': 'SA Dice'}


def is_die(pid): return 'action die' in D.label(pid)


def follow(e, loc, flipped, faces):
    """Apply one decoded event to the piece locations (the same rules as decode.py)."""
    v = e['verb']; pcs = [p['id'] for p in e.get('pieces', [])]
    if v in ('moves', 'draws', 'plays card'):
        if e.get('to'):
            for p in pcs: loc[p] = e['to']
    elif v == 'kills':
        for p in pcs:
            o = D.owner(p); lab = D.label(p)
            if p in D.PT.AUTH_ARMY_PIECES and o == 'SH': loc[p] = 'SA Reinforcements'
            elif lab == 'hunt tile': loc[p] = 'Used Hunt Tiles'
            elif 'card' in lab: loc[p] = 'discard'
            else: loc[p] = 'FP Casualties' if o == 'FP' else 'SA Casualties' if o == 'SH' else 'removed'
    elif v == 'allocates':
        for p in pcs:
            if e.get('to') == 'The Hunt For The Ring' or faces.get(p) == 'Eye': loc[p] = 'The Hunt For The Ring'
    elif v == 'flips':
        flipped.symmetric_difference_update(pcs)
    elif v == 'rolls':
        if pcs and is_die(pcs[0]):                 # dice that are rolled are in their owner's pool, wherever the log last put them
            for p, f in zip(pcs, e.get('faces', [])): faces[p] = f; loc[p] = POOL['FP' if D.label(p).startswith('FP') else 'SH']
    elif v == 'changes die':
        if pcs: faces[pcs[0]] = LETTER.get(e.get('face'), '?')


ARMYDIE = ('Army', 'Muster/Army', 'Character'); MUSTERDIE = ('Muster', 'Muster/Army')


def what_was_done(side, evs):
    """The acting side's use of the die, from the events of one action: a tuple of words, or None if it is not one we teach."""
    did = what_events_say(side, evs)
    die = evs[0].get('die')
    if not did: return None
    if did[0] in ('army', 'attack') and die not in ARMYDIE: return None      # an event card that moved or fought: a card play
    if did[0] in ('muster', 'diplo') and die not in MUSTERDIE: return None
    if did[0] == 'draw' and die != 'Event': return None
    if did[0] == 'fsp' and (die != 'Character' or side != 'FP'): return None
    return did


def what_events_say(side, evs):
    # a battle (both sides choose a combat card): the die was spent on an attack, on the region where the enemy's units fell
    # or, failing that, the one the attackers then advanced along
    if any(e['verb'] == 'chooses' for e in evs) and evs[0].get('die') in ARMYDIE:
        for e in evs:
            if e['verb'] == 'kills' and e.get('from'):
                foe = [p for p in e.get('pieces', []) if p['id'] in D.PT.AUTH_ARMY_PIECES and D.owner(p['id']) not in (side, None)]
                r, _ = P.region(e['from'])
                if foe and r is not None: return ('attack', r)
    for e in evs:
        if e['side'] != side: continue
        v = e['verb']; pcs = [p['id'] for p in e.get('pieces', [])]
        if not pcs or is_die(pcs[0]): continue
        frm, to = e.get('from') or '', e.get('to') or ''
        if v == 'plays card': return None
        if v == 'draws':
            if 'Deck' in frm and 'hand' in to and ('Character' in frm or 'Strategy' in frm): return ('draw', 0 if 'Character' in frm else 1)
            return None
        if v != 'moves': continue
        lab = D.label(pcs[0])
        if frm.startswith('Political Track') and to.startswith('Political Track'):
            n = P.POLN.get(lab)
            return ('diplo', n) if n is not None and int(to.split()[-1]) < int(frm.split()[-1]) else None
        if lab == 'FSP' and frm.startswith('Fellowship Track') and to.startswith('Fellowship Track'): return ('fsp',)
        mine = [p for p in pcs if D.owner(p) == side or D.label(p) == 'NAZGUL' and side == 'SH']
        if not mine: continue
        r2, in2 = P.region(to)
        if r2 is None: continue
        if 'Reinforcements' in frm or frm == '@reinforcements':
            p = mine[0]; lab = D.label(p)
            if lab in P.MIN: return ('muster', r2, 3, P.MIN[lab])
            if p in D.PT.AUTH_ARMY_PIECES:
                kind = D.PT.AUTH_ARMY_PIECES[p][1]
                return ('muster', r2, 2 if kind == 'ELITE' else 1 if kind == 'LEADER' else 0, 0)
            if lab == 'NAZGUL': return ('muster', r2, 1, 0)
            return None
        r1, in1 = P.region(frm)
        if r1 is None: continue
        if r1 == r2: continue                      # into or out of a besieged stronghold: not what the die was spent on
        reg = eli = 0
        for e2 in evs:                             # everything the side dragged along the same road in this action is one army
            if e2['side'] == side and e2['verb'] == 'moves' and e2.get('from') == frm and e2.get('to') == to:
                for p in e2.get('pieces', []):
                    k = D.PT.AUTH_ARMY_PIECES.get(p['id'])
                    if k and k[1] == 'REGULAR': reg += 1
                    elif k and k[1] == 'ELITE': eli += 1
        if reg + eli == 0: return None             # Nazgul, minions or companions on their own
        return ('army', r1, r2, reg, eli)
    return None


def main(d, out, everything):
    games = [g for g in json.load(open(os.path.join(d, 'games.json'))) if 'expansion' not in g['module'] and g['turns'] >= 5 and g['moves_consistent'] > 100 and g['moves_inconsistent'] < 0.05 * g['moves_consistent']]
    if not everything: games = [g for g in games if not g.get('handicap')]
    n = 0; kinds = {}
    with open(out, 'w') as f:
        for g in games:
            evs = [json.loads(l) for l in open(os.path.join(d, g['id'] + '.events.jsonl'))]
            loc = dict((p, D.MSG.get(a, a)) for p, a in D.PIL.INITIAL_FIGURE_LOCATIONS.items())
            flipped = set(); faces = {}; seen = set()
            i = 0
            while i < len(evs):
                e = evs[i]; pcs = [p['id'] for p in e.get('pieces', [])]
                start = e['verb'] == 'moves' and e.get('to') == 'Current Action' and pcs and is_die(pcs[0]) and 'action' in e and e['side'] in POOL and e['turn'] >= 1
                if not start:
                    follow(e, loc, flipped, faces); i += 1; continue
                j = i
                while j < len(evs) and evs[j].get('action') == e['action']: j += 1
                did = what_was_done(e['side'], evs[i:j])
                if did and e['turn'] <= 16:
                    board, tracks = D.snapshot(loc, flipped)
                    cmds = [c for c in P.commands(dict(turn=e['turn'] - 1, board=board, tracks=tracks), seen) if c != 'prior']
                    dice = {}; hunt = [0, 0]; ok = True
                    for p, where in loc.items():
                        if not is_die(p): continue
                        s = 0 if D.label(p).startswith('FP') else 1
                        if where == POOL['FP' if s == 0 else 'SH']:
                            fc = FACE.get(faces.get(p))
                            if fc is None: ok = False
                            dice[(s, fc)] = dice.get((s, fc), 0) + 1
                        elif where == 'The Hunt For The Ring': hunt[s] += 1
                    if ok:
                        cmds += ['die %d %d %d' % (s, fc, k) for (s, fc), k in sorted(dice.items())]
                        cmds.append('hunt %d %d' % (hunt[1], hunt[0]))
                        side = 0 if e['side'] == 'FP' else 1
                        f.write('\n'.join(cmds) + '\nact %d %s\nend %s %d %d %s %d\n' % (side, ' '.join(str(x) for x in did), g['id'], e['turn'], g['turns'], won(g), 1 if g.get('handicap') else 0))
                        n += 1; kinds[(side, did[0])] = kinds.get((side, did[0]), 0) + 1
                for k in range(i, j): follow(evs[k], loc, flipped, faces)
                i = j
    print('%d actions from %d games -> %s' % (n, len(games), out))
    for k in sorted(kinds): print('  %s %-7s %d' % ('Shadow' if k[0] else 'Free Peoples', k[1], kinds[k]))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], len(sys.argv) > 3 and sys.argv[3] == 'all')
