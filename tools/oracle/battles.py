#!/usr/bin/env python3
"""battles.py -- set-piece battles fought in both engines with the same fixed simple choices; prints the odds side by side."""
import json, os, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C
MAP = json.load(open(os.path.join(C.A, 'map.json')))['regions']
def nb(r, plain=True):
    for a in MAP[r]['adjacency']:
        if not plain or (not MAP[a].get('settlement') and not MAP[a].get('nation')): return a
    raise SystemExit('no neighbour for ' + r)
P1 = next(r for r in sorted(MAP) if not MAP[r].get('settlement') and not MAP[r].get('nation') and any(not MAP[a].get('settlement') and not MAP[a].get('nation') for a in MAP[r]['adjacency']))
T = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
SC = [
 dict(name='open field, 5 regulars each', att='shadow', frm=P1, to=nb(P1), attackers={'sauron': [5, 0]}, defenders={'gondor': [5, 0]}),
 dict(name='open field, 5 regulars + 2 Nazgul vs 3 regulars, 1 elite, 1 leader', att='shadow', frm=P1, to=nb(P1), attackers={'sauron': [5, 0]}, att_nazgul=2, defenders={'gondor': [3, 1]}, def_leaders=1),
 dict(name='city (Dale): 8 regulars, 2 elites, 3 Nazgul vs 5 regulars', att='shadow', frm=nb('dale'), to='dale', attackers={'sauron': [8, 2]}, att_nazgul=3, defenders={'north': [5, 0]}),
 dict(name='fortification (Osgiliath): 6 regulars vs 2 regulars', att='shadow', frm=nb('osgiliath'), to='osgiliath', attackers={'sauron': [6, 0]}, defenders={'gondor': [2, 0]}),
 dict(name='Minas Tirith, defender fights in the field: 8R 2E 2 Nazgul vs 4R 1E 1 leader', att='shadow', frm='osgiliath', to='minas-tirith', attackers={'sauron': [8, 2]}, att_nazgul=2, defenders={'gondor': [4, 1]}, def_leaders=1, withdraw=0),
 dict(name='Minas Tirith, defender withdraws into the siege', att='shadow', frm='osgiliath', to='minas-tirith', attackers={'sauron': [8, 2]}, att_nazgul=2, defenders={'gondor': [4, 1]}, def_leaders=1, withdraw=1),
 dict(name="siege assault on Helm's Deep: 5R 5E 4 Nazgul vs 3R 2E 1 leader", att='shadow', frm='helms-deep', to='helms-deep', siege=1, attackers={'isengard': [5, 5]}, att_nazgul=4, defenders={'rohan': [3, 2]}, def_leaders=1),
 dict(name='siege assault on Minas Tirith: 5 regulars vs 1 regular', att='shadow', frm='minas-tirith', to='minas-tirith', siege=1, attackers={'sauron': [5, 0]}, defenders={'gondor': [1, 0]}),
 dict(name='Free Peoples attack Orthanc in the field: 3R 2E 2 leaders vs 4R 1E', att='fp', frm='fords-of-isen', to='orthanc', attackers={'rohan': [3, 2]}, att_leaders=2, defenders={'isengard': [4, 1]}, withdraw=0),
 dict(name='Free Peoples siege assault on Orthanc: 4R 1E 1 leader vs 2 regulars', att='fp', frm='orthanc', to='orthanc', siege=1, attackers={'rohan': [4, 1]}, att_leaders=1, defenders={'isengard': [2, 0]}),
]
def ours(sc):
    side = 0 if sc['att'] == 'fp' else 1; o = []; a, b = C.RI[sc['frm']], C.RI[sc['to']]
    for reg, units in ((a, sc['attackers']), (b, sc['defenders'])):
        for n, (r, e) in units.items(): o.append(f"u {reg} {C.NI[n]} {r} {e}")
    fpn = lambda u: next((C.NI[n] for n in u if C.NI[n] < 5), 2)
    if side == 0:
        if sc.get('att_leaders'): o.append(f"l {a} {fpn(sc['attackers'])} {sc['att_leaders']}")
    else:
        if sc.get('att_nazgul'): o.append(f"z {a} {sc['att_nazgul']}")
        if sc.get('def_leaders'): o.append(f"l {b} {fpn(sc['defenders'])} {sc['def_leaders']}")
    if sc.get('siege'): o.append(f"g {b}")
    for n in range(8): o.append(f"pol {n} 0 1")
    for i in range(7): o.append(f"comp {i} {C.LOC_FSP}")
    for i in range(3): o.append(f"min {i} {C.LOC_NONE}")
    o += [f"fsp {C.RI['rivendell']} 0 0 0", "fsp2 0 -1 0", f"die {side} 1 2", f"side {side}", f"battle {a} {b} {sc.get('withdraw', 0)} {T}"]
    return '\n'.join(o) + '\n'
out = subprocess.run([os.path.join(C.ROOT, 'bin', 'inject')], input=''.join(ours(s) for s in SC), capture_output=True, text=True).stdout.split('end\n')
qf, rf = os.path.join(C.HERE, '.sc.json'), os.path.join(C.HERE, '.scout.json')
json.dump([dict(s, **{'from': s['frm'], 'trials': T}) for s in SC], open(qf, 'w'))
r = subprocess.run(['npx', 'vite-node', 'battle.mjs', qf, rf], cwd=C.HERE, capture_output=True, text=True)
if r.returncode: print(r.stderr[-2000:]); sys.exit(1)
theirs = json.load(open(rf)); os.remove(qf); os.remove(rf)
print(f"{T} fights each; first number ours, second theirs")
for sc, o, t in zip(SC, out, theirs):
    print(sc['name'])
    if t.get('err'): print('    their engine refused:', t['err'])
    v = o.strip().split()
    if not v or v[0] != 'battle': print('    our engine has no such attack:', o.strip()); continue
    v = list(map(float, v[2:]))
    lab = ['attacker wiped out', 'defender wiped out', 'attacker units lost', 'defender units lost', 'settlement captured', 'siege begins']
    tv = [t['attw'], t['defw'], t['al'], t['dl'], t['cap'], t['sieged']]
    print('    ' + '; '.join(f"{l} {a:.2f} / {b:.2f}" + (' <<<' if abs(a - b) > max(0.04, 0.06 * max(a, b)) else '') for l, a, b in zip(lab, v, tv)))
