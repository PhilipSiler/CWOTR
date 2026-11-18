#!/usr/bin/env python3
"""What strong human players do, turn by turn, from the decoded tournament games.

Usage: stats.py <decoded dir>
Reads games.json, *.events.jsonl and *.turns.jsonl written by decode.py (base-game games only).
"""
import collections, json, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decode as D

MAP = set(v for k, v in D.MSG.items() if k.startswith('Game.') and 1881 <= int(k[5:]) <= 1996)
MINIONS = ('Witch King', 'Saruman', 'Mouth of Sauron')
POL = ('Elves', 'Dwarves', 'North', 'Rohan', 'Gondor', 'S&E', 'Sauron', 'Isengard')
MAXT = 12


def is_army(what): return what.split()[0] in ('ISENGARD', 'SAURON', 'SOUTHRON_EASTERLINGS', 'THE_NORTH', 'ROHAN', 'GONDOR', 'ELVES', 'DWARVES')


def classify(evs):
    """One action (everything done with one die) -> a short label."""
    kinds = set(); detail = None
    for e in evs:
        whats = [p['what'].rstrip('*') for p in e.get('pieces', [])]
        frm, to = e.get('from', ''), e.get('to', '')
        if e['verb'] == 'plays card': kinds.add('card'); detail = e.get('card')
        elif e['verb'] == 'draws' and 'Deck' in frm: kinds.add('draw')
        elif e['verb'] == 'moves':
            if any(w in POL for w in whats) and 'Political' in to: kinds.add('politics')
            if any(w in MINIONS for w in whats) and to in MAP and frm not in MAP: kinds.add('minion')
            if any(w == 'FSP' for w in whats): kinds.add('fellowship')
            if any(w == 'Fellowship' for w in whats) and to.startswith('Mordor'): kinds.add('fellowship')
            if any(is_army(w) for w in whats) and to in MAP:
                kinds.add('armymove' if frm in MAP else 'muster')
            elif any(w == 'NAZGUL' or w in MINIONS or w in D.COMPANIONS for w in whats) and to in MAP and frm in MAP: kinds.add('charmove')
            elif any(w == 'NAZGUL' for w in whats) and to in MAP: kinds.add('muster')
    for k, name in (('card', 'play a card'), ('minion', 'bring in a minion'), ('fellowship', 'move the Fellowship'), ('armymove', 'move or attack with armies'), ('muster', 'recruit units'),
                    ('politics', 'advance a nation toward war'), ('draw', 'draw a card'), ('charmove', 'move Nazgul / characters')):
        if k in kinds: return name, detail
    return 'other / unclear', detail


def main(d):
    games = [g for g in json.load(open(os.path.join(d, 'games.json'))) if 'expansion' not in g['module'] and g['turns'] >= 5 and g['moves_consistent'] > 100 and g['moves_inconsistent'] < 0.05 * g['moves_consistent']]
    n = len(games)
    print('%d base-game tournament games; length: average %.1f turns, shortest %d, longest %d' % (n, sum(g['turns'] for g in games) / n, min(g['turns'] for g in games), max(g['turns'] for g in games)))
    acts = {s: collections.defaultdict(collections.Counter) for s in ('SH', 'FP')}
    face_use = collections.defaultdict(collections.Counter)
    per = collections.defaultdict(lambda: collections.defaultdict(list))        # per[name][turn] -> values
    cards = collections.Counter(); war = collections.defaultdict(list); minion = collections.defaultdict(list); first_cap = collections.Counter(); cap_turn = collections.defaultdict(list)
    alive = collections.Counter()
    for g in games:
        evs = [json.loads(l) for l in open(os.path.join(d, g['id'] + '.events.jsonl'))]
        turns = [json.loads(l) for l in open(os.path.join(d, g['id'] + '.turns.jsonl'))]
        by = collections.defaultdict(list)
        for e in evs:
            if 'action' in e: by[e['action']].append(e)
        hunt = collections.Counter(); seen_min = set(); at_war = set()
        for a, es in by.items():
            die = next((e for e in es if 'die' in e), None)
            if not die: continue
            s, t = die['side'], die['turn']
            if s not in acts: continue
            name, detail = classify([e for e in es if e['side'] == s])
            acts[s][min(t, MAXT)][name] += 1
            if s == 'SH': face_use[die['die']][name] += 1
            if name == 'play a card' and s == 'SH' and detail: cards[detail] += 1
        for e in evs:
            t = e['turn']
            if e['verb'] == 'allocates' and e.get('to') == 'The Hunt For The Ring': hunt[t] += len(e['pieces'])
            for p in e.get('pieces', []):
                w = p['what'].rstrip('*')
                if e['verb'] == 'moves' and w in MINIONS and e.get('to') in MAP and w not in seen_min: seen_min.add(w); minion[w].append(t)
                if e['verb'] == 'moves' and w in POL and e.get('to') == 'Political Track 0' and w not in at_war: at_war.add(w); war[w].append(t)
        caps = [c for c in g['captures'] if c['by'] == 'SH']
        if caps: first_cap[caps[0]['region']] += 1
        seen = set()
        for c in caps:
            if c['region'] not in seen: seen.add(c['region']); cap_turn[c['region']].append(c['turn'])
        for tn in turns:
            t = tn['turn']
            if t > MAXT: continue
            alive[t] += 1
            tr = tn['tracks']
            per['Shadow VP'][t].append(tr.get('Shadow VP', 0)); per['FP VP'][t].append(tr.get('FP VP', 0)); per['corruption'][t].append(tr.get('corruption', 0))
            per['Hunt dice set aside'][t].append(hunt[t]); per['Eyes rolled'][t].append(tn['rolled'].get('SH', []).count('Eye'))
            per['Shadow dice'][t].append(len(tn['rolled'].get('SH', [])) + hunt[t]); per['FP dice'][t].append(len(tn['rolled'].get('FP', [])))
            per['in Mordor'][t].append(1 if 'Mordor step' in tr else 0)
            b = tn['board']
            shu = [(sum(c for k, c in v.get('SH', {}).items() if k != 'NAZGUL'), r) for r, v in b.items() if r in MAP or r.endswith('Stronghold')]
            per['Shadow units on the map'][t].append(sum(x for x, _ in shu)); per['largest Shadow army'][t].append(max([x for x, _ in shu] or [0]))
            per['Shadow armies of 6+'][t].append(sum(1 for x, _ in shu if x >= 6))
            per['FP units on the map'][t].append(sum(sum(v.get('FP', {}).values()) for r, v in b.items() if r in MAP or r.endswith('Stronghold')))
            per['FP units besieged'][t].append(sum(sum(v.get('FP', {}).values()) for r, v in b.items() if r.endswith('Stronghold') and not r.startswith('Shadow')))
            home = ('Barad-Dur', 'Gorgoroth', 'Nurn', 'Minas Morgul', 'Morannon')
            per['Shadow units still in Mordor'][t].append(sum(x for x, r in shu if r in home))
    T = range(1, MAXT + 1)
    print('\nturn                              ' + ''.join('%6d' % t for t in T))
    print('games still going                 ' + ''.join('%6d' % alive[t] for t in T))
    for k in ('Shadow VP', 'FP VP', 'corruption', 'in Mordor', 'Shadow dice', 'FP dice', 'Hunt dice set aside', 'Eyes rolled', 'Shadow units on the map', 'Shadow units still in Mordor', 'largest Shadow army', 'Shadow armies of 6+', 'FP units on the map', 'FP units besieged'):
        print('%-34s' % k + ''.join('%6.1f' % (sum(per[k][t]) / len(per[k][t])) if per[k][t] else '     -' for t in T))
    for s, title in (('SH', 'Shadow'), ('FP', 'Free Peoples')):
        names = [k for k, _ in sum((acts[s][t] for t in T), collections.Counter()).most_common()]
        print('\n%s actions per game, by turn (what each action die was spent on)' % title)
        for k in names: print('  %-32s' % k + ''.join('%6.2f' % (acts[s][t][k] / alive[t]) if alive[t] else '     -' for t in T))
    print('\nShadow die face -> what it was spent on (all turns)')
    for f, c in face_use.items():
        tot = sum(c.values()); print('  %-12s %4d: ' % (f, tot) + ', '.join('%s %.0f%%' % (k, 100 * v / tot) for k, v in c.most_common(5)))
    print('\nwhen things happen (turn: average, and in how many of the %d games)' % n)
    for w in MINIONS: print('  %-18s enters turn %.1f (%d games)' % (w, sum(minion[w]) / max(1, len(minion[w])), len(minion[w])))
    for w in POL: print('  %-18s at war turn %.1f (%d games)' % (w, sum(war[w]) / max(1, len(war[w])), len(war[w])))
    print('\nShadow captures (settlement: games, average turn); first capture of the game: ' + ', '.join('%s %d' % kv for kv in first_cap.most_common()))
    for r, ts in sorted(cap_turn.items(), key=lambda kv: -len(kv[1])): print('  %-16s %2d games, turn %.1f' % (r, len(ts), sum(ts) / len(ts)))
    print('\nShadow cards played most: ' + ', '.join('%s %d' % kv for kv in cards.most_common(14)))


if __name__ == '__main__':
    main(sys.argv[1])
