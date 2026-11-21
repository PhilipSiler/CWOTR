#!/usr/bin/env python3
"""Statistics on decoded human War of the Ring games (2nd edition base game).

Part 1 (this half): reading the decoded folders written by tools/humanlogs/decode.py and boiling every game down to a
compact summary, cached as one JSON line per game in cache/<set>.jsonl (no ids, names or chat: an anonymous running number only).
    iter_games(set_name)   one game at a time: dict(n, meta, events, turns)
    summarise(game)        the compact per-game dict (see its docstring for every field)
    build_cache(set_name)  one-time pass writing the summaries
    load(set_name, ...)    the cached summaries as a list
Part 2: one small function per table, each computed from the cache in seconds and printing the table with its n.

Command line:  humanstats.py build <set>      humanstats.py tables <set>      humanstats.py report
Never reads the raw logs: only <set>/decoded/*.events.jsonl, *.turns.jsonl and games.json.
"""
import json, os, sys, collections
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.environ.get('WOTR_HUMAN_DATA', os.path.join(HERE, '..', '..', 'data'))
TOOLS = os.environ.get('WOTR_HUMAN_TOOLS', HERE)
CACHE = os.path.join(DATA, 'cache')        # under data/, which is never committed
SETS = ('community', 'ladder2022', 'library', 'tourney2018')
sys.dont_write_bytecode = True
sys.path.insert(0, TOOLS)
import decode as D
MAP = set(v for k, v in D.MSG.items() if k.startswith('Game.') and 1881 <= int(k[5:]) <= 1996)
MAPX = MAP | set(m + ' Stronghold' for m in MAP) | {'Grey Havens Stronghold'}
MINIONS = ('Witch King', 'Saruman', 'Mouth of Sauron')
POL = ('Elves', 'Dwarves', 'North', 'Rohan', 'Gondor', 'S&E', 'Sauron', 'Isengard')
COMP = ('Gandalf', 'Strider', 'Legolas', 'Gimli', 'Boromir', 'Meriadoc', 'Peregrin')
RINGS = ('Vilya', 'Nenya', 'Narya')
SHN = ('ISENGARD', 'SAURON', 'SOUTHRON_EASTERLINGS'); FPN = ('THE_NORTH', 'ROHAN', 'GONDOR', 'ELVES', 'DWARVES')
SH_SETTLE = ('Orthanc', 'Moria', 'Dol Guldur', 'Mount Gundabad', 'Barad-Dur', 'Minas Morgul', 'Morannon', 'Umbar', 'Angmar', 'Far Harad', 'South Rhun')
SETTLE = tuple(D.VP)
LETTER = {'A': 'Army', 'C': 'Character', 'M': 'Muster', 'H': 'Muster/Army', 'P': 'Event', 'E': 'Eye', 'W': 'Will of the West'}

def gorgoroth(r, W):
    """The Mordor-track boxes are drawn on top of Gorgoroth: an army or Nazgul dropped on one of them is in Gorgoroth."""
    return 'Gorgoroth' if r.startswith('Mordor ') and W and (army_side(W[0]) or W[0] == 'NAZGUL' or W[0] in MINIONS) else r


def base(r): return r[:-11] if r.endswith(' Stronghold') else r
def onmap(r): return bool(r) and r in MAPX
def army_side(w):
    n = w.split()[0]
    return 'SH' if n in SHN else 'FP' if n in FPN else None
def won(r):
    r = r or ''
    if r.startswith('FP') or 'FP won' in r: return 'F'
    if r.startswith('SP') or 'SP won' in r: return 'S'
    return '?'

def classify(seg, s, names={}, infs=COMP):
    """seg: list of events of one action die of side s -> (cls, detail, info dict)"""
    info = {}
    card = None; ontable = False; draw = None; fsp = False; hide = False; pol = None; minion = None; newchar = None
    tablecard = None; tohunt = False; ownloss = False
    sep = []; compmove = False; naz = False; musters = []; moves = []; battle = False; ring = None; chg = None; nfl = False
    cardpid = None
    for e in seg[1:]:
        v = e['verb']; sd = e['side']; frm = e.get('from') or ''; to = e.get('to') or ''
        P = e.get('pieces', []); W = [p['what'].rstrip('*') for p in P]
        frm, to = gorgoroth(frm, W), gorgoroth(to, W)
        if v == 'plays card':
            if sd == s and card is None:
                card = e.get('card', '?'); cardpid = P[0]['id'] if P else None
                if to.startswith('Board'): ontable = True
            continue
        if v == 'chooses': battle = True; continue
        if v == 'changes die' and sd == s: chg = LETTER.get(e.get('face'), '?'); continue
        if v == 'draws':
            if 'Deck' in frm and 'hand' in to and sd == s and draw is None: draw = 'C' if 'Character' in frm else 'S'
            continue
        if v == 'flips':
            if sd == 'FP' and s == 'FP':
                if 'Fellowship' in W or 'FSP' in W: hide = True
                for c in ('Gandalf', 'Strider'):
                    if c in W and not P[W.index(c)]['what'].endswith('*'): newchar = c
            continue
        if v == 'kills':
            if frm != 'Current Action' and onmap(frm):
                if any(army_side(w) not in (None, s) for w in W): battle = True
                elif any(army_side(w) == s for w in W): ownloss = True
            continue
        if v != 'moves': continue
        if onmap(frm) and ('Reinforcements' in to or 'Casualties' in to):
            if any(army_side(w) not in (None, s) for w in W): battle = True
            elif any(army_side(w) == s for w in W): ownloss = True
        if frm.startswith('Board') and P and P[0]['id'] in names and tablecard is None: tablecard = names[P[0]['id']]
        if cardpid and P and P[0]['id'] == cardpid and to.startswith('Board'): ontable = True
        if any(w in RINGS for w in W) and ('Ring' in frm): ring = [w for w in W if w in RINGS][0]
        if W and 'action die' in W[0]:
            if s == 'FP' and frm == 'Current Action' and to == 'The Hunt For The Ring': fsp = True
            if s == 'SH' and frm == 'Current Action' and to == 'The Hunt For The Ring': tohunt = True
            continue
        if sd != s: continue
        if any(w in POL for w in W) and frm.startswith('Political') and to.startswith('Political'):
            try:
                nat = [w for w in W if w in POL][0]
                if int(to.split()[-1]) < int(frm.split()[-1]) and pol is None and (nat in ('S&E', 'Sauron', 'Isengard')) == (s == 'SH'): pol = nat
            except ValueError: pass
            continue
        if s == 'FP' and 'FSP' in W and frm.startswith('Fellowship Track') and to.startswith('Fellowship Track'):
            continue
        if s == 'SH' and any(w in MINIONS for w in W) and onmap(to) and not onmap(frm) and minion is None:
            minion = [w for w in W if w in MINIONS][0]; info['minion_at'] = base(to)
        if s == 'FP' and any(w in COMP for w in W) and onmap(to):
            cs = [w for w in W if w in COMP]
            if not onmap(frm) and frm != 'FP Casualties' and any(c in infs for c in cs): sep.append([[c for c in cs if c in infs], base(to)])
            elif onmap(frm) and base(frm) != base(to) and not any(army_side(w) for w in W):
                hop = [x for x in sep if x[1] == base(frm) and all(c in x[0] for c in cs)]
                if hop and len(hop[0][0]) == len(cs): hop[0][1] = base(to)       # the same group walking on within the separation
                elif hop: hop[0][0] = [c for c in hop[0][0] if c not in cs]; sep.append([cs, base(to)])
                else: compmove = True
        mine = [w for w in W if army_side(w) == s]
        if mine and onmap(to):
            if onmap(frm):
                if base(frm) != base(to): moves.append((base(frm), base(to), len(mine)))
            else:
                for w in mine: musters.append((base(to), w))
        elif s == 'SH' and 'NAZGUL' in W and onmap(to):
            if onmap(frm):
                if base(frm) != base(to): naz = True
            else: musters.append((base(to), 'NAZGUL'))
        if mine and frm in ('@reinforcements',) and onmap(to): pass
    info['ring'] = ring; info['chg'] = chg; info['ontable'] = ontable
    info['mustered'] = [w for _, w in musters]
    if sep:   # a later hop by the same companions replaces the first landing place
        info['sep'] = sep
    if card is not None: return 'card', card, info
    if fsp: return 'fsp', '', info
    if newchar: return 'newchar', newchar, info
    if minion: return 'minion', minion, info
    if sep: return 'sep', ';'.join('%s>%s' % ('+'.join(c), r) for c, r in sep), info
    if pol: return 'pol', pol, info
    if ownloss and not musters: battle = True      # own units swapped for others while mustering (Saruman's elites) is a muster, not a battle
    if moves or battle:
        seen = []; 
        for f, t, n in moves:
            if (f, t) not in seen: seen.append((f, t))
        det = '+'.join('%s>%s' % ft for ft in seen[:3])
        return ('attack' if battle else 'move'), det, info
    if musters:
        c = collections.Counter(musters)
        return 'muster', '+'.join('%s:%s%s' % (r, w, '' if n == 1 else 'x%d' % n) for (r, w), n in sorted(c.items())), info
    if draw: return 'draw', draw, info
    if compmove: return 'compmove', '', info
    if naz: return 'naz', '', info
    if hide: return 'hide', '', info
    if tablecard: return 'tablecard', tablecard, info
    if tohunt: return 'tohunt', '', info
    return 'other', '', info

def set_dir(set_name):
    """A set name (community, ladder2022, library, tourney2018) or a path to a decoded folder -> the decoded folder."""
    return set_name if os.path.isdir(set_name) and os.path.exists(os.path.join(set_name, 'games.json')) else os.path.join(DATA, set_name, 'decoded')


def iter_games(set_name, limit=None):
    """Yield one game at a time from a decoded folder: dict(n = anonymous running number, meta = the games.json entry
    without id and file name, events = list of decoded events, turns = list of end-of-turn boards). Streams: only one
    game is in memory at a time. With limit, an evenly spaced sample of that many games."""
    d = set_dir(set_name)
    games = json.load(open(os.path.join(d, 'games.json')))
    idx = list(range(len(games)))
    if limit: idx = idx[::max(1, len(games) // limit)][:limit]
    for n in idx:
        g = games[n]
        try:
            ev = [json.loads(l) for l in open(os.path.join(d, g['id'] + '.events.jsonl'))]
            tn = [json.loads(l) for l in open(os.path.join(d, g['id'] + '.turns.jsonl'))]
        except OSError: continue
        yield dict(n=n, meta={k: v for k, v in g.items() if k not in ('id', 'file')}, events=ev, turns=tn)


def summarise(G):
    """One game (from iter_games) -> a compact dict. Turn numbers are counted here as the number of Free Peoples
    action-dice rolls so far; things done between the last action of a turn and the next roll (declaring the Fellowship,
    entering Mordor, changing the guide) are booked to the turn about to start.
      n, res (sheet result code), win F/S/?, turns, hc (handicap: action tokens / dwarven rings / muster points), usable (stats.py's filter)
      T      per turn [turn, Shadow VP, FP VP, corruption, Mordor step or -1, Shadow units on map, FP units on map, FP units inside besieged strongholds, Fellowship piece region] at END of turn
      hunt   {turn: [dice the Shadow set aside for the Hunt before rolling, Eyes rolled, Shadow dice rolled, FP dice rolled, Shadow faces, FP faces]}
      acts   every action die spent: [turn, side 0 FP / 1 Shadow, die face, class, detail]; class is one of
             card (event card played; detail = title), draw (C/S deck), fsp (die placed in the Hunt box = a Fellowship move), hide,
             sep (companions leave the Fellowship; detail who>where), compmove, newchar (Gandalf the White / Aragorn), minion, pol (nation advanced),
             muster (detail region:unit), move (armies moved, no losses seen), attack (armies' die with units removed or combat cards chosen),
             naz (Nazgul/minions only), tablecard (a card on the table removed with a die), tohunt, other
      cards  [turn, side, title, E event with own die / C combat card (auto-revealed after 'chooses') / X other reveal, die face, 1 if it went onto the table]
      rings  [turn, user, ring, face of the die as rolled, face it was changed to if logged, class of the action it was used for]
      fsmoves one row per Fellowship move: [turn, k-th move of the turn, Eyes in the Hunt box (set aside + rolled), 1 if in Mordor, hunt tiles drawn, corruption change, companions removed, 1 if a Fellowship/FSP flip was logged, die face, -]
      fmoves the Fellowship figure's moves on the map [turn, context between/fsmove/shaction/fpaction/loose, from, to, steps so far, corruption, -]
      mordor entry: dict(turn, corr, comp = companions still in the Fellowship, who, steps, frm) or None
      seps [turn, companion, region, steps so far, in Mordor]; deaths [turn, companion, 'fellowship' or region]; guides [turn, new guide]
      caps [turn, settlement, by]; war {nation: turn it reached At War}; active {nation: turn its marker was first flipped}; minion {name: [turn, region]}
      newchar {Gandalf/Strider: turn first flipped = Gandalf the White / Aragorn}; table [turn, side, card placed on the table]
      lost / must {turn: [FP units, Shadow units]} removed from the map / brought on from reinforcements by an action die
    """
    g = G['meta']
    out = dict(n=G['n'], res=g.get('result'), win=won(g.get('result')), turns=g['turns'], sheet_turns=g.get('sheet_turns'),
               hc=bool(g.get('handicap')) if 'handicap' in g else (g.get('balancing') not in (None, 'Nope!')),
               tok=g.get('action_tokens'), dring=g.get('dwarven_rings'), mp=g.get('muster_points'), bal=g.get('balancing'),
               comp=g.get('competition') or g.get('competitive'), year=(g.get('date') or '')[:4],
               usable='expansion' not in g['module'] and g['turns'] >= 5 and g['moves_consistent'] > 100 and g['moves_inconsistent'] < 0.05 * g['moves_consistent'],
               end_svp=g['shadow_vp'], end_fvp=g['fp_vp'], end_corr=g['corruption'], end_mordor=g['mordor_step'], ended=g['ended'])
    # ---- end-of-turn boards
    T = []; held = set(); caps2 = []; lostback = []
    for t in G['turns']:
        tr = t['tracks']; b = t['board']
        shu = sum(sum(c for k, c in v.get('SH', {}).items() if k != 'NAZGUL') for r, v in b.items() if r in MAPX or r.startswith('Mordor '))
        fpu = sum(sum(v.get('FP', {}).values()) for r, v in b.items() if r in MAPX)
        fpb = sum(sum(v.get('FP', {}).values()) for r, v in b.items() if r.endswith('Stronghold') and r in MAPX)
        for r in SETTLE:      # who holds each settlement: enemy army alone in it, or an enemy control marker (kept once seen, until the owner's units stand there alone)
            v = b.get(r, {}); own = 'SH' if r in SH_SETTLE else 'FP'; foe = 'FP' if own == 'SH' else 'SH'
            box = b.get(('Grey Havens' if r == 'The Grey Havens' else r) + ' Stronghold', {})
            nf = sum(c for k, c in v.get(foe, {}).items() if k != 'NAZGUL'); no = sum(v.get(own, {}).values()) + sum(box.get(own, {}).values())
            mark = any(x == 'captured by ' + ('Shadow' if foe == 'SH' else 'Free Peoples') for x in v.get('other', []))
            if mark or (nf and not no):
                if r not in held: held.add(r); caps2.append([t['turn'], r, foe])
            elif no and not nf and r in held: held.discard(r); lostback.append([t['turn'], r])
        T.append([t['turn'], tr.get('Shadow VP', 0), tr.get('FP VP', 0), tr.get('corruption', 0), tr.get('Mordor step', -1), shu, fpu, fpb, tr.get('Fellowship last seen', '')])
    out['T'] = T; out['caps2'] = caps2; out['lostback'] = lostback; out['held_end'] = sorted(held)
    # ---- events
    turn = 0; between = True; seg = None; segs = []
    acts = []; cards = []; rings = []; fsmoves = []; fmoves = []; seps = []; deaths = []; guides = []; caps = []
    war = {}; active = {}; minion = {}; newchar = {}; prep = {}; hunt = {}
    infs = set(COMP); corr = 0; steps = 0; mordor = None; revealed = False; kfs = collections.Counter()
    disc = collections.Counter(); lost = collections.defaultdict(lambda: [0, 0]); must = collections.defaultdict(lambda: [0, 0]); mismatch = 0
    alloc = 0; shf = None; fpf = None
    comploc = {}; names = {}; table = []; sepseg = {}
    def close():
        nonlocal seg, steps
        if seg is None: return
        s = seg['s']; ev = seg['ev']
        # a die put back in the pool with nothing done: not an action
        last = ev[-1]
        cancelled = len(ev) <= 3 and any(e['verb'] == 'moves' and e.get('from') == 'Current Action' and (e.get('to') or '').endswith(' Dice') for e in ev[1:])
        if not cancelled:
            cls, det, info = classify(ev, s, names, seg['infs0'])
            t = seg['t']
            acts.append([t, 0 if s == 'FP' else 1, seg['die'], cls, det])
            if cls == 'card': cards.append([t, s, det, 'E', seg['die'], 1 if info['ontable'] else 0])
            if info.get('ring'): rings.append([t, s, info['ring'], seg['die'], info.get('chg'), cls])
            for w in info['mustered']: must[t][0 if s == 'FP' else 1] += 1
            if cls == 'fsp':
                tile = sum(1 for e in ev if e['verb'] == 'draws' and e.get('from') == 'Hunt Pool')
                dc = 0; cas = []
                for e in ev:
                    W = [p['what'].rstrip('*') for p in e.get('pieces', [])]
                    if e['verb'] == 'moves' and 'Corruption' in W and (e.get('from') or '').startswith('Fellowship Track') and (e.get('to') or '').startswith('Fellowship Track'):
                        dc += int(e['to'].split()[-1]) - int(e['from'].split()[-1])
                    if e['verb'] == 'kills': cas += [w for w in W if w in COMP]
                rev = any(e['verb'] == 'flips' and any(p['what'].rstrip('*') in ('Fellowship', 'FSP') for p in e.get('pieces', [])) for e in ev)
                kfs[t] += 1
                h = hunt.get(t, [0, 0])
                fsmoves.append([t, kfs[t], h[0] + h[1], 1 if seg['mordor'] else 0, tile, dc, '+'.join(cas), 1 if rev else 0, seg['die'], 1 if seg['rev0'] else 0])
                if not seg['mordor']: steps += 1
        seg = None
    for e in G['events']:
        v = e['verb']; sd = e['side']
        if sd == 'setup' and e['turn'] == 0 and v != 'plays card': continue
        P = e.get('pieces', []); W = [p['what'].rstrip('*') for p in P]
        frm = e.get('from') or ''; to = e.get('to') or ''
        frm, to = gorgoroth(frm, W), gorgoroth(to, W)
        isdie = bool(W) and 'action die' in W[0]
        if v == 'rolls' and isdie:
            close()
            if W[0].startswith('FP') and len(W) >= 3:
                turn += 1; fpf = e.get('faces', [])
                if e['turn'] != turn: mismatch += 1
            elif W[0].startswith('SH'): shf = e.get('faces', [])
            between = True
            continue
        if v == 'allocates':
            close(); between = True
            if to == 'The Hunt For The Ring': alloc += len(P)
            continue
        if v == 'moves' and isdie and W[0].startswith('SH') and (between or seg is None):     # Hunt dice dragged by hand instead of 'allocates'
            if frm == 'SA Dice' and to == 'The Hunt For The Ring': alloc += len(P); between = True; continue
            if frm == 'The Hunt For The Ring' and to == 'SA Dice' and alloc > 0: alloc = max(0, alloc - len(P)); continue
        if v == 'moves' and isdie and to == 'Current Action' and frm.endswith(' Dice'):
            close()
            if between:      # first action of the turn: fix the turn's dice
                hunt[turn] = [alloc, (shf or []).count('Eye'), len(shf or []), len(fpf or []), ''.join((x[0] if x != 'Muster/Army' else 'H') for x in (shf or [])), ''.join((x[0] if x != 'Muster/Army' else 'H') for x in (fpf or []))]
                alloc = 0; shf = None; fpf = None; between = False
            seg = dict(s='FP' if W[0].startswith('FP') else 'SH', t=turn, die=e.get('die', '?'), ev=[e], mordor=mordor is not None, rev0=revealed, infs0=set(infs))
            continue
        if v == 'moves' and isdie and frm in ('FP Casualties', 'SA Casualties', 'The Hunt For The Ring') and to.endswith(' Dice') and seg is None or \
           v == 'moves' and isdie and frm in ('FP Casualties', 'SA Casualties') and to.endswith(' Dice'):
            close(); between = True; continue
        if seg is not None: seg['ev'].append(e)
        tt = turn + 1 if between and turn >= 1 else turn       # things done between turns belong to the turn about to start
        if v == 'plays card' and P: names[P[0]['id']] = e.get('card', '?')
        if v in ('moves', 'plays card') and to.startswith('Board') and P and P[0]['id'] in names and not frm.startswith('Board'):
            table.append([turn, 'FP' if P[0]['what'].startswith('FP') else 'SH', names[P[0]['id']]])
        if v == 'plays card':
            if sd == 'setup' and P:
                cards.append([turn, 'FP' if P[0]['what'].startswith('FP') else 'SH', e.get('card', '?'), 'C', None, 0])
            elif seg is None or sd != seg['s'] or sum(1 for x in seg['ev'] if x['verb'] == 'plays card' and x['side'] == sd) > 1:
                cards.append([turn, sd, e.get('card', '?'), 'X', None, 0])       # outside the owner's own action, or a second card in it
            continue
        if v == 'kills':
            if frm in ('FP hand', 'Shadow hand'): disc[frm[:2]] += len(P)
            for p, w in zip(P, W):
                if w in COMP:
                    deaths.append([turn, w, 'fellowship' if w in infs else base(comploc.get(w, '?'))]); infs.discard(w)
                a = army_side(w)
                if a and frm != 'Current Action' and onmap(frm): lost[turn][0 if a == 'FP' else 1] += 1
            continue
        if v == 'moves' and onmap(frm) and ('Reinforcements' in to or 'Casualties' in to):
            for w in W:
                a = army_side(w)
                if a: lost[turn][0 if a == 'FP' else 1] += 1
        if v == 'flips':
            for p in P:
                w = p['what'].rstrip('*')
                if w == 'Fellowship': revealed = not p['what'].endswith('*')
                if w in ('Gandalf', 'Strider') and not p['what'].endswith('*') and w not in newchar and turn >= 1: newchar[w] = turn
                if w in POL and not p['what'].endswith('*') and w not in active and turn >= 1: active[w] = turn
            continue
        if v not in ('moves', 'draws'): continue
        if e.get('captures'):
            caps.append([turn, e['captures'], 'SH' if W[0].startswith('Shadow') else 'FP'])
        for w in W:
            if w in POL and to == 'Political Track 0' and w not in war: war[w] = turn
            if w in MINIONS and onmap(to) and w not in minion: minion[w] = [turn, base(to)]
            if w in COMP:
                if to == 'Guide Of The Fellowship' and frm != to: guides.append([tt, w])
                if onmap(to):
                    if w in infs and not onmap(frm) and frm != 'FP Casualties':
                        infs.discard(w); seps.append([turn, w, base(to), steps, 1 if mordor is not None else 0]); sepseg[w] = (seg['ev'][0]['line'] if seg else -1, len(seps) - 1)
                    elif w in sepseg and seg is not None and sepseg[w][0] == seg['ev'][0]['line'] and onmap(frm): seps[sepseg[w][1]][2] = base(to)
                    comploc[w] = to
            if w == 'Corruption' and to.startswith('Fellowship Track'):
                try: corr = int(to.split()[-1])
                except ValueError: pass
        if 'Fellowship' in W and v == 'moves':
            if to.startswith('Mordor '):
                k = int(to.split()[1])
                if mordor is None:
                    mordor = dict(turn=tt, corr=corr, comp=len(infs), who=sorted(infs), steps=steps, frm=base(frm), k=k, between=between)
            elif onmap(to) and onmap(frm):
                ctx = 'between' if between else ('fsmove' if seg is not None and seg['s'] == 'FP' and any(x['verb'] == 'moves' and x.get('to') == 'The Hunt For The Ring' for x in seg['ev']) else 'shaction' if seg is not None and seg['s'] == 'SH' else 'fpaction' if seg is not None else 'loose')
                if fmoves and fmoves[-1][1] == ctx and fmoves[-1][3] == base(frm) and fmoves[-1][0] == tt: fmoves[-1][3] = base(to)
                else: fmoves.append([tt, ctx, base(frm), base(to), steps, corr, 1 if revealed else 0])
    close()
    out.update(acts=acts, cards=cards, rings=rings, fsmoves=fsmoves, fmoves=fmoves, seps=seps, deaths=deaths, guides=guides, caps=caps, war=war, active=active,
               minion=minion, newchar=newchar, table=table, hunt=hunt, mordor=mordor, steps=steps, disc=dict(disc), lost=dict(lost), must=dict(must), myturns=turn, mismatch=mismatch)
    return out

def build_cache(set_name, limit=None):
    """One streaming pass over a set: writes cache/<set>.jsonl, one summarise() dict per line. Returns (written, failed)."""
    os.makedirs(CACHE, exist_ok=True)
    n = err = 0
    with open(os.path.join(CACHE, os.path.basename(set_name.rstrip('/')) + '.jsonl'), 'w') as f:
        for G in iter_games(set_name, limit):
            try: f.write(json.dumps(summarise(G), ensure_ascii=False, separators=(',', ':')) + '\n'); n += 1
            except Exception:
                err += 1
                if err < 4: import traceback; traceback.print_exc()
    return n, err


def load(set_name, usable=True, nohc=False):
    """The cached summaries of a set as a list; usable=True keeps games passing the usability filter of tools/humanlogs
    (base game, >= 5 turns, piece tracking consistent) that also have a recorded winner; nohc=True keeps no-handicap games only."""
    out = []
    for l in open(os.path.join(CACHE, set_name + '.jsonl')):
        g = json.loads(l)
        if usable and not (g['usable'] and g['win'] in 'FS'): continue
        if nohc and g['hc']: continue
        out.append(g)
    return out


# TABLES ---------------------------------------------------------------------------------------------------------
# Every function takes the list from load() and prints one table. "SH win" / "FP win" are shares of games won by that side
# among the games in the row; n is always the number of games (or of the stated unit) behind the figure.
import statistics
VTYPE = {'SPMV': 'Shadow military (10 VP)', 'SPRV': 'Shadow by corruption', 'FPRV': 'Ring destroyed', 'FPMV': 'Free Peoples military (4 VP)',
         'Conceded SP won': 'concession, Shadow won', 'Conceded FP won': 'concession, FP won'}
FPSET = ('The Grey Havens', 'Rivendell', 'Woodland Realm', 'Lorien', 'Erebor', "Helm's Deep", 'Minas Tirith', 'Dol Amroth', 'The Shire', 'Edoras', 'Dale', 'Pelargir')
SHSET = ('Orthanc', 'Moria', 'Dol Guldur', 'Mount Gundabad', 'Barad-Dur', 'Minas Morgul', 'Morannon', 'Umbar', 'Angmar', 'Far Harad', 'South Rhun')
BANDS = ((1, 3, 'turns 1-3'), (4, 6, 'turns 4-6'), (7, 10, 'turns 7-10'), (11, 99, 'turns 11+'))
GROUP = {'move': 'move armies (no losses seen)', 'attack': 'battle (losses or combat cards seen)', 'muster': 'muster units', 'card': 'play event card', 'draw': 'draw card',
         'fsp': 'move Fellowship', 'hide': 'hide Fellowship', 'pol': 'diplomacy (advance nation)', 'minion': 'bring in minion', 'newchar': 'Gandalf the White / Aragorn',
         'sep': 'separate companions', 'compmove': 'move companions', 'naz': 'move Nazgul / minions only', 'tablecard': 'remove a card on the table',
         'tohunt': 'other / nothing logged', 'other': 'other / nothing logged'}


def mean(x): return sum(x) / len(x) if x else float('nan')
def med(x): return statistics.median(x) if x else float('nan')
def pc(a, b): return '%5.1f%%' % (100.0 * a / b) if b else '    - '
def shw(gs): return pc(sum(g['win'] == 'S' for g in gs), len(gs))
def fpw(gs): return pc(sum(g['win'] == 'F' for g in gs), len(gs))
def Tmap(g): return {r[0]: r for r in g['T']}
def head(t): print('\n' + t); print('-' * min(len(t), 110))


def label(a, fine=False):
    """Plain name of one action [turn, side, die, class, detail]; fine=True keeps card titles and army routes."""
    c, d = a[3], a[4]
    if c == 'pol': return 'diplomacy ' + d
    if c == 'minion': return 'bring in ' + d
    if c == 'newchar': return 'Gandalf the White' if d == 'Gandalf' else 'Aragorn'
    if c == 'card': return 'card: ' + d if fine else 'play card'
    if c == 'draw': return 'draw ' + ('character' if d == 'C' else 'strategy') + ' card'
    if c == 'muster': return 'muster at ' + '+'.join(sorted(set(x.split(':')[0] for x in d.split('+')))) if fine else 'muster'
    if c in ('move', 'attack'): return ('battle ' if c == 'attack' else 'move ') + d if fine else ('battle' if c == 'attack' else 'army move')
    if c == 'fsp': return 'Fellowship move'
    if c == 'hide': return 'hide Fellowship'
    if c == 'sep': return 'separate ' + d if fine else 'separate companions'
    if c == 'compmove': return 'move companions'
    if c == 'naz': return 'move Nazgul'
    if c == 'tablecard': return 'remove table card ' + d
    return 'other / nothing logged'


def t_outcomes(G):
    """Section 1. Result = the result code reported for the game (SPMV Shadow military, SPRV corruption, FPRV Ring destroyed,
    FPMV FP military, or a reported concession). Length = number of Free Peoples action-dice rolls in the log (turn of the last action)."""
    n = len(G); head('1. OUTCOMES  (n = %d games)' % n)
    print('  Shadow wins %s   Free Peoples wins %s' % (shw(G), fpw(G)))
    print('  %-32s %6s %7s %10s %8s %8s' % ('result as reported', 'games', 'share', 'mean turn', 'median', '10%-90%'))
    for k, v in VTYPE.items():
        ts = sorted(g['turns'] for g in G if g['res'] == k)
        if ts: print('  %-32s %6d %7s %10.1f %8.1f %5d-%d' % (v, len(ts), pc(len(ts), n), mean(ts), med(ts), ts[len(ts) // 10], ts[len(ts) * 9 // 10]))
    ts = [g['turns'] for g in G]
    print('  all games: mean length %.2f turns, median %s' % (mean(ts), med(ts)))
    c = collections.Counter(min(t, 16) for t in ts)
    print('  game ends in turn:   ' + ' '.join('%5s' % (str(t) if t < 16 else '16+') for t in range(5, 17)))
    print('  share of games       ' + ' '.join('%5.1f' % (100 * c[t] / n) for t in range(5, 17)))
    for k in ('SPMV', 'FPRV', 'SPRV', 'FPMV'):
        gs = [g for g in G if g['res'] == k]; c = collections.Counter(min(g['turns'], 16) for g in gs)
        if gs: print('  %-20s ' % (VTYPE[k][:20]) + ' '.join('%5.1f' % (100 * c[t] / len(gs)) for t in range(5, 17)))
    print('  Shadow win rate by game length (turn the game ended):')
    for lo, hi in ((5, 6), (7, 7), (8, 8), (9, 9), (10, 10), (11, 11), (12, 12), (13, 14), (15, 99)):
        gs = [g for g in G if lo <= g['turns'] <= hi]
        print('    ended turn %-6s n=%5d  SH win %s' % ('%d-%d' % (lo, hi) if hi < 99 and lo != hi else '%d+' % lo if hi == 99 else str(lo), len(gs), shw(gs)))
    print('  by handicap given to the Free Peoples (action tokens / dwarven rings / muster points as reported):')
    for name, f in (('no handicap', lambda g: not g['hc']), ('any handicap', lambda g: g['hc']), ('1 action token only', lambda g: g.get('tok') == 1 and not g.get('dring') and not g.get('mp')),
                    ('2 action tokens only', lambda g: g.get('tok') == 2 and not g.get('dring') and not g.get('mp')), ('1+ dwarven ring (any tokens)', lambda g: (g.get('dring') or 0) >= 1)):
        gs = [g for g in G if f(g)]
        if gs: print('    %-30s n=%5d  SH win %s  mean length %.1f' % (name, len(gs), shw(gs), mean([g['turns'] for g in gs])))


def _turn_acts(g, side, lo, hi): return [a for a in g['acts'] if a[1] == side and lo <= a[0] <= hi]


def t_openings(G):
    """Section 2. An "action" is one action die taken from the pool and spent; its class comes from what its owner moved
    while the die was in play (see summarise). Patterns are the unordered set of a side's turn-1 actions. Win rates are of the
    games in the row. Hunt dice = dice the Shadow put in the Hunt box before rolling (rolled Eyes are extra)."""
    n = len(G); base = shw(G)
    head('2. OPENINGS  (n = %d games; baseline Shadow win %s, FP win %s)' % (n, base, fpw(G)))
    for side, nm, wf, wn in ((1, 'Shadow', shw, 'SH win'), (0, 'Free Peoples', fpw, 'FP win')):
        first = collections.defaultdict(list); pat = collections.defaultdict(list); fine1 = collections.defaultdict(list)
        ind = collections.defaultdict(list); ind2 = collections.defaultdict(list); nact = collections.Counter()
        for g in G:
            A = _turn_acts(g, side, 1, 1)
            if not A: continue
            first[label(A[0])].append(g); fine1[label(A[0], True)].append(g)
            pat[tuple(sorted(set(label(a) for a in A)))].append(g)
            for tag, B, dst in (('turn 1', A, ind), ('turns 1-2', _turn_acts(g, side, 1, 2), ind2)):
                labs = collections.Counter(label(a) for a in B)
                fl = collections.Counter(label(a, True) for a in B if a[3] in ('muster', 'card', 'sep'))
                for a in B:
                    if a[3] in ('move', 'attack') and a[4]:
                        for rt in a[4].split('+'): fl['army ' + rt.replace('>', ' -> ')] += 1
                for k in labs: dst[k].append(g)
                for k, v in labs.items():
                    if v >= 2 and k in ('Fellowship move', 'draw character card', 'draw strategy card', 'army move', 'play card', 'muster'): dst['%s x%d%s' % (k, min(v, 3), '+' if v >= 3 else '')].append(g)
                if not labs['Fellowship move'] and side == 0: dst['no Fellowship move'].append(g)
                for k in fl:
                    if tag == 'turn 1': dst[k].append(g)
        print('\n  %s: FIRST action of turn 1 (top 12)%40s' % (nm, 'games  share  ' + wn))
        for k, gs in sorted(first.items(), key=lambda kv: -len(kv[1]))[:12]: print('    %-62s %6d %s %s' % (k, len(gs), pc(len(gs), n), wf(gs)))
        print('  %s: first action in detail (top 14)' % nm)
        for k, gs in sorted(fine1.items(), key=lambda kv: -len(kv[1]))[:14]: print('    %-62s %6d %s %s' % (k[:62], len(gs), pc(len(gs), n), wf(gs)))
        print('  %s: whole turn 1 as the set of different things done, repeats ignored (top 16; depends on the dice rolled)' % nm)
        for k, gs in sorted(pat.items(), key=lambda kv: -len(kv[1]))[:16]: print('    %-100s %5d %s %s' % (', '.join(k)[:100], len(gs), pc(len(gs), n), wf(gs)))
        print('  %s: games in which turn 1 contains at least one ... (n >= 1%% of games)' % nm)
        for k, gs in sorted(ind.items(), key=lambda kv: -len(kv[1])):
            if len(gs) >= n / 100: print('    %-62s %6d %s %s' % (k[:62], len(gs), pc(len(gs), n), wf(gs)))
        print('  %s: by the end of turn 2, at least one ...' % nm)
        for k, gs in sorted(ind2.items(), key=lambda kv: -len(kv[1])):
            if len(gs) >= n / 50: print('    %-62s %6d %s %s' % (k[:62], len(gs), pc(len(gs), n), wf(gs)))
    print('\n  Hunt dice the Shadow sets aside before rolling, turns 1-5 (share of games; then Shadow win rate by the turn-1..3 choice)')
    print('    %-8s %7s %7s %7s %7s %7s   mean   mean Eyes rolled' % ('turn', '0', '1', '2', '3', '4+'))
    for t in range(1, 6):
        hs = [g['hunt'][str(t)] for g in G if str(t) in g['hunt']]
        c = collections.Counter(min(h[0], 4) for h in hs)
        print('    %-8d ' % t + ' '.join(pc(c[k], len(hs)) for k in range(5)) + '  %.2f   %.2f   (n=%d)' % (mean([h[0] for h in hs]), mean([h[1] for h in hs]), len(hs)))
    for t in (1, 2, 3):
        for k in range(0, 3):
            gs = [g for g in G if str(t) in g['hunt'] and min(g['hunt'][str(t)][0], 2) == k]
            print('    turn %d, %s set aside: n=%5d  SH win %s' % (t, ('0', '1', '2+')[k], len(gs), shw(gs)))


def t_dice(G):
    """Section 3. Every action die spent, grouped by turn band and side: share of the side's dice in that band by what the
    die did (classes as in summarise; "battle" = an army die action in which units were removed or combat cards were chosen,
    so bloodless battles count as moves). Dice per turn = dice spent / game-turns played in the band. Second table: use by die face."""
    head('3. ACTION DICE USE BY TURN BAND  (n = %d games)' % len(G))
    for side, nm in ((1, 'Shadow'), (0, 'Free Peoples')):
        cnt = {b: collections.Counter() for b in BANDS}; gt = collections.Counter()
        for g in G:
            for b in BANDS: gt[b] += max(0, min(g['turns'], b[1]) - b[0] + 1)
            for a in g['acts']:
                if a[1] != side: continue
                for b in BANDS:
                    if b[0] <= a[0] <= b[1]: cnt[b][GROUP[a[3]]] += 1
        names = [k for k, _ in sum(cnt.values(), collections.Counter()).most_common()]
        print('\n  %-40s' % nm + ''.join('%14s' % b[2] for b in BANDS))
        print('  %-40s' % 'game-turns in band' + ''.join('%14d' % gt[b] for b in BANDS))
        print('  %-40s' % 'dice spent per turn' + ''.join('%14.2f' % (sum(cnt[b].values()) / gt[b] if gt[b] else 0) for b in BANDS))
        for k in names: print('  %-40s' % k + ''.join('%13.1f%%' % (100 * cnt[b][k] / max(1, sum(cnt[b].values()))) for b in BANDS))
    for side, nm in ((1, 'Shadow'), (0, 'Free Peoples')):
        use = collections.defaultdict(collections.Counter)
        for g in G:
            for a in g['acts']:
                if a[1] == side: use[a[2]][GROUP[a[3]]] += 1
        print('\n  %s die face -> what it was spent on (all turns; face as rolled, before any Elven-ring change)' % nm)
        for f, c in sorted(use.items(), key=lambda kv: -sum(kv[1].values())):
            tot = sum(c.values())
            if tot >= 500: print('    %-17s n=%7d: ' % (f, tot) + ', '.join('%s %.0f%%' % (k, 100 * v / tot) for k, v in c.most_common(6)))


def t_vp(G):
    """Section 4. VP = position of each side's victory-point marker at the end of each turn (the marker is moved by hand, so the
    last blow of a game is sometimes never recorded). Captures = a control marker laid on a settlement; the turn is the turn of the lay.
    "Still going" = games whose log has that turn."""
    n = len(G); head('4. VICTORY POINTS AND SETTLEMENTS  (n = %d games)' % n)
    TT = range(1, 15)
    print('  after turn                   ' + ''.join('%6d' % t for t in TT))
    rows = [('all games', lambda g: True), ('Shadow military wins', lambda g: g['res'] == 'SPMV'), ('corruption wins', lambda g: g['res'] == 'SPRV'), ('Ring destroyed', lambda g: g['res'] == 'FPRV'), ('FP military wins', lambda g: g['res'] == 'FPMV')]
    for col, nm in ((1, 'Shadow VP'), (2, 'FP VP')):
        for rn, f in rows:
            gs = [Tmap(g) for g in G if f(g)]
            print('  %-30s' % ('%s, %s' % (nm, rn))[:30] + ''.join(('%6.2f' % mean([m[t][col] for m in gs if t in m])) if sum(t in m for m in gs) >= 30 else '     -' for t in TT))
    print('  %-30s' % 'games still going' + ''.join('%6d' % sum(g['turns'] >= t for g in G) for t in TT))
    print('  Shadow military wins: turn the Shadow first holds >= k VP (cumulative share of those games, n=%d)' % sum(g['res'] == 'SPMV' for g in G))
    for k in (2, 4, 6, 8):
        ts = []
        for g in G:
            if g['res'] != 'SPMV': continue
            ts.append(next((r[0] for r in g['T'] if r[1] >= k), 99))
        print('    >= %d VP by end of turn: ' % k + ' '.join('t%d %s' % (t, pc(sum(x <= t for x in ts), len(ts)).strip()) for t in range(2, 11)) + '   median %s' % med([x for x in ts if x < 99]))
    for by, SET, nm in (('SH', FPSET, 'Free Peoples settlements taken by the Shadow'), ('FP', SHSET, 'Shadow settlements taken by the Free Peoples')):
        first = collections.defaultdict(list); order = collections.Counter(); firstone = collections.Counter(); wins = collections.defaultdict(list)
        for g in G:
            seen = []
            for t, r, b in g['caps2']:
                if b == by and r in SET and r not in seen: seen.append(r); first[r].append(t); wins[r].append(g)
            if seen: firstone[seen[0]] += 1
            if len(seen) >= 3: order[tuple(seen[:3])] += 1
        print('\n  %s (first capture per game)' % nm)
        print('    %-18s %6s %8s %10s %7s %9s %14s  %s' % ('settlement', 'games', 'of games', 'mean turn', 'median', '10%-90%', 'first to fall', 'taker wins'))
        for r, ts in sorted(first.items(), key=lambda kv: -len(kv[1])):
            ts.sort(); w = shw(wins[r]) if by == 'SH' else fpw(wins[r])
            print('    %-18s %6d %8s %10.1f %7.1f %6d-%-2d %8d %s  %s' % (r, len(ts), pc(len(ts), n), mean(ts), med(ts), ts[len(ts) // 10], ts[len(ts) * 9 // 10], firstone[r], pc(firstone[r], n), w))
        print('    no capture at all: %d games (%s)' % (n - sum(firstone.values()), pc(n - sum(firstone.values()), n)))
        print('    most common first three, in order: ' + '; '.join('%s (%d)' % (' > '.join(k), v) for k, v in order.most_common(8)))
    gs = [g for g in G if g['res'] == 'SPMV' and g['end_svp'] >= 10]
    print('\n  Shadow military wins whose log reaches 10 VP (n=%d): which settlements the Shadow had captured' % len(gs))
    c = collections.Counter(); sets = collections.Counter(); ns = []
    gs = [g for g in gs if sum(D.VP[r] for r in g['held_end'] if r in FPSET) == g['end_svp']]
    print('    (kept: the %d games where the settlements seen in Shadow hands on the final board add up exactly to the final VP marker)' % len(gs))
    for g in gs:
        s = [r for r in g['held_end'] if r in FPSET]
        for r in s: c[r] += 1
        sets[tuple(sorted(s))] += 1; ns.append(len(s))
    print('    ' + ', '.join('%s %s' % (r, pc(v, len(gs)).strip()) for r, v in c.most_common()))
    print('    number of settlements captured: ' + ', '.join('%d: %s' % (k, pc(v, len(gs)).strip()) for k, v in sorted(collections.Counter(ns).items())))
    for reg, nm in ((('Woodland Realm', 'Dale', 'Erebor'), 'north-east (Woodland Realm, Dale, Erebor; 5 VP)'), (('Lorien',), 'Lorien (2 VP)'), (('Rivendell', 'The Grey Havens', 'The Shire'), 'north-west (Rivendell, Grey Havens, Shire; 5 VP)'), ((("Helm's Deep", 'Edoras')), 'Rohan (Helm\'s Deep, Edoras; 3 VP)'), (('Minas Tirith', 'Dol Amroth', 'Pelargir'), 'Gondor (Minas Tirith, Dol Amroth, Pelargir; 5 VP)')):
        v = [sum(D.VP[r] for r in g['held_end'] if r in reg) for g in gs]
        print('    VP from %-66s mean %.2f; 0 VP in %s, all of them in %s' % (nm + ':', mean(v), pc(sum(x == 0 for x in v), len(v)).strip(), pc(sum(x == sum(D.VP[r] for r in reg) for x in v), len(v)).strip()))
    print('    most common complete sets: ')
    for k, v in sets.most_common(12): print('      %4d (%s)  %s' % (v, pc(v, len(gs)).strip(), ', '.join(k)))


def _cond(G, name, turn_of, bins, wf=shw, wn='SH win'):
    """Rows of a 'happens by turn' table: share of games and win rate in each bin of turn_of(g) (None = never)."""
    ts = [turn_of(g) for g in G]; have = sorted(t for t in ts if t is not None)
    line = '    %-22s in %s of games, mean turn %4.1f, median %4.1f |' % (name, pc(len(have), len(G)).strip(), mean(have), med(have))
    for lab, lo, hi in bins:
        gs = [g for g, t in zip(G, ts) if t is not None and lo <= t <= hi]
        line += ' %s: n=%d %s %s |' % (lab, len(gs), wn, wf(gs).strip())
    gs = [g for g, t in zip(G, ts) if t is None]
    line += ' never: n=%d %s %s' % (len(gs), wn, wf(gs).strip())
    print(line)


def t_tempo(G):
    """Section 5. At war = the nation's political marker reaches the last box (turn of that move). Minions = the turn the figure is first
    placed on the map; Gandalf the White / Aragorn = the turn the Gandalf / Strider figure is first flipped. "never" rows are mostly
    short games, so compare the early and late bins with each other, not with never."""
    n = len(G); head('5. TEMPO MARKERS  (n = %d games; baseline Shadow win %s)' % (n, shw(G)))
    print('  Nations at war (Shadow win rate in each bin)')
    for nat, bins in (('Isengard', (('t1', 1, 1), ('t2', 2, 2), ('t3+', 3, 99))), ('Sauron', (('t1', 1, 1), ('t2', 2, 2), ('t3+', 3, 99))), ('S&E', (('t1-2', 1, 2), ('t3-4', 3, 4), ('t5+', 5, 99))),
                      ('Elves', (('t1-3', 1, 3), ('t4-5', 4, 5), ('t6+', 6, 99))), ('North', (('t1-3', 1, 3), ('t4-5', 4, 5), ('t6+', 6, 99))), ('Gondor', (('t1-3', 1, 3), ('t4-5', 4, 5), ('t6+', 6, 99))),
                      ('Rohan', (('t1-4', 1, 4), ('t5-7', 5, 7), ('t8+', 8, 99))), ('Dwarves', (('t1-4', 1, 4), ('t5-7', 5, 7), ('t8+', 8, 99)))):
        _cond(G, nat, lambda g: g['war'].get(nat), bins)
    print('  Characters (Shadow win rate in each bin)')
    for who, bins in (('Saruman', (('t1', 1, 1), ('t2', 2, 2), ('t3+', 3, 99))), ('Witch King', (('t1-2', 1, 2), ('t3', 3, 3), ('t4', 4, 4), ('t5+', 5, 99))), ('Mouth of Sauron', (('t1-5', 1, 5), ('t6-8', 6, 8), ('t9+', 9, 99)))):
        _cond(G, who, lambda g: (g['minion'].get(who) or [None])[0], bins)
    for who, nm, bins in (('Gandalf', 'Gandalf the White', (('t1-3', 1, 3), ('t4-5', 4, 5), ('t6-7', 6, 7), ('t8+', 8, 99))), ('Strider', 'Aragorn', (('t1-4', 1, 4), ('t5-6', 5, 6), ('t7-8', 7, 8), ('t9+', 9, 99)))):
        _cond(G, nm, lambda g: g['newchar'].get(who), bins)
    print('  Same, restricted to games lasting at least 9 turns (n=%d, Shadow win %s) to take game length out' % (sum(g['turns'] >= 9 for g in G), shw([g for g in G if g['turns'] >= 9])))
    L = [g for g in G if g['turns'] >= 9]
    _cond(L, 'Witch King', lambda g: (g['minion'].get('Witch King') or [None])[0], (('t1-2', 1, 2), ('t3', 3, 3), ('t4', 4, 4), ('t5+', 5, 99)))
    _cond(L, 'Mouth of Sauron', lambda g: (g['minion'].get('Mouth of Sauron') or [None])[0], (('t1-5', 1, 5), ('t6-8', 6, 8), ('t9+', 9, 99)))
    _cond(L, 'Gandalf the White', lambda g: g['newchar'].get('Gandalf'), (('t1-3', 1, 3), ('t4-5', 4, 5), ('t6-7', 6, 7), ('t8+', 8, 99)))
    _cond(L, 'Aragorn', lambda g: g['newchar'].get('Strider'), (('t1-4', 1, 4), ('t5-6', 5, 6), ('t7-8', 7, 8), ('t9+', 9, 99)))
    for nat in ('Elves', 'North', 'Gondor', 'Rohan', 'Dwarves'): _cond(L, nat, lambda g: g['war'].get(nat), (('t1-4', 1, 4), ('t5-6', 5, 6), ('t7+', 7, 99)))
    print('  Where the minions first appear: ' + ' | '.join('%s: %s' % (w, ', '.join('%s %s' % (r, pc(v, sum(1 for g in G if w in g['minion'])).strip()) for r, v in collections.Counter(g['minion'][w][1] for g in G if w in g['minion']).most_common(5))) for w in MINIONS))
    print('  Turn distribution (share of all games):')
    for nm, f in (('Saruman', lambda g: (g['minion'].get('Saruman') or [None])[0]), ('Witch King', lambda g: (g['minion'].get('Witch King') or [None])[0]), ('Mouth of Sauron', lambda g: (g['minion'].get('Mouth of Sauron') or [None])[0]),
                  ('Gandalf the White', lambda g: g['newchar'].get('Gandalf')), ('Aragorn', lambda g: g['newchar'].get('Strider'))):
        c = collections.Counter(min(f(g), 10) for g in G if f(g) is not None)
        print('    %-18s ' % nm + ' '.join('t%s %4.1f' % (str(t) if t < 10 else '10+', 100 * c[t] / n) for t in range(1, 11)))


def t_fellowship(G):
    """Section 6. A Fellowship move = an FP action die placed in the Hunt box. Steps = Fellowship moves made outside Mordor (the
    progress counter itself is reset by hand and is not reliable). Eyes in the box = dice set aside + Eyes rolled that turn (FP dice
    already in the box are not counted). "Hunt hits" = at least one hunt tile was drawn during the move (tile values are not in the logs;
    the corruption change is the movement of the corruption marker during the move). Entering Mordor = the Fellowship figure is put on
    the first Mordor step; companions at entry = companions not yet separated or removed. Declared = the figure is moved on the map
    between two turns (not into Mordor)."""
    n = len(G); head('6. THE FELLOWSHIP  (n = %d games)' % n)
    TT = range(1, 15)
    F = [g for g in G if g['win'] == 'F']; S = [g for g in G if g['win'] == 'S']; R = [g for g in G if g['res'] == 'FPRV']
    def cum(g, t): return sum(1 for m in g['fsmoves'] if m[0] <= t and not m[3])
    print('  after turn                         ' + ''.join('%6d' % t for t in TT))
    for nm, gs in (('all', G), ('FP wins', F), ('Shadow wins', S), ('Ring destroyed', R)):
        print('  %-36s' % ('Fellowship moves that turn, ' + nm)[:36] + ''.join('%6.2f' % mean([sum(1 for m in g['fsmoves'] if m[0] == t) for g in gs if g['turns'] >= t]) if sum(g['turns'] >= t for g in gs) >= 30 else '     -' for t in TT))
    for nm, gs in (('all', G), ('FP wins', F), ('Shadow wins', S), ('Ring destroyed', R)):
        print('  %-36s' % ('steps so far (outside Mordor), ' + nm)[:36] + ''.join('%6.2f' % mean([cum(g, t) for g in gs if g['turns'] >= t]) if sum(g['turns'] >= t for g in gs) >= 30 else '     -' for t in TT))
    for nm, gs in (('all', G), ('FP wins', F), ('Shadow wins', S), ('Ring destroyed', R), ('corruption wins', [g for g in G if g['res'] == 'SPRV'])):
        ms = [Tmap(g) for g in gs]
        print('  %-36s' % ('corruption, ' + nm) + ''.join('%6.2f' % mean([m[t][3] for m in ms if t in m]) if sum(t in m for m in ms) >= 30 else '     -' for t in TT))
    for nm, gs in (('all', G), ('Ring destroyed', R)):
        print('  %-36s' % ('share already in Mordor, ' + nm) + ''.join('%6.2f' % mean([1 if g['mordor'] and g['mordor']['turn'] - (1 if g['mordor']['between'] else 0) <= t else 0 for g in gs if g['turns'] >= t]) if sum(g['turns'] >= t for g in gs) >= 30 else '     -' for t in TT))
    print('  share of FP dice / game-turn spent hiding: mean hides per turn ' + ' '.join('t%d %.2f' % (t, mean([sum(1 for a in g['acts'] if a[0] == t and a[3] == 'hide') for g in G if g['turns'] >= t])) for t in range(1, 11)))
    # by eyes
    print('\n  Outside Mordor: Fellowship moves in a turn by Eyes in the Hunt box that turn (game-turns before the entry turn, last turn of the game left out)')
    by = collections.defaultdict(list)
    for g in G:
        last = min(g['turns'] - 1, (g['mordor']['turn'] - 1) if g['mordor'] else 99)
        k = collections.Counter(m[0] for m in g['fsmoves'])
        for t in range(1, last + 1):
            if str(t) in g['hunt']: by[min(g['hunt'][str(t)][0] + g['hunt'][str(t)][1], 5)].append(k[t])
    print('    %-12s %9s %7s   share of turns with 0 / 1 / 2 / 3+ moves' % ('Eyes in box', 'turns', 'mean'))
    for e in sorted(by):
        v = by[e]; c = collections.Counter(min(x, 3) for x in v)
        print('    %-12s %9d %7.2f   %s' % (str(e) if e < 5 else '5+', len(v), mean(v), ' / '.join(pc(c[i], len(v)).strip() for i in range(4))))
    print('\n  Outside Mordor: how often the Hunt hits (a tile is drawn), by Eyes in the box and by which move of the turn it is')
    hit = collections.defaultdict(lambda: [0, 0, 0, 0])
    for g in G:
        for m in g['fsmoves']:
            if m[3]: continue
            h = hit[(min(m[2], 4), min(m[1], 4))]; h[0] += 1; h[1] += m[4] > 0
            if m[4] > 0: h[2] += m[5]; h[3] += 1 if m[6] else 0
    print('    %-10s' % 'Eyes' + ''.join('%26s' % ('move %s of the turn' % (str(k) if k < 4 else '4+')) for k in range(1, 5)))
    for e in range(0, 5):
        print('    %-10s' % (str(e) if e < 4 else '4+') + ''.join('%15s (n=%6d)' % (pc(hit[(e, k)][1], hit[(e, k)][0]).strip(), hit[(e, k)][0]) for k in range(1, 5)))
    tot = [sum(h[i] for h in hit.values()) for i in range(4)]
    print('    all moves outside Mordor: n=%d, Hunt hits %s; per hit: corruption marker moves %+.2f on average, a companion is removed in %s of hits' % (tot[0], pc(tot[1], tot[0]).strip(), tot[2] / max(1, tot[1]), pc(tot[3], tot[1]).strip()))
    mm = [m for g in G for m in g['fsmoves'] if m[3]]
    print('    moves in Mordor: n=%d, corruption marker moves %+.2f per move on average; 0 corruption in %s, companion removed in %s' % (len(mm), mean([m[5] for m in mm]), pc(sum(m[5] <= 0 for m in mm), len(mm)).strip(), pc(sum(1 for m in mm if m[6]), len(mm)).strip()))
    # Mordor entry
    M = [g for g in G if g['mordor']]
    print('\n  Entering Mordor: %d games (%s of all); mean turn %.1f, median %s; mean corruption at entry %.1f; mean companions still in the Fellowship %.1f; mean steps taken %.1f' % (
        len(M), pc(len(M), n).strip(), mean([g['mordor']['turn'] for g in M]), med([g['mordor']['turn'] for g in M]), mean([g['mordor']['corr'] for g in M]), mean([g['mordor']['comp'] for g in M]), mean([g['mordor']['steps'] for g in M])))
    print('    %-34s %6s %8s %16s %10s %12s' % ('', 'games', 'share', 'Ring destroyed', 'FP wins', 'corruption loss'))
    def row(nm, gs): print('    %-34s %6d %8s %16s %10s %12s' % (nm, len(gs), pc(len(gs), len(M)), pc(sum(g['res'] == 'FPRV' for g in gs), len(gs)), fpw(gs), pc(sum(g['res'] == 'SPRV' for g in gs), len(gs))))
    row('all games that entered', M); row('games that never entered', [g for g in G if not g['mordor']])
    for t in range(4, 14): row('entered at start of turn %s' % (str(t) if t < 13 else '13+'), [g for g in M if (g['mordor']['turn'] == t if t < 13 else g['mordor']['turn'] >= 13) or (t == 4 and g['mordor']['turn'] < 4)])
    for c in range(0, 10): row('corruption at entry %s' % (str(c) if c < 9 else '9+'), [g for g in M if (g['mordor']['corr'] == c if c < 9 else g['mordor']['corr'] >= 9)])
    for c in range(0, 7): row('companions at entry %s' % (str(c) if c < 6 else '6-7'), [g for g in M if (g['mordor']['comp'] == c if c < 6 else g['mordor']['comp'] >= 6)])
    print('    Ring destroyed rate by corruption at entry (rows) and entry turn (columns): rate (n)')
    cb = ((0, 1, '0-1'), (2, 3, '2-3'), (4, 5, '4-5'), (6, 7, '6-7'), (8, 12, '8+')); tb = ((1, 6, 'by t6'), (7, 7, 't7'), (8, 8, 't8'), (9, 9, 't9'), (10, 10, 't10'), (11, 99, 't11+'))
    print('    %-10s' % 'corruption' + ''.join('%16s' % x[2] for x in tb))
    for c0, c1, cn in cb:
        line = '    %-10s' % cn
        for t0, t1, tn in tb:
            gs = [g for g in M if c0 <= g['mordor']['corr'] <= c1 and t0 <= g['mordor']['turn'] <= t1]
            line += '%9s (%4d)' % (pc(sum(g['res'] == 'FPRV' for g in gs), len(gs)).strip(), len(gs))
        print(line)
    who = collections.Counter(w for g in M for w in g['mordor']['who'])
    print('    companions still with the Ring at entry: ' + ', '.join('%s %s' % (w, pc(v, len(M)).strip()) for w, v in who.most_common()))
    d = [g['turns'] - g['mordor']['turn'] + 1 for g in R if g['mordor']]
    print('    Ring-destroyed games: turns spent in Mordor (entry turn to last turn inclusive) mean %.2f; ' % mean(d) + ', '.join('%d: %s' % (k, pc(v, len(d)).strip()) for k, v in sorted(collections.Counter(min(x, 6) for x in d).items())))
    # declarations
    dec = [(g, m) for g in G for m in g['fmoves'] if m[1] == 'between']
    per = collections.Counter(sum(1 for m in g['fmoves'] if m[1] == 'between') for g in G)
    print('\n  Declaring the Fellowship (figure moved on the map between turns): %d declarations' % len(dec))
    pk = collections.Counter(min(k, 4) for k in per.elements())
    print('    declarations per game: ' + ', '.join('%s: %s' % (str(k) if k < 4 else '4+', pc(pk[k], n).strip()) for k in range(5)))
    c = collections.Counter(m[3] for g, m in dec)
    fpcs = set(FPSET)
    incity = sum(v for r, v in c.items() if r in fpcs)
    print('    into a Free Peoples city or stronghold: %s of declarations; destinations: ' % pc(incity, len(dec)).strip() + ', '.join('%s %s' % (r, pc(v, len(dec)).strip()) for r, v in c.most_common(12)))
    tc = collections.Counter(min(m[0], 10) for g, m in dec)
    print('    at the start of turn: ' + ' '.join('t%s %s' % (str(t) if t < 10 else '10+', pc(tc[t], len(dec)).strip()) for t in range(2, 11)))
    for r in ('Lorien', 'Rivendell', 'Minas Tirith', 'Dol Amroth', "Helm's Deep", 'Woodland Realm', 'Edoras'):
        ds = [(g, m) for g, m in dec if m[3] == r]
        if len(ds) >= 30: print('    declared in %-15s n=%5d  mean turn %.1f, mean steps so far %.1f, mean corruption then %.1f; FP win %s' % (r, len(ds), mean([m[0] for g, m in ds]), mean([m[4] for g, m in ds]), mean([m[5] for g, m in ds]), fpw([g for g, m in ds])))
    gl = [g for g in G if any(m[1] == 'between' and m[3] == 'Lorien' for m in g['fmoves'])]
    print('    games with a declaration in Lorien: n=%d FP win %s; without: n=%d FP win %s' % (len(gl), fpw(gl), n - len(gl), fpw([g for g in G if g not in gl]) if n < 6000 else pc(sum(g['win'] == 'F' for g in G) - sum(g['win'] == 'F' for g in gl), n - len(gl))))
    # guides, separations, deaths
    gc = collections.Counter(w for g in G for t, w in g['guides'])
    print('\n  Guide changes: %.2f per game; new guide: ' % (sum(gc.values()) / n) + ', '.join('%s %d' % kv for kv in gc.most_common()))
    print('  Companions leaving the Fellowship alive (separated, or Merry/Pippin sent away instead of dying), and removed while still in it')
    print('    %-10s %9s %9s %10s %8s %10s %9s   %s' % ('companion', 'separated', 'of games', 'mean turn', 'median', 'removed in', 'died on', 'top landing places'))
    print('    %-10s %9s %9s %10s %8s %10s %9s' % ('', '', '', '', '', 'Fellowship', 'the map'))
    for w in COMP:
        ss = [(g, s) for g in G for s in g['seps'] if s[1] == w]
        dst = collections.Counter(s[2] for g, s in ss)
        df = sum(1 for g in G for d in g['deaths'] if d[1] == w and d[2] == 'fellowship'); dm = sum(1 for g in G for d in g['deaths'] if d[1] == w and d[2] != 'fellowship')
        print('    %-10s %9d %9s %10.1f %8.1f %10s %9s   %s' % (w, len(ss), pc(len(ss), n), mean([s[0] for g, s in ss]), med([s[0] for g, s in ss]), pc(df, n).strip(), pc(dm, n).strip(), ', '.join('%s %s' % (r, pc(v, len(ss)).strip()) for r, v in dst.most_common(5))))
    sa = collections.Counter(a[4] for g in G for a in g['acts'] if a[3] == 'sep')
    print('    most common separations done with an action die (who>where): ' + '; '.join('%s (%d)' % kv for kv in sa.most_common(12)))
    for w in ('Strider', 'Gandalf', 'Boromir', 'Legolas', 'Gimli', 'Meriadoc', 'Peregrin'):
        a = [g for g in G if any(s[1] == w for s in g['seps'])]
        print('    games where %-9s left the Fellowship alive: n=%5d FP win %s (others: n=%d FP win %s)' % (w, len(a), fpw(a), n - len(a), pc(sum(g['win'] == 'F' for g in G) - sum(g['win'] == 'F' for g in a), n - len(a))))
    fd = [d for g in G for d in g['deaths'] if d[2] == 'fellowship']
    print('    first companion removed from the Fellowship (per game): ' + ', '.join('%s %s' % (w, pc(v, n).strip()) for w, v in collections.Counter(next((d[1] for d in g['deaths'] if d[2] == 'fellowship'), 'none') for g in G).most_common()))
    print('    turn Gandalf (the Grey) is removed from the Fellowship: ' + ' '.join('t%s %s' % (str(t) if t < 9 else '9+', pc(v, n).strip()) for t, v in sorted(collections.Counter(min(d[0], 9) for g in G for d in g['deaths'] if d[1] == 'Gandalf' and d[2] == 'fellowship').items())))


def t_cards(G, minn=50):
    """Section 7. E = revealed from the hand by its owner while one of the owner's action dice is in play (played as an event);
    C = revealed automatically after both players 'choose' a combat card (played as a combat card; identified by the card's event title);
    X = revealed in any other way (during the opponent's action, or a second card in one action). Discards are face down and cannot
    be identified. "Adj. baseline" = the playing side's win rate among games that lasted at least to the turn of each play, averaged
    over the plays: the fair comparison, since long games see more cards. Differences are association, not cause."""
    n = len(G); head('7. EVENT CARDS  (n = %d games)' % n)
    surv = {}
    for side, w in (('SH', 'S'), ('FP', 'F')):
        for t in range(0, 40):
            gs = [g for g in G if g['turns'] >= t]; surv[(side, t)] = sum(g['win'] == w for g in gs) / max(1, len(gs))
    tot = collections.Counter((k[1], k[3]) for g in G for k in g['cards'])
    dr = collections.Counter(a[1] for g in G for a in g['acts'] if a[3] == 'draw')
    print('  per game: Shadow plays %.1f event cards with dice, %.1f combat cards, %.1f other reveals, draws %.1f cards with dice, discards %.1f face down' % (tot[('SH', 'E')] / n, tot[('SH', 'C')] / n, tot[('SH', 'X')] / n, dr[1] / n, sum(g['disc'].get('Sh', 0) for g in G) / n))
    print('  per game: FP plays     %.1f event cards with dice, %.1f combat cards, %.1f other reveals, draws %.1f cards with dice, discards %.1f face down' % (tot[('FP', 'E')] / n, tot[('FP', 'C')] / n, tot[('FP', 'X')] / n, dr[0] / n, sum(g['disc'].get('FP', 0) for g in G) / n))
    out = {}
    for side, w, nm in (('SH', 'S', 'Shadow'), ('FP', 'F', 'Free Peoples')):
        ev = collections.defaultdict(list); cb = collections.Counter(); xx = collections.Counter(); dice = collections.defaultdict(collections.Counter); tab = collections.Counter()
        for g in G:
            seen = set(); seenc = set()
            for t, s, title, mode, die, ont in g['cards']:
                if s != side: continue
                if mode == 'E':
                    if title not in seen: seen.add(title); ev[title].append((g, t))
                    dice[title][die] += 1; tab[title] += ont
                elif mode == 'C':
                    if title not in seenc: seenc.add(title); cb[title] += 1
                else: xx[title] += 1
        basew = sum(g['win'] == w for g in G) / n
        rows = []
        for title in set(ev) | set(cb):
            gs = ev.get(title, []); k = len(gs)
            wr = sum(g['win'] == w for g, t in gs) / k if k else float('nan')
            adj = mean([surv[(side, t)] for g, t in gs]) if k else float('nan')
            ts = sorted(t for g, t in gs)
            rows.append(dict(title=title, k=k, wr=wr, adj=adj, mt=mean(ts), md=med(ts), cb=cb[title], x=xx[title], die=dice[title].most_common(1)[0][0] if dice[title] else '', tab=tab[title]))
        out[side] = rows
        print('\n  %s cards, by how often played as an event (baseline %s win %.1f%%). Columns: games played as event, share of games, mean / median turn of the play,' % (nm, nm, 100 * basew))
        print('  win rate of %s in those games, length-adjusted baseline, difference; games used as combat card; other reveals; times put on the table' % nm)
        print('    %-40s %6s %7s %5s %4s %7s %7s %7s %7s %6s %6s' % ('card', 'event', 'games', 'mean', 'med', 'win', 'adj.b.', 'diff', 'combat', 'other', 'table'))
        for r in sorted(rows, key=lambda r: -r['k']):
            if r['title'] == '?': continue
            print('    %-40s %6d %7s %5.1f %4.0f %6.1f%% %6.1f%% %+6.1f %7d %6d %6d' % (r['title'][:40], r['k'], pc(r['k'], n), r['mt'] if r['k'] else 0, r['md'] if r['k'] else 0, 100 * r['wr'] if r['k'] else 0, 100 * r['adj'] if r['k'] else 0, 100 * (r['wr'] - r['adj']) if r['k'] else 0, r['cb'], r['x'], r['tab']))
        big = [r for r in rows if r['k'] >= minn and r['title'] != '?']
        print('  %s: largest positive difference to the length-adjusted baseline (n >= %d): ' % (nm, minn) + '; '.join('%s %+.1f (n=%d)' % (r['title'], 100 * (r['wr'] - r['adj']), r['k']) for r in sorted(big, key=lambda r: -(r['wr'] - r['adj']))[:12]))
        print('  %s: largest negative difference (n >= %d): ' % (nm, minn) + '; '.join('%s %+.1f (n=%d)' % (r['title'], 100 * (r['wr'] - r['adj']), r['k']) for r in sorted(big, key=lambda r: (r['wr'] - r['adj']))[:12]))
        print('  %s: most used as combat cards (games; card named by its event title): ' % nm + '; '.join('%s %d' % (r['title'], r['cb']) for r in sorted(rows, key=lambda r: -r['cb'])[:15]))
        early = collections.Counter(title for g in G for t, s, title, mode, die, ont in g['cards'] if s == side and mode == 'E' and t <= 3)
        print('  %s: most played as events in turns 1-3: ' % nm + '; '.join('%s %d' % kv for kv in early.most_common(12)))
        tb = collections.Counter(c for g in G for t, s, c in g['table'] if s == side)
        print('  %s: cards placed on the table (times): ' % nm + '; '.join('%s %d' % kv for kv in tb.most_common(14)))
    rm = collections.Counter((('FP' if a[1] == 0 else 'Shadow'), a[4]) for g in G for a in g['acts'] if a[3] == 'tablecard')
    print('\n  Cards on the table removed by spending an action die (who spends, card): ' + '; '.join('%s: %s %d' % (k[0], k[1], v) for k, v in rm.most_common(8)))
    return out


def t_rings(G):
    """Section 8. An Elven Ring use = a ring token moved off its box while an action die is in play (FP: Elven Ring box -> Shadow's
    side; Shadow: its box -> used). The new face is taken from the 'changes die' entry when the log has one; otherwise only what
    the die was then used for is known."""
    n = len(G); head('8. ELVEN RINGS  (n = %d games)' % n)
    for side, nm, wf in (('FP', 'Free Peoples', fpw), ('SH', 'Shadow', shw)):
        R = [(g, r) for g in G for r in g['rings'] if r[1] == side]
        per = collections.Counter(min(3, sum(1 for r in g['rings'] if r[1] == side)) for g in G)
        print('  %s: %d uses, %.2f per game; games with 0/1/2/3 uses: %s' % (nm, len(R), len(R) / n, ' / '.join(pc(per[k], n).strip() for k in range(4))))
        tc = collections.Counter(min(r[0], 12) for g, r in R)
        print('    turn of use:  ' + ' '.join('t%s %s' % (str(t) if t < 12 else '12+', pc(tc[t], len(R)).strip()) for t in range(1, 13)))
        first = [min([r[0] for r in g['rings'] if r[1] == side] or [0]) for g in G]; f = [x for x in first if x]
        print('    first use: mean turn %.1f, median %s' % (mean(f), med(f)))
        uc = collections.Counter(GROUP[r[5]] for g, r in R)
        print('    the changed die was used to: ' + ', '.join('%s %s' % (k, pc(v, len(R)).strip()) for k, v in uc.most_common(8)))
        fc = collections.Counter(r[3] for g, r in R)
        print('    face of the die as rolled: ' + ', '.join('%s %s' % (k, pc(v, len(R)).strip()) for k, v in fc.most_common(6)))
        ch = collections.Counter(r[4] for g, r in R if r[4])
        print('    new face where logged (n=%d): ' % sum(ch.values()) + ', '.join('%s %s' % (k, pc(v, sum(ch.values())).strip()) for k, v in ch.most_common(6)))
        for k in range(4):
            gs = [g for g in G if min(3, sum(1 for r in g['rings'] if r[1] == side)) == k]
            print('    games with %d uses: n=%5d  %s win %s, mean length %.1f' % (k, len(gs), nm, wf(gs), mean([g['turns'] for g in gs])))
        for lo, hi in ((1, 2), (3, 4), (5, 6), (7, 99)):
            gs = [g for g, x in zip(G, first) if lo <= x <= hi]
            print('    first use in turns %s: n=%5d  %s win %s' % ('%d-%d' % (lo, hi) if hi < 99 else '%d+' % lo, len(gs), nm, wf(gs)))


def t_winners(G):
    """Section 9. Each line compares games the Shadow went on to win with games the Free Peoples went on to win, using only games
    that were still going at the stated turn. Then win rates by bins of the most telling measures."""
    n = len(G); head('9. WHAT WINNERS DO DIFFERENTLY  (n = %d games; Shadow win %s)' % (n, shw(G)))
    def cmp(nm, f, t):
        a = [f(g) for g in G if g['turns'] > t and g['win'] == 'S']; b = [f(g) for g in G if g['turns'] > t and g['win'] == 'F']
        a = [x for x in a if x is not None]; b = [x for x in b if x is not None]
        print('    %-78s %7.2f (n=%5d) %7.2f (n=%5d)' % (nm, mean(a), len(a), mean(b), len(b)))
    def T(g, t, col): return Tmap(g).get(t, [None] * 9)[col]
    def nact(g, side, t, cls): return sum(1 for a in g['acts'] if a[1] == side and a[0] <= t and a[3] in cls)
    def sm(d, t, i): return sum(v[i] for k, v in d.items() if int(k) <= t)
    print('    %-78s %18s %18s' % ('measure (games still going after the turn)', 'Shadow went on to win', 'FP went on to win'))
    for t in (4, 6, 8): cmp('Shadow VP after turn %d' % t, lambda g: T(g, t, 1), t)
    for t in (3, 5, 7): cmp('Fellowship steps (moves outside Mordor) by end of turn %d' % t, lambda g: sum(1 for m in g['fsmoves'] if m[0] <= t and not m[3]), t)
    for t in (3, 5, 7): cmp('corruption after turn %d' % t, lambda g: T(g, t, 3), t)
    cmp('corruption per Fellowship step by end of turn 5', lambda g: (T(g, 5, 3) or 0) / max(1, sum(1 for m in g['fsmoves'] if m[0] <= 5 and not m[3])), 5)
    cmp('Hunt dice set aside, total turns 1-5', lambda g: sum(g['hunt'][str(t)][0] for t in range(1, 6) if str(t) in g['hunt']), 5)
    cmp('Eyes rolled, total turns 1-5 (luck)', lambda g: sum(g['hunt'][str(t)][1] for t in range(1, 6) if str(t) in g['hunt']), 5)
    cmp('Hunt hits (tiles drawn) on Fellowship moves, turns 1-5', lambda g: sum(1 for m in g['fsmoves'] if m[0] <= 5 and not m[3] and m[4] > 0), 5)
    cmp('companions removed from the Fellowship by end of turn 5', lambda g: sum(1 for d in g['deaths'] if d[0] <= 5 and d[2] == 'fellowship'), 5)
    cmp('Shadow dice on army moves and battles, turns 1-5', lambda g: nact(g, 1, 5, ('move', 'attack')), 5)
    cmp('Shadow dice on battles, turns 1-5', lambda g: nact(g, 1, 5, ('attack',)), 5)
    cmp('Shadow dice on mustering, turns 1-5', lambda g: nact(g, 1, 5, ('muster',)), 5)
    cmp('Shadow event cards played with dice, turns 1-5', lambda g: nact(g, 1, 5, ('card',)), 5)
    cmp('Shadow dice on drawing cards, turns 1-5', lambda g: nact(g, 1, 5, ('draw',)), 5)
    cmp('Shadow units brought on with dice or cards, turns 1-5', lambda g: sm(g['must'], 5, 1), 5)
    cmp('Shadow units removed from the map, turns 1-5', lambda g: sm(g['lost'], 5, 1), 5)
    cmp('FP units removed from the map, turns 1-5', lambda g: sm(g['lost'], 5, 0), 5)
    cmp('FP units brought on with dice or cards, turns 1-5', lambda g: sm(g['must'], 5, 0), 5)
    cmp('FP dice on Fellowship moves, turns 1-5', lambda g: nact(g, 0, 5, ('fsp',)), 5)
    cmp('FP dice on hiding the Fellowship, turns 1-5', lambda g: nact(g, 0, 5, ('hide',)), 5)
    cmp('FP dice on mustering, turns 1-5', lambda g: nact(g, 0, 5, ('muster',)), 5)
    cmp('FP dice on army moves and battles, turns 1-5', lambda g: nact(g, 0, 5, ('move', 'attack')), 5)
    cmp('FP event cards played with dice, turns 1-5', lambda g: nact(g, 0, 5, ('card',)), 5)
    cmp('FP dice on diplomacy, turns 1-5', lambda g: nact(g, 0, 5, ('pol',)), 5)
    cmp('Shadow units on the map after turn 5', lambda g: T(g, 5, 5), 5)
    cmp('FP units on the map after turn 5', lambda g: T(g, 5, 6), 5)
    cmp('FP units inside besieged strongholds after turn 5', lambda g: T(g, 5, 7), 5)
    cmp('Will of the West faces rolled by the FP, turns 1-5 (luck)', lambda g: sum(g['hunt'][str(t)][5].count('W') for t in range(1, 6) if str(t) in g['hunt']), 5)
    cmp('Character faces rolled by the FP, turns 1-5 (luck)', lambda g: sum(g['hunt'][str(t)][5].count('C') for t in range(1, 6) if str(t) in g['hunt']), 5)
    cmp('turn of the first Shadow capture (games with one)', lambda g: next((t for t, r, b in g['caps2'] if b == 'SH'), None), 0)
    cmp('turn the Fellowship enters Mordor (games that enter)', lambda g: g['mordor']['turn'] if g['mordor'] else None, 0)
    def bins(nm, f, t, edges, wf=shw, wn='SH win'):
        print('  ' + nm)
        gs = [(g, f(g)) for g in G if g['turns'] > t]
        for lo, hi in edges:
            sel = [g for g, x in gs if x is not None and lo <= x <= hi]
            print('    %-10s n=%6d  %s %s' % (str(lo) if lo == hi else ('%s+' % lo if hi >= 90 else '%s-%s' % (lo, hi)), len(sel), wn, wf(sel)))
    e1 = ((0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 99))
    bins('Shadow win rate by Shadow VP after turn 4 (games going past turn 4)', lambda g: T(g, 4, 1), 4, ((0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 99)))
    bins('Shadow win rate by Shadow VP after turn 6 (games going past turn 6)', lambda g: T(g, 6, 1), 6, ((0, 1), (2, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 7), (8, 99)))
    bins('Shadow win rate by Shadow VP after turn 8 (games going past turn 8)', lambda g: T(g, 8, 1), 8, ((0, 3), (4, 4), (5, 5), (6, 6), (7, 7), (8, 8), (9, 99)))
    bins('FP win rate by Fellowship steps by end of turn 3 (games going past turn 3)', lambda g: sum(1 for m in g['fsmoves'] if m[0] <= 3 and not m[3]), 3, ((0, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 99)), fpw, 'FP win')
    bins('FP win rate by Fellowship steps by end of turn 5 (games going past turn 5)', lambda g: sum(1 for m in g['fsmoves'] if m[0] <= 5 and not m[3]), 5, ((0, 4), (5, 5), (6, 6), (7, 7), (8, 8), (9, 9), (10, 99)), fpw, 'FP win')
    bins('FP win rate by corruption after turn 5 (games going past turn 5)', lambda g: T(g, 5, 3), 5, ((0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 99)), fpw, 'FP win')
    bins('Shadow win rate by Hunt dice set aside in turns 1-5, total', lambda g: sum(g['hunt'][str(t)][0] for t in range(1, 6) if str(t) in g['hunt']), 5, ((0, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 99)))
    bins('Shadow win rate by Shadow dice spent on armies (moves + battles) in turns 1-5', lambda g: nact(g, 1, 5, ('move', 'attack')), 5, ((0, 7), (8, 9), (10, 11), (12, 13), (14, 15), (16, 99)))
    bins('Shadow win rate by Shadow dice spent on mustering in turns 1-5', lambda g: nact(g, 1, 5, ('muster',)), 5, ((0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 99)))
    bins('FP win rate by FP dice spent on mustering in turns 1-5', lambda g: nact(g, 0, 5, ('muster',)), 5, ((0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 99)), fpw, 'FP win')
    bins('FP win rate by FP dice spent on Fellowship moves in turns 1-5', lambda g: nact(g, 0, 5, ('fsp',)), 5, ((0, 4), (5, 5), (6, 6), (7, 7), (8, 8), (9, 99)), fpw, 'FP win')


def t_extra(G):
    """Section 10. Assorted measures; each line says how it is counted."""
    n = len(G); head('10. OTHER THINGS IN THE DATA  (n = %d games)' % n)
    # eyes luck
    print('  Luck of the Shadow roll: Shadow win rate by Eyes rolled in turns 1-4 in total (games going past turn 4)')
    for lo, hi in ((0, 2), (3, 3), (4, 4), (5, 5), (6, 6), (7, 8), (9, 99)):
        gs = [g for g in G if g['turns'] > 4 and lo <= sum(g['hunt'][str(t)][1] for t in range(1, 5) if str(t) in g['hunt']) <= hi]
        print('    %-6s n=%6d SH win %s' % ('%d-%d' % (lo, hi) if hi < 99 and hi != lo else str(lo) + ('+' if hi == 99 else ''), len(gs), shw(gs)))
    print('  Luck of the FP roll: FP win rate by Will of the West faces rolled in turns 1-4 in total')
    for lo, hi in ((0, 0), (1, 1), (2, 2), (3, 3), (4, 4), (5, 99)):
        gs = [g for g in G if g['turns'] > 4 and lo <= sum(g['hunt'][str(t)][5].count('W') for t in range(1, 5) if str(t) in g['hunt']) <= hi]
        print('    %-6s n=%6d FP win %s' % (str(lo) + ('+' if hi == 99 else ''), len(gs), fpw(gs)))
    print('  Dice in the pools: mean Shadow dice (set aside + rolled) and FP dice rolled, by turn')
    print('    turn        ' + ''.join('%6d' % t for t in range(1, 13)))
    print('    Shadow dice ' + ''.join('%6.2f' % mean([g['hunt'][str(t)][0] + g['hunt'][str(t)][2] for g in G if str(t) in g['hunt']]) for t in range(1, 13)))
    print('    FP dice     ' + ''.join('%6.2f' % mean([g['hunt'][str(t)][3] for g in G if str(t) in g['hunt']]) for t in range(1, 13)))
    print('    Eyes in box ' + ''.join('%6.2f' % mean([g['hunt'][str(t)][0] + g['hunt'][str(t)][1] for g in G if str(t) in g['hunt']]) for t in range(1, 13)))
    print('  Units: mean per game-turn by turn (brought on / removed from the map)')
    print('    turn                  ' + ''.join('%6d' % t for t in range(1, 13)))
    for nm, d, i in (('Shadow units brought on', 'must', 1), ('Shadow units removed', 'lost', 1), ('FP units brought on', 'must', 0), ('FP units removed', 'lost', 0)):
        print('    %-22s' % nm + ''.join('%6.2f' % mean([g[d].get(str(t), [0, 0])[i] for g in G if g['turns'] >= t]) for t in range(1, 13)))
    for nm, col in (('Shadow units on map', 5), ('FP units on map', 6), ('FP units under siege', 7)):
        print('    %-22s' % nm + ''.join('%6.1f' % mean([Tmap(g)[t][col] for g in G if t in Tmap(g)]) for t in range(1, 13)))
    # who acts first
    fa = collections.Counter()
    for g in G:
        a = next((a for a in g['acts'] if a[0] == 1), None)
        if a: fa[a[1]] += 1
    print('  First action die of turn 1 is spent by: Free Peoples %s, Shadow %s (the FP may pass while they have fewer dice)' % (pc(fa[0], n).strip(), pc(fa[1], n).strip()))
    # FP military
    gs = [g for g in G if g['res'] == 'FPMV']
    c = collections.Counter(r for g in gs for r in set(r for t, r, b in g['caps2'] if b == 'FP' and r in SHSET))
    print('  Free Peoples military wins (n=%d): Shadow strongholds taken: ' % len(gs) + ', '.join('%s %s' % (r, pc(v, len(gs)).strip()) for r, v in c.most_common(8)))
    # Mouth / WK placement vs win; Saruman never
    # shadow first-capture timing vs win
    print('  Shadow win rate by the turn of its first capture:')
    for lo, hi in ((1, 3), (4, 4), (5, 5), (6, 6), (7, 7), (8, 99)):
        gs = [g for g in G if lo <= next((t for t, r, b in g['caps2'] if b == 'SH'), 0) <= hi]
        print('    first capture turn %-5s n=%6d SH win %s, military win %s' % ('%d-%d' % (lo, hi) if hi < 99 and hi != lo else str(lo) + ('+' if hi == 99 else ''), len(gs), shw(gs), pc(sum(g['res'] == 'SPMV' for g in gs), len(gs))))
    gs = [g for g in G if not any(b == 'SH' for t, r, b in g['caps2'])]
    print('    no capture at all        n=%6d SH win %s' % (len(gs), shw(gs)))
    # stronghold sieges length not available. Mordor run pace
    print('  Shadow win rate by turn the Fellowship enters Mordor vs Shadow VP at that moment (end of the previous turn): rate (n)')
    vb = ((0, 3, '0-3 VP'), (4, 5, '4-5 VP'), (6, 7, '6-7 VP'), (8, 9, '8-9 VP')); tb = ((1, 6, 'by t6'), (7, 7, 't7'), (8, 8, 't8'), (9, 9, 't9'), (10, 99, 't10+'))
    print('    %-10s' % '' + ''.join('%16s' % x[2] for x in tb))
    for v0, v1, vn in vb:
        line = '    %-10s' % vn
        for t0, t1, tn in tb:
            sel = [g for g in G if g['mordor'] and t0 <= g['mordor']['turn'] <= t1 and v0 <= (Tmap(g).get(g['mordor']['turn'] - 1, [0, 0])[1]) <= v1]
            line += '%9s (%4d)' % (shw(sel).strip(), len(sel))
        print(line)
    # the last turn: how wins are closed
    print('  Fellowship moves in the LAST turn of Ring-destroyed games: ' + ', '.join('%d: %s' % (k, pc(v, sum(1 for g in G if g['res'] == 'FPRV')).strip()) for k, v in sorted(collections.Counter(min(4, sum(1 for m in g['fsmoves'] if m[0] == g['turns'])) for g in G if g['res'] == 'FPRV').items())))
    print('  Final corruption in Ring-destroyed games (marker at end of log): ' + ', '.join('%d: %s' % (k, pc(v, sum(1 for g in G if g['res'] == 'FPRV')).strip()) for k, v in sorted(collections.Counter(g['end_corr'] for g in G if g['res'] == 'FPRV').items())))
    print('  Final Shadow VP in games the FP won by the Ring: ' + ', '.join('%d: %s' % (k, pc(v, sum(1 for g in G if g['res'] == 'FPRV')).strip()) for k, v in sorted(collections.Counter(min(g['end_svp'], 10) for g in G if g['res'] == 'FPRV').items())))
    print('  Final Mordor step in Shadow military wins: ' + ', '.join('%s: %s' % (k, pc(v, sum(1 for g in G if g['res'] == 'SPMV')).strip()) for k, v in sorted(collections.Counter(('not in Mordor' if g['end_mordor'] is None else 'step %d' % g['end_mordor']) for g in G if g['res'] == 'SPMV').items())))


def all_tables(G):
    """All ten sections for one sample."""
    t_outcomes(G); t_openings(G); t_dice(G); t_vp(G); t_tempo(G); t_fellowship(G); t_cards(G); t_rings(G); t_winners(G); t_extra(G)


def t_headline(samples):
    """Headline figures side by side for several samples [(name, games)]: the independent check of one set against another."""
    head('0. HEADLINE FIGURES, SAMPLE BY SAMPLE')
    def share(f): return lambda G: '%.1f%%' % (100 * mean([1 if f(g) else 0 for g in G]))
    def avg(f, fmt='%.2f'): return lambda G: fmt % mean([x for x in (f(g) for g in G) if x is not None])
    def T(g, t, col): return Tmap(g).get(t, [None] * 9)[col]
    def hitrate(e, k):
        def f(G):
            ms = [m for g in G for m in g['fsmoves'] if not m[3] and m[2] == e and m[1] == k]
            return '%.1f%%' % (100 * mean([1 if m[4] > 0 else 0 for m in ms]))
        return f
    rows = [('games', lambda G: '%d' % len(G)),
            ('Shadow wins', share(lambda g: g['win'] == 'S')),
            ('  Shadow military', share(lambda g: g['res'] == 'SPMV')), ('  corruption', share(lambda g: g['res'] == 'SPRV')),
            ('  Ring destroyed', share(lambda g: g['res'] == 'FPRV')), ('  FP military', share(lambda g: g['res'] == 'FPMV')),
            ('  conceded (either side)', share(lambda g: g['res'].startswith('Conceded'))),
            ('mean length, turns', avg(lambda g: g['turns'])),
            ('mean last turn, Shadow military wins', avg(lambda g: g['turns'] if g['res'] == 'SPMV' else None)),
            ('mean last turn, Ring destroyed', avg(lambda g: g['turns'] if g['res'] == 'FPRV' else None)),
            ('Isengard at war in turn 1', share(lambda g: g['war'].get('Isengard') == 1)),
            ('Saruman in play in turn 1', share(lambda g: (g['minion'].get('Saruman') or [0])[0] == 1)),
            ('Witch-king: mean turn', avg(lambda g: (g['minion'].get('Witch King') or [None])[0])),
            ('Mouth of Sauron appears', share(lambda g: 'Mouth of Sauron' in g['minion'])),
            ('Gandalf the White appears', share(lambda g: 'Gandalf' in g['newchar'])),
            ('Gandalf the White: mean turn', avg(lambda g: g['newchar'].get('Gandalf'))),
            ('Aragorn appears', share(lambda g: 'Strider' in g['newchar'])),
            ('no Hunt die set aside in turn 1', share(lambda g: g['hunt'].get('1', [1])[0] == 0)),
            ('Hunt dice set aside per turn, turns 2-5', avg(lambda g: mean([g['hunt'][str(t)][0] for t in range(2, 6) if str(t) in g['hunt']] or [0]))),
            ('Fellowship moves in turn 1', avg(lambda g: sum(1 for m in g['fsmoves'] if m[0] == 1))),
            ('Fellowship steps by end of turn 5', avg(lambda g: sum(1 for m in g['fsmoves'] if m[0] <= 5 and not m[3]))),
            ('corruption after turn 5', avg(lambda g: T(g, 5, 3))),
            ('Shadow VP after turn 4', avg(lambda g: T(g, 4, 1))), ('Shadow VP after turn 6', avg(lambda g: T(g, 6, 1))),
            ('Fellowship enters Mordor', share(lambda g: bool(g['mordor']))),
            ('  mean turn of entry', avg(lambda g: g['mordor']['turn'] if g['mordor'] else None)),
            ('  mean corruption at entry', avg(lambda g: g['mordor']['corr'] if g['mordor'] else None)),
            ('  Ring destroyed, of those entering', lambda G: '%.1f%%' % (100 * mean([1 if g['res'] == 'FPRV' else 0 for g in G if g['mordor']]))),
            ('Hunt hits: 1 Eye, 1st move of turn', hitrate(1, 1)), ('Hunt hits: 2 Eyes, 1st move', hitrate(2, 1)), ('Hunt hits: 2 Eyes, 2nd move', hitrate(2, 2)),
            ('Shadow event cards played per game', avg(lambda g: sum(1 for k in g['cards'] if k[1] == 'SH' and k[3] == 'E'), '%.1f')),
            ('FP event cards played per game', avg(lambda g: sum(1 for k in g['cards'] if k[1] == 'FP' and k[3] == 'E'), '%.1f')),
            ('FP Elven-ring uses per game', avg(lambda g: sum(1 for r in g['rings'] if r[1] == 'FP'))),
            ('Shadow Elven-ring uses per game', avg(lambda g: sum(1 for r in g['rings'] if r[1] == 'SH'))),
            ('Shadow dice on armies, turns 1-5', avg(lambda g: sum(1 for a in g['acts'] if a[1] == 1 and a[0] <= 5 and a[3] in ('move', 'attack')) if g['turns'] > 5 else None)),
            ('Dale taken by the Shadow', share(lambda g: any(r == 'Dale' and b == 'SH' for t, r, b in g['caps2']))),
            ('Minas Tirith taken by the Shadow', share(lambda g: any(r == 'Minas Tirith' and b == 'SH' for t, r, b in g['caps2']))),
            ("Helm's Deep taken by the Shadow", share(lambda g: any(r == "Helm's Deep" and b == 'SH' for t, r, b in g['caps2'])))]
    print('  %-42s' % '' + ''.join('%26s' % nm for nm, G in samples))
    for nm, f in rows: print('  %-42s' % nm + ''.join('%26s' % f(G) for _, G in samples))


CAVEATS = """HUMAN GAME DATA FOR THE WAR OF THE RING GUIDE (2nd edition, base game)
Computed by humanstats.py from the decoded logs only (events and end-of-turn boards); no raw logs, names, chat or game ids were read.

SAMPLES
  A  community, no handicap : games of 2023-2026 played without action tokens / dwarven rings / muster points. The cleanest sample of the plain game.
  B  community, all usable  : the same site, all games passing the usability filter (base game, 5+ turns, piece tracking consistent, winner reported).
     About three quarters of these gave the Free Peoples a handicap (extra action tokens and the like), assigned by rating, so results stay balanced
     but FP play is slightly richer than the rules allow. Token actions are not action dice and are not counted in any dice table.
  C  ladder2022             : an older, separate ladder (about 1,500 games, 86% without handicap), kept as an independent check.
  The small sets were not used: 'library' (367 games) has no reported winners, 'tourney2018' has 24 games.
  Section 0 puts the headline figures of the three samples side by side; then sections 1-10 follow for A, then B, then C.

READING THE NUMBERS
  Every figure is exact for the games in its row; n is stated. A win rate over n games carries a 95% margin of about +-98/sqrt(n) points:
  n=100 -> +-10, n=400 -> +-5, n=1600 -> +-2.5, n=6400 -> +-1.2. Rows under ~200 games are noise for anything but large differences.
  All win-rate differences are associations. Many are driven by the position (a side that is already ahead does different things), by the
  dice rolled, or by game length (long games see more cards, more characters, more of everything), not by the choice itself.

WHAT COULD NOT BE EXTRACTED
  - Battles as such. Combat rolls are not in the logs. An army-die action counts as a "battle" only if units were removed or combat cards were
    chosen during it; a bloodless battle or a failed siege round looks like a move or like nothing. Battle rounds, odds, sieges' length, and
    who attacked whom are not available. Units "removed from the map" include losses from cards.
  - Hunt tile values and Hunt dice results. Only whether a tile was drawn during a Fellowship move ("the Hunt hit") and how far the
    corruption marker then moved. Whether the Fellowship was revealed or hidden at a given moment is not reliable (players flip either of two
    pieces, or neither), so no revealed/hidden statistics are given beyond the number of dice spent hiding.
  - Cards discarded or held. Hands are hidden: a card is identified only when it is revealed. So "played as an event" and "played as a combat
    card" are known; discards (counted, but face down) and cards never drawn are not. Combat-card use is named by the card's EVENT title; the
    combat text is not in the data. "Other reveals" are cards shown outside the owner's own action die (responses, second cards, mis-clicks).
  - "Cards on the table": counted when a revealed card is put on one of the table slots. Some players also park ordinary event cards there
    while resolving them, so only the large counts (Mithril Coat, Axe and Bow, Horn of Gondor, Wizard's Staff, A Power Too Great, Grond,
    Palantir, Worn with Sorrow and Toil, Fighting Uruk-hai, Black Captain, Ringwraiths, Denethor's Folly, Balrog, Crebain) mean much.
  - Distance left to Mordor. The progress counter is reset by hand and unreliable; "steps" are counted as Fellowship moves made outside Mordor
    (10 are needed on the shortest road from Rivendell; detours and Shadow cards that push the Fellowship back make it more).
  - Why a game ended. The result code is as reported by the players. In about 40-45% of games reported as Shadow military wins the log stops
    before the VP marker reaches 10 (last blow not played out, or a concession reported as a win), so "final" figures use the last logged position.
  - Elven-ring results: the new face is logged in only about 1 use in 6-7; otherwise only what the changed die was then used for is known.

HOW EACH MEASURE IS COUNTED (details in the docstrings of humanstats.py)
  - Turn = number of Free Peoples action-dice rolls so far. Things done between the last action of a turn and the next roll (declaring,
    entering Mordor, changing guide) are booked to the turn about to start.
  - Action = one action die moved from the pool into play, with everything its owner then moved until the next die. Class by what moved:
    card revealed > die placed in the Hunt box (Fellowship move) > new character > companions leaving the Fellowship > own nation's political
    marker advanced > army units moved between regions (battle if losses/combat cards) > units from reinforcements (muster) > card drawn >
    companions / Nazgul only > Fellowship piece flipped (hide). Die face is the face as rolled.
  - Hunt dice set aside = Shadow dice put in the Hunt box before the roll (by the client's allocate command or dragged by hand; the project's
    older stats.py missed the hand-dragged ones and so undercounts). Eyes in the box = set aside + Eyes rolled.
  - Victory points = the VP markers at end of turn. Settlement captures = first end-of-turn board on which an enemy army stands alone in the
    settlement (or an enemy control marker lies there); control markers alone would miss roughly a quarter to a third of captures. On the final board the
    settlements so held add up exactly to the VP marker in about 5 games out of 6.
  - At war = the nation's political marker reaches the last box. Minions = first placed on the map. Gandalf the White / Aragorn = the
    Gandalf / Strider figure is first flipped.
  - Companion "removed in the Fellowship" = taken off while still with the Ring (a Hunt casualty or a Shadow card); "left alive" = moved from
    the Fellowship to the map (a separation with a die, by a card, or Merry/Pippin leaving instead of dying).

VERIFIED BY EYE against the event stream of individual games: action classes and die faces for both sides over whole turns 1-2 (1 game)
and for the Free Peoples' turn 1 (2 more games); Hunt dice set aside and Eyes rolled (3 games); settlement captures against the VP marker
turn by turn (1 game) and in aggregate (final holdings = final VP in 83% of 385 games); Mordor entry turn against the end-of-turn board
(1 game); the event pattern of Gandalf's removal as guide and his return as the White (2 games); the turn-1 Strider run to Dol Amroth
(2 games); Saruman's elite-upgrade musters (4 actions); a sample of ~50 actions the classifier could not name. Checks that need no eye:
the three samples agree closely on every headline figure (section 0), and with 1 Eye in the box the first move of a turn is hit 18-20%
of the time (the bare die gives 16.7%; re-rolls and cards add the rest), with 2 Eyes 33-34% (bare 30.6%).
NOT separately verified: card-on-table counts, Elven-ring new faces, units brought on / removed, companion landing places, guide changes.
"""


def report(out=None):
    """Write the full report (caveats, headline comparison, then all sections for the three samples) to out (default human-data.txt here)."""
    import io, contextlib
    out = out or os.path.join(HERE, 'human-data.txt')
    A = load('community', nohc=True); B = load('community'); C = load('ladder2022')
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        print(CAVEATS)
        t_headline([('A community no handicap', A), ('B community all', B), ('C ladder2022', C)])
        for nm, G in (('A: COMMUNITY 2023-2026, NO-HANDICAP GAMES', A), ('B: COMMUNITY 2023-2026, ALL USABLE GAMES', B), ('C: LADDER 2022 (INDEPENDENT CHECK)', C)):
            print('\n\n' + '=' * 118 + '\nSAMPLE ' + nm + '  (n = %d)\n' % len(G) + '=' * 118)
            all_tables(G)
    open(out, 'w').write(buf.getvalue())
    return out


if __name__ == '__main__':
    if sys.argv[1] == 'build':
        print(sys.argv[2], 'written %d, failed %d' % build_cache(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else None))
    if sys.argv[1] == 'tables':
        all_tables(load(sys.argv[2], nohc=len(sys.argv) > 3 and sys.argv[3] == 'nohc'))
    if sys.argv[1] == 'report':
        print(report(sys.argv[2] if len(sys.argv) > 2 else None))
