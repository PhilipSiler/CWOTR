#!/usr/bin/env python3
"""Turn the decoded human games into positions of our own engine and print the strategy formula's terms for each.

Usage: positions.py <decoded dir> <out csv>
Every end-of-turn board in <id>.turns.jsonl is written in the position loader's language (tests/inject.c) and fed to
bin/inject, which answers with the formula terms. The csv has one row per position: game, turn, how the game ended,
then the terms. What the logs do not show (cards in hand, the Hunt pool, reinforcement pools) is left at its start
value; dice are not rolled yet, so the "dice in hand" term is zero throughout.
"""
import csv, json, os, re, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
REG = {n: int(i) for n, i in re.findall(r'\bR_([A-Z_]+)\s*=\s*(\d+)', open(os.path.join(ROOT, 'src', 'data.h')).read())}
NAT = {'DWARVES': 0, 'ELVES': 1, 'GONDOR': 2, 'THE_NORTH': 3, 'ROHAN': 4, 'ISENGARD': 5, 'SAURON': 6, 'SOUTHRON_EASTERLINGS': 7}
POLN = {'Dwarves': 0, 'Elves': 1, 'Gondor': 2, 'North': 3, 'Rohan': 4, 'Isengard': 5, 'Sauron': 6, 'S&E': 7}
POL0 = {0: 3, 1: 3, 2: 2, 3: 3, 4: 3, 5: 1, 6: 1, 7: 2}
ACT0 = {0: 0, 1: 1, 2: 0, 3: 0, 4: 0, 5: 1, 6: 1, 7: 1}
COMP = {'Gandalf': 0, 'Strider': 1, 'Boromir': 2, 'Legolas': 3, 'Gimli': 4, 'Meriadoc': 5, 'Peregrin': 6}
MIN = {'Witch King': 0, 'Saruman': 1, 'Mouth of Sauron': 2}
NTERMS = 24
# how the game ended -> 0 Ring destroyed, 1 corruption, 2 Shadow conquest, 3 Free Peoples conquest (4/5: Shadow / Free Peoples won, way unknown)
END = {'Ring destroyed': 0, 'corruption': 1, 'Shadow conquest': 2, 'Free Peoples conquest': 3, 'FR': 0, 'SR': 1, 'SM': 2, 'FM': 3, 'SC': 4, 'FC': 5,
       'FPRV': 0, 'SPRV': 1, 'SPMV': 2, 'FPMV': 3, 'Conceded SP won': 4, 'Conceded FP won': 5}
FPSET = ('GREY_HAVENS', 'RIVENDELL', 'WOODLAND_REALM', 'LORIEN', 'EREBOR', 'HELMS_DEEP', 'MINAS_TIRITH', 'DOL_AMROTH', 'THE_SHIRE', 'EDORAS', 'DALE', 'PELARGIR')
SHSET = ('ORTHANC', 'MORIA', 'DOL_GULDUR', 'MOUNT_GUNDABAD', 'BARAD_DUR', 'MINAS_MORGUL', 'MORANNON', 'UMBAR', 'ANGMAR', 'FAR_HARAD', 'SOUTH_RHUN')


def region(name):
    """Decoded area name -> (our region number, inside a besieged stronghold?) or (None, False)."""
    inside = name.endswith(' Stronghold')
    if inside: name = name[:-11]
    k = re.sub(r"[^A-Z ]", '', name.upper().replace('-', ' ').replace("'", '')).strip().replace(' ', '_')
    if k == 'THE_GREY_HAVENS': k = 'GREY_HAVENS'
    return REG.get(k), inside


def commands(t, seen_min):
    out = ['turn %d' % (t['turn'] + 1)]
    units = {}; fp_here = {}; sh_here = {}; siege = set(); marks = {}
    comp = {c: 201 for c in COMP.values()}; minion = {}; white = aragorn = 0
    for name, v in t['board'].items():
        if name in ('Fellowship Of The Ring', 'Guide Of The Fellowship'):
            for o in v.get('other', []):
                if o.rstrip('*') in COMP: comp[COMP[o.rstrip('*')]] = 200
            continue
        r, inside = region(name)
        if r is None: continue
        for side in ('FP', 'SH'):
            for what, n in v.get(side, {}).items():
                if what == 'NAZGUL': units[(r, 'z')] = units.get((r, 'z'), 0) + n; continue
                nat, kind = what.rsplit(' ', 1)
                units[(r, NAT[nat], kind)] = units.get((r, NAT[nat], kind), 0) + n
                d = fp_here if side == 'FP' else sh_here; d[r] = d.get(r, 0) + n
            if inside and side == 'FP' and v.get('FP'): siege.add(r)
        for o in v.get('other', []):
            b = o.rstrip('*')
            if b in COMP:
                comp[COMP[b]] = r
                if o.endswith('*') and b == 'Gandalf': white = 1
                if o.endswith('*') and b == 'Strider': aragorn = 1
            elif b in MIN: minion[MIN[b]] = r; seen_min.add(MIN[b])
            elif b.startswith('captured by'): marks[r] = 1 if 'Shadow' in b else 0
    for (r, *rest), n in units.items():
        if rest == ['z']: out.append('z %d %d' % (r, n))
    for r in set(k[0] for k in units if len(k) == 3):
        for nat in range(8):
            reg, eli, ldr = units.get((r, nat, 'REGULAR'), 0), units.get((r, nat, 'ELITE'), 0), units.get((r, nat, 'LEADER'), 0)
            if reg or eli: out.append('u %d %d %d %d' % (r, nat, reg, eli))
            if ldr and nat < 5: out.append('l %d %d %d' % (r, nat, ldr))
    for r in siege: out.append('g %d' % r)
    # who holds each settlement: a control marker, or the enemy standing in it with no defender left
    for names, own in ((FPSET, 0), (SHSET, 1)):
        for nm in names:
            r = REG[nm]; mine, theirs = (fp_here, sh_here) if own == 0 else (sh_here, fp_here)
            if marks.get(r) == 1 - own or (theirs.get(r, 0) and not mine.get(r, 0)): out.append('c %d %d' % (r, 1 - own))
    for c, where in comp.items(): out.append('comp %d %d' % (c, where))
    for m in range(3): out.append('min %d %d' % (m, minion.get(m, 201 if m in seen_min else 202)))
    out.append('flags %d %d %d 0' % (white, aragorn, 1 if seen_min else 0))
    pol = dict(POL0); act = dict(ACT0)
    for name, pos in t['tracks'].get('politics', {}).items():
        n = POLN.get(name.rstrip('*'))
        if n is None: continue
        pol[n] = pos
        if name.endswith('*'): act[n] = 1 - ACT0[n] if n < 5 else 1
    for n in range(8): out.append('pol %d %d %d' % (n, pol[n], 1 if pol[n] == 0 else act[n]))
    tr = t['tracks']
    fr, _ = region(tr.get('Fellowship last seen', 'Rivendell'))
    mordor = tr.get('Mordor step', -1)
    out.append('fsp %d %d 0 %d' % (fr if fr is not None else REG['RIVENDELL'], min(12, tr.get('FSP', 0)) if mordor < 0 else 0, min(12, tr.get('corruption', 0))))
    guide = 0 if comp[0] == 200 else next((c for c in range(7) if comp[c] == 200), 7)
    out.append('fsp2 %d %d 0' % (guide, mordor))
    out.append('prior')
    return out


def main(d, outcsv):
    games = [g for g in json.load(open(os.path.join(d, 'games.json'))) if 'expansion' not in g['module'] and g['turns'] >= 5 and g['moves_consistent'] > 100 and g['moves_inconsistent'] < 0.05 * g['moves_consistent']]
    rows = []; script = []; labels = []
    for g in games:
        seen = set()
        for l in open(os.path.join(d, g['id'] + '.turns.jsonl')):
            t = json.loads(l)
            script += commands(t, seen)
            rows.append([g['id'], t['turn'], g['turns'], g.get('result') or g.get('ended', ''), t['tracks'].get('Shadow VP', 0), t['tracks'].get('FP VP', 0)])
            held = int(__import__('hashlib').sha1(g['id'].encode()).hexdigest(), 16) % 5 == 0       # a fifth of the games is never trained on
            end = END.get(rows[-1][3], -1)
            labels.append('%d %d %d %d' % (-1 if held or end < 0 else (1 if end in (0, 3, 5) else 0), g['turns'] - t['turn'], end, held))
    res = subprocess.run([os.path.join(ROOT, 'bin', 'inject')], input='\n'.join(script) + '\n', capture_output=True, text=True).stdout.splitlines()
    pri = [l.split()[1:] for l in res if l.startswith('prior')]
    assert len(pri) == len(rows), (len(pri), len(rows))
    net = len(pri[0]) > NTERMS + 7; n = NTERMS
    with open(outcsv, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['game', 'turn', 'game_turns', 'ended', 'log_shadow_vp', 'log_fp_vp'] + ['p%d' % i for i in range(n)] + ['our_shadow_vp', 'our_fp_vp', 'mordor_units', 'front_units', 'walls_units', 'inside_units', 'fp_besieged'] + (['net_logit'] if net else []))
        for r, p in zip(rows, pri): w.writerow(r + p)
    if os.environ.get('INJECT_SAMPLES'):      # one label line per position, in the order of the feature records: win (1 FP, 0 Shadow, -1 unused), turns left, ending, held out
        open(os.environ['INJECT_SAMPLES'] + '.labels', 'w').write('\n'.join(labels) + '\n')
    print('%d positions from %d games -> %s' % (len(rows), len(games), outcsv))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
