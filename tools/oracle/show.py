#!/usr/bin/env python3
"""show.py positions.jsonl seed step [prefix] -- print one recorded position and how the two legal sets differ."""
import json, subprocess, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import compare as C
path, seed, step = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]); pre = sys.argv[4] if len(sys.argv) > 4 else ''
for l in open(path):
    p = json.loads(l)
    if p['seed'] == seed and p['n'] == step: break
s = p['state']; side = 0 if p['actor'] == 'fp' else 1
print('actor', p['actor'], 'turn', s['turn'], 'dice', s['dice'], 'rings', s['elvenRings'], 'hunt box', s['hunt']['box'], s['hunt']['fpDiceInBox'])
print('fellowship', {k: v for k, v in s['fellowship'].items()}, '\nchars', s['characters']['inPlay'], 'dead', s['characters']['eliminated'])
print('politics', {n: (v['step'], v['active']) for n, v in s['nations'].items()})
print('hands', s['cards']['fp']['hand'], s['cards']['shadow']['hand'], 'table', s['cards']['fp']['table'], s['cards']['shadow']['table'])
for rid, r in s['regions'].items():
    if r['units'] or r['nazgul'] or r['characters'] or r['control'] or r['besieged']:
        print(f"  {rid:22s}", {n: (u['regular'], u['elite']) for n, u in r['units'].items()}, 'ldr', r['leaders'], 'naz', r['nazgul'], r['characters'], 'ctrl', r['control'], 'BESIEGED box=' + json.dumps(r.get('siegeBox')) if r['besieged'] else '')
out = subprocess.run([os.path.join(C.ROOT, 'bin', 'inject')], input=C.position_text(p), capture_output=True, text=True).stdout
o = set(C.ours_keys([l for l in out.split('end\n')[0].split('\n') if l.strip()], side, p)); t = C.theirs_keys(p['legal'])
print('OURS ONLY  ', sorted(k for k in o - t if k.startswith(pre)))
print('THEIRS ONLY', sorted(k for k in t - o if k.startswith(pre)))
print('BOTH       ', sorted(k for k in o & t if k.startswith(pre))[:60])
