#!/usr/bin/env python3
"""How well does a trained value network predict who won the human games it was never trained on?

Usage: heldout.py <net file (.fp)> [decoded dir]     prints the score: 0.5 = coin flip, 1.0 = perfect
"""
import csv, hashlib, os, subprocess, sys, tempfile
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
END = {'FPRV': 1, 'FPMV': 1, 'Conceded FP won': 1, 'SPRV': 0, 'SPMV': 0, 'Conceded SP won': 0}


def auc(s, y):
    o = np.argsort(s); r = np.empty(len(s)); r[o] = np.arange(1, len(s) + 1); n1 = y.sum(); n0 = len(y) - n1
    return (r[y == 1].sum() - n1 * (n1 + 1) / 2) / (n1 * n0)


net = sys.argv[1]; d = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, '..', '..', 'data', 'ladder2022', 'decoded')
out = tempfile.mktemp(suffix='.csv')
env = dict(os.environ, INJECT_NET=net); env.pop('INJECT_SAMPLES', None)
subprocess.run([sys.executable, os.path.join(HERE, 'positions.py'), d, out], env=env, check=True, stdout=subprocess.DEVNULL)
R = [r for r in csv.DictReader(open(out)) if r['ended'] in END and int(hashlib.sha1(r['game'].encode()).hexdigest(), 16) % 5 == 0]
os.remove(out)
y = np.array([END[r['ended']] for r in R], float); z = np.array([float(r['net_logit']) for r in R]); t = np.array([int(r['turn']) for r in R])
print('%s: %.3f on %d held-out positions; by turn %s; mean predicted P(FP wins) %.2f, actual %.2f' % (
    net, auc(z, y), len(R), ' '.join('t%d %.2f' % (k, auc(z[t == k], y[t == k])) for k in (2, 4, 6, 8)), (1 / (1 + np.exp(-z))).mean(), y.mean()))
