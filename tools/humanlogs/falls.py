#!/usr/bin/env python3
"""Which victory-point settlements fall within two turns? Fit a small forecast on the human games.

Usage: falls.py <decoded dir>
For every end-of-turn position and every settlement still in its owner's hands, the local picture (garrison, enemies
next door, enemies two regions away, siege, leadership on both sides, stronghold or city) is paired with whether the
enemy holds the settlement two turns later. Prints how often that happens, how well the engine's present hand-made
"danger" figure ranks it, and a logistic fit with its weights -- separately for Shadow attacking Free Peoples
settlements and the reverse. A fifth of the games (the same fifth as elsewhere) is kept out of the fit and used to score it.
"""
import collections, hashlib, json, os, subprocess, sys
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import positions as P


def auc(s, y):
    o = np.argsort(s); r = np.empty(len(s)); r[o] = np.arange(1, len(s) + 1); n1 = y.sum(); n0 = len(y) - n1
    return (r[y == 1].sum() - n1 * (n1 + 1) / 2) / (n1 * n0) if n1 and n0 else float('nan')


def main(d):
    games = [g for g in json.load(open(os.path.join(d, 'games.json'))) if 'expansion' not in g['module'] and g['turns'] >= 5 and g['moves_consistent'] > 100 and g['moves_inconsistent'] < 0.05 * g['moves_consistent']]
    script = []; index = []
    for g in games:
        seen = set()
        for l in open(os.path.join(d, g['id'] + '.turns.jsonl')):
            t = json.loads(l); script += P.commands(t, seen); index.append((g['id'], t['turn']))
    out = subprocess.run([os.path.join(P.ROOT, 'bin', 'inject')], input='\n'.join(script) + '\n', capture_output=True, text=True, env=dict(os.environ, INJECT_LOCAL='1')).stdout.splitlines()
    pos = []; cur = None
    for l in out:
        if l.startswith('prior'): cur = []; pos.append(cur)
        elif l.startswith('local'): cur.append([int(x) for x in l.split()[1:]])
    assert len(pos) == len(index)
    by = collections.defaultdict(dict)
    for (gid, turn), loc in zip(index, pos): by[gid][turn] = loc
    rows = {0: [], 1: []}                       # by original owner: 0 Free Peoples settlement, 1 Shadow settlement
    for gid, turns in by.items():
        held = int(hashlib.sha1(gid.encode()).hexdigest(), 16) % 5 == 0
        for t, loc in turns.items():
            later = turns.get(t + 2) or turns.get(t + 1)
            if later is None or t + 1 not in turns: continue        # the game ended: nothing to learn about "two turns on"
            for a, b in zip(loc, later):
                r, own, holder, mine, e1, e2, siege, la, ld, sh = a
                if holder != own: continue
                rows[own].append((held, b[2] != own, mine, e1, e2, siege, la, ld, sh, t))
    for own, title in ((0, 'Shadow taking a Free Peoples settlement'), (1, 'the Free Peoples taking a Shadow settlement')):
        A = np.array(rows[own], float); held = A[:, 0] == 1; y = A[:, 1]
        mine, e1, e2, siege, la, ld, sh = (A[:, i] for i in range(2, 9))
        print('\n%s within two turns: %d settlement-positions, %.1f%% fall' % (title, len(A), 100 * y.mean()))
        danger = np.clip((e1 + 0.5 * e2 - np.where(sh == 1, 1.2, 0.8) * mine) / 6 + 0.5 * ((mine == 0) & (e1 + e2 > 0)), 0, 1)
        print('  present "danger" figure ranks them at %.3f (0.5 = coin flip, 1.0 = perfect), held-out games' % auc(danger[held], y[held]))
        F = np.column_stack([np.minimum(e1, 10), np.minimum(e2, 10), np.minimum(mine, 10), siege * (0 if os.environ.get('FALLS_NO_SIEGE') else 1), sh, np.minimum(la, 4), np.minimum(ld, 4), (mine == 0) * 1.0, np.minimum(e1, 10) * sh, np.ones(len(A))])
        names = ['enemy units next door or besieging', 'enemy units two regions away', 'garrison units', 'under siege', 'stronghold', 'attackers\' leaders', 'defenders\' leaders', 'no garrison', 'enemy next door x stronghold', 'constant']
        w = np.zeros(F.shape[1]); Ft, yt = F[~held], y[~held]
        for it in range(30):                    # Newton's method: a few steps reach the logistic fit exactly
            p = 1 / (1 + np.exp(-(Ft @ w))); H = (Ft * (p * (1 - p))[:, None]).T @ Ft + 1e-3 * np.eye(Ft.shape[1])
            w -= np.linalg.solve(H, Ft.T @ (p - yt) + 1e-3 * w)
        z = F @ w; p = 1 / (1 + np.exp(-z))
        print('  fitted forecast ranks them at %.3f on the held-out games' % auc(z[held], y[held]))
        print('  weights: ' + ', '.join('%s %+.2f' % (n, x) for n, x in zip(names, w)))
        print('  calibration on held-out games (forecast -> actual): ' + ', '.join('%.0f%% -> %.0f%% (%d)' % (100 * p[held & m].mean(), 100 * y[held & m].mean(), (held & m).sum()) for lo, hi in ((0, .05), (.05, .15), (.15, .3), (.3, .5), (.5, 1.01)) for m in [(p >= lo) & (p < hi)] if (held & m).sum() > 20))
        print('  C: {' + ', '.join('%.3ff' % x for x in w) + '}')


if __name__ == '__main__':
    main(sys.argv[1])
