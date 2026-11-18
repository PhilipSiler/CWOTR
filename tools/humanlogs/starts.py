#!/usr/bin/env python3
"""Write the end-of-turn boards of the decoded human games as starting positions for training games.

Usage: starts.py <decoded dir> <out file>
One block per position in the position language of wotr_set (src/util.c), closed by a line
"end <game> <turn about to start> <turns the game lasted> <F|S|?: who won>". The trainer loads the file with WOTR_STARTS=<file>.
Positions from a game's last turn are left out (the game was over), as are turns after 14.
"""
import json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import positions as P


def won(g):
    """F / S: who won the game the position comes from ('?' if the record does not say), for `wotr lessons`."""
    r = (g.get('result') or '')
    if r.startswith('FP') or 'FP won' in r: return 'F'
    if r.startswith('SP') or 'SP won' in r: return 'S'
    return '?'


def main(d, out):
    games = [g for g in json.load(open(os.path.join(d, 'games.json'))) if 'expansion' not in g['module'] and g['turns'] >= 5 and g['moves_consistent'] > 100 and g['moves_inconsistent'] < 0.05 * g['moves_consistent']]
    n = 0
    with open(out, 'w') as f:
        for g in games:
            seen = set()
            for l in open(os.path.join(d, g['id'] + '.turns.jsonl')):
                t = json.loads(l)
                cmds = [c for c in P.commands(t, seen) if c != 'prior']
                if t['turn'] >= g['turns'] or t['turn'] > 13: continue
                f.write('\n'.join(cmds) + '\nend %s %d %d %s\n' % (g['id'], t['turn'] + 1, g['turns'], won(g))); n += 1
    print('%d starting positions from %d games -> %s' % (n, len(games), out))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
