#!/usr/bin/env python3
"""Decode War of the Ring Java-client game logs into plain JSON.

Usage: decode.py <raw log dir> <out dir> [sheet csv | id->result json | the community site's gameReports.csv ...]

The raw logs are the client's event stream: "$player<~Scribe.141~>211 <~Scribe.142~><~Game.2042~>..." where
<~X.n~> are keys into the client's message table and three-digit numbers are playing pieces. The message table,
the piece table, the starting positions and the card numbering come from the AlphaWOTR repository next door
(read only), which derived them from the client itself.

For every game this writes
  <id>.events.jsonl  one decoded event per line (no chat, no passwords, sides instead of player names)
  <id>.turns.jsonl   the board at the end of every turn, rebuilt by following every piece
and a games.json index with the result of each game (from the tournament sheet) and replay-quality counts.
"""
import collections, csv, glob, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ALPHA = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'AlphaWOTR'))
sys.path.insert(0, os.path.join(ALPHA, 'src'))
import piece_table_authoritative as PT          # noqa: E402
import physical_initial_locations as PIL        # noqa: E402
import card_decoder as CD                       # noqa: E402

MSG = {}
for _l in open(os.path.join(ALPHA, 'wotr_bridge', 'play_client', '2ndedition', 'messagesb2.properties'), encoding='latin-1'):
    if '=' in _l and not _l.startswith('#'):
        _k, _v = _l.rstrip('\r\n').split('=', 1)
        MSG[_k] = _v.replace('\\ ', ' ')

SH_NATIONS = ('ISENGARD', 'SAURON', 'SOUTHRON_EASTERLINGS')
SH_FACE = {0: 'Muster', 1: 'Muster/Army', 2: 'Army', 3: 'Character', 4: 'Event', 5: 'Eye'}
FP_FACE = {0: 'Will of the West', 1: 'Character', 2: 'Event', 3: 'Muster', 4: 'Muster/Army'}
VP = {'The Grey Havens': 2, 'Rivendell': 2, 'Woodland Realm': 2, 'Lorien': 2, 'Erebor': 2, "Helm's Deep": 2, 'Minas Tirith': 2, 'Dol Amroth': 2,
      'The Shire': 1, 'Edoras': 1, 'Dale': 1, 'Pelargir': 1,
      'Orthanc': 2, 'Moria': 2, 'Dol Guldur': 2, 'Mount Gundabad': 2, 'Barad-Dur': 2, 'Minas Morgul': 2, 'Morannon': 2, 'Umbar': 2,
      'Angmar': 1, 'Far Harad': 1, 'South Rhun': 1}
COMPANIONS = ('Gandalf', 'Strider', 'Legolas', 'Gimli', 'Boromir', 'Meriadoc', 'Peregrin', 'Gollum')
DECKS = {'FC': CD.FP_CHARACTER_CARDS, 'FS': CD.FP_STRATEGY_CARDS, 'SC': CD.SA_CHARACTER_CARDS, 'SS': CD.SA_STRATEGY_CARDS}


def card_deck(pid):
    n = int(pid)
    if 106 <= n < 106 + len(CD._FP_PID_TYPES) and CD._FP_PID_TYPES[n - 106] != 'X': return 'F' + CD._FP_PID_TYPES[n - 106]
    if 158 <= n < 158 + len(CD._SA_PID_TYPES) and CD._SA_PID_TYPES[n - 158] != 'X': return 'S' + CD._SA_PID_TYPES[n - 158]
    return None


def label(pid):
    if card_deck(pid): return {'FC': 'FP character card', 'FS': 'FP strategy card', 'SC': 'Shadow character card', 'SS': 'Shadow strategy card'}[card_deck(pid)]
    return PT.AUTH_PIECE_LABELS.get(pid, 'piece ' + pid)


def owner(pid):
    """Which side a piece belongs to, where that is knowable from the piece alone."""
    if pid in PT.AUTH_ARMY_PIECES: return 'SH' if PT.AUTH_ARMY_PIECES[pid][0] in SH_NATIONS else 'FP'
    lab = label(pid)
    if lab.startswith('SH ') or lab.startswith('Shadow') or lab in ('Witch King', 'Saruman', 'Mouth of Sauron'): return 'SH'
    if lab.startswith('FP ') or lab in COMPANIONS: return 'FP'
    return None


def area(tok):
    if tok == 'Controls.45': return 'FP hand'
    if tok == 'Controls.46': return 'Shadow hand'
    return MSG.get(tok, tok)


TOK = re.compile(r'<~([A-Za-z]+\.\d+)~>')
TAIL = re.compile(r'\s+(null|[0-9A-F]{6})\s*$')


def undo_pass(lines):
    """Blank commands that were taken back (same rule as AlphaWOTR's undo_prepass)."""
    redo = []
    for i, l in enumerate(lines):
        if '<~Controls.819~>' in l:
            for j in range(i - 1, -1, -1):
                lj = lines[j]
                if lj.startswith('$') and '<~Controls.819~>' not in lj and '<~Controls.16~>' not in lj and not re.match(r'\$\S+ (silent|noisy)\b', lj):
                    if re.match(r'\$\S+ changes ', lj): lines[j] = ''; continue
                    redo.append((j, lj)); lines[j] = ''
                    break
            lines[i] = ''
        elif '<~Controls.16~>' in l and l.startswith('$'):
            if redo:
                j, o = redo.pop(); lines[j] = o
            lines[i] = ''
        elif l.startswith('$') and redo and not re.match(r'\$\S+ (silent|noisy)\b', l):
            redo = []
    return lines


def parse(line):
    """-> dict(player, verb, pieces, frm, to, extra) or None for lines that carry no game event."""
    m = re.match(r'\$(\S*?)(<~|\s)', line)
    if not m: return None
    player = m.group(1)
    body = TAIL.sub('', line[len(player) + 1:])
    keys = TOK.findall(body)
    if not keys:
        cm = re.match(r'\s*changes (\d{3})\s+to (\w)', body)
        if cm: return dict(player=player, verb='changes die', pieces=[cm.group(1)], extra={'face': cm.group(2)})
        return None
    parts = TOK.split(body)                       # ['', key, text, key, text, ...]
    segs = [(parts[i], parts[i + 1]) for i in range(1, len(parts), 2)]
    verb = MSG.get(segs[0][0], segs[0][0]).strip()
    ev = dict(player=player, verb=verb, key=segs[0][0], pieces=[], extra={})
    first = segs[0][1]
    if 'reveal' in first:                         # a card played face up: "reveal <piece> <hash> <card number>"
        rm = re.search(r'reveal (\d{3}) \S+ (\d+)', first)
        ev['pieces'] = [rm.group(1)]; d = card_deck(rm.group(1))
        ev['extra']['card'] = DECKS[d].get(int(rm.group(2)), '?') if d else '?'
        ev['verb'] = 'plays card'
    else:
        ev['pieces'] = re.findall(r'\b\d{3}\b', first)
    places = []
    for k, _t in segs[1:]:
        if k.startswith('Game.') or k in ('Controls.45', 'Controls.46'): places.append(area(k))
        elif k in ('Controls.307', 'Controls.309'): places.append('@reinforcements')
        elif k == 'Controls.308': places.append('@casualties')
        elif k == 'CardBuffet.11': places.append('Current FP Card')
        elif k == 'CardBuffet.16': places.append('Current SA Card')
    if 'Spare' in body: places.append('Spare')
    if verb in ('moves', 'draws', 'plays card'):
        if len(places) >= 2: ev['frm'], ev['to'] = places[0], places[1]
        elif len(places) == 1: ev['to'] = places[0]
    elif verb == 'kills':
        if places: ev['frm'] = places[0]
    elif verb == 'rolls':
        vals = [int(x) for x in segs[1][1].split()] if len(segs) > 1 else []
        ev['extra']['faces'] = vals
    elif verb.startswith('views'):
        ev['pieces'] = []
    return ev


def unit_kind(pid):
    if pid in PT.AUTH_ARMY_PIECES:
        nat, kind = PT.AUTH_ARMY_PIECES[pid]
        return nat, kind
    return None, label(pid)


def snapshot(loc, flipped):
    """Board as {region: {FP: {...}, SH: {...}}} plus the tracks, from where every piece is."""
    regions = collections.defaultdict(lambda: {'FP': collections.Counter(), 'SH': collections.Counter(), 'other': []})
    tracks = {}
    for pid, where in loc.items():
        lab = label(pid)
        if where is None: continue
        if where.startswith('Fellowship Track'):
            tracks[lab.lower() if lab in ('Corruption',) else lab] = int(where.split()[-1]); continue
        if where.startswith('Victory Points') and lab.startswith('Victory Points Marker'):
            tracks['Shadow VP' if 'Shadow' in lab else 'FP VP'] = int(where.split()[-1]); continue
        if where.startswith('Mordor ') and lab == 'Fellowship':
            tracks['Mordor step'] = int(where.split()[1]); continue
        if lab == 'Fellowship': tracks['Fellowship last seen'] = where; continue
        if where.startswith('Political Track'):
            tracks.setdefault('politics', {})[lab + ('*' if pid in flipped else '')] = int(where.split()[-1]); continue
        if where in ('Spare', 'FP hand', 'Shadow hand') or 'Deck' in where or 'Dice' in where or 'Hunt' in where or 'Card' in where or 'Current' in where: continue
        if 'Reinforcements' in where or 'Casualties' in where: continue
        nat, kind = unit_kind(pid)
        if nat:
            side = 'SH' if nat in SH_NATIONS else 'FP'
            regions[where][side][nat + ' ' + kind] += 1
        elif lab == 'NAZGUL': regions[where]['SH']['NAZGUL'] += 1
        elif 'die' in lab or 'card' in lab or 'tile' in lab or lab.startswith('cls:') or lab.startswith('piece '):
            continue
        elif 'Control marker' in lab:
            regions[where]['other'].append('captured by ' + ('Shadow' if lab.startswith('Shadow') else 'Free Peoples'))
        else:
            regions[where]['other'].append(lab + ('*' if pid in flipped else ''))
    out = {}
    for r, v in sorted(regions.items()):
        o = {}
        if v['FP']: o['FP'] = dict(v['FP'])
        if v['SH']: o['SH'] = dict(v['SH'])
        if v['other']: o['other'] = sorted(v['other'])
        if o: out[r] = o
    return out, tracks


def decode(path, outdir, gid, meta):
    raw = open(path, encoding='latin-1').read().splitlines()
    lines = undo_pass([l if l.startswith('$') or l.startswith('<replay>') or l.startswith('<auto>') else '' for l in raw])
    loc = dict((p, MSG.get(a, a)) for p, a in PIL.INITIAL_FIGURE_LOCATIONS.items())
    flipped = set()
    side = {}                      # player name -> FP / SH
    turn = 0
    stats = collections.Counter()
    events, turns = [], []
    captures = []                             # settlements that changed hands: a control marker laid on them
    action = 0                                # events between a die entering and leaving the action area share a number
    open_action = False
    rolled = {}
    faces = {}                                # die piece -> face it shows

    def end_turn():
        if turn == 0: return
        board, tracks = snapshot(loc, flipped)
        turns.append(dict(turn=turn, rolled=dict(rolled), tracks=tracks, board=board))

    # Turn boundaries: saved replays carry "<replay> Turn N" markers; plain game logs do not, and there a turn starts
    # where the Free Peoples roll their action dice
    marked = any(l.startswith('<replay>') and re.search(r'\d', l) for l in lines)
    for i, l in enumerate(lines):
        if not l: continue
        if not marked and l.startswith('$') and '<~Controls.345~>' in l and len(re.findall(r'\b21[0-3]\b', l.split('<~Controls.346~>')[0])) >= 3:
            end_turn(); turn += 1; rolled = {}; open_action = False
        if l.startswith('<replay>'):
            m = re.search(r'(\d+)', l)
            if m and int(m.group(1)) != turn:
                end_turn(); turn = int(m.group(1)); rolled = {}; open_action = False
            continue
        if l.startswith('<auto>'): l = '$auto' + l[6:]
        ev = parse(l)
        if not ev: continue
        who = ev['player']
        if ev['verb'] == 'rolls' and who not in side and ev['pieces']:
            if ev['pieces'][0] in ('210', '211', '212', '213'): side[who] = 'FP'
            elif '099' <= ev['pieces'][0] <= '105': side[who] = 'SH'
        s = side.get(who, 'setup' if who in ('auto', 'base', 'TEST') else '?')
        e = dict(line=i + 1, turn=turn, side=s, verb=ev['verb'])
        if ev['verb'] == 'moves' and ev.get('to') == 'Current Action' and ev['pieces'] and 'action die' in label(ev['pieces'][0]) and not open_action:
            action += 1; open_action = True
        if open_action or ev['verb'] == 'kills' and ev.get('frm') == 'Current Action': e['action'] = action
        if ev['verb'] == 'kills' and ev.get('frm') == 'Current Action': open_action = False
        pcs = ev['pieces']
        if pcs: e['pieces'] = [dict(id=p, what=label(p) + ('*' if p in flipped else '')) for p in pcs]
        for k in ('frm', 'to'):
            if k in ev: e['from' if k == 'frm' else 'to'] = ev[k]
        if ev['extra']: e.update(ev['extra'])
        v = ev['verb']
        if v in ('moves', 'draws', 'plays card'):
            for p in pcs:
                frm, to = ev.get('frm'), ev.get('to')
                if frm and frm.startswith('@'): frm = None
                if s == 'setup': pass
                elif frm and p in loc and loc[p] != frm and frm != 'Spare':
                    stats['from_mismatch'] += 1; e.setdefault('note', []).append('%s was tracked in %s' % (p, loc[p]))
                elif frm and p in loc: stats['from_ok'] += 1
                if to: loc[p] = to
                if to in VP and 'Control marker' in label(p):
                    captures.append(dict(turn=turn, region=to, by='SH' if label(p).startswith('Shadow') else 'FP', vp=VP[to]))
                    e['captures'] = to
            if ev.get('to') == 'Current Action' and pcs and 'action die' in label(pcs[0]):
                e['die'] = faces.get(pcs[0], '?')
        elif v == 'kills':
            # "kills ... in Current Action" clears the staging area at the end of an action (the die, the card, any
            # casualties dragged there), so the named area is only checked when it is a real place
            for p in pcs:
                if p in loc and ev.get('frm') and ev['frm'] != 'Current Action' and loc[p] != ev['frm']:
                    stats['kill_mismatch'] += 1; e.setdefault('note', []).append('%s was tracked in %s' % (p, loc[p]))
                elif p in loc: stats['kill_ok'] += 1
                o = owner(p); lab = label(p)
                if p in PT.AUTH_ARMY_PIECES and o == 'SH': loc[p] = 'SA Reinforcements'      # Shadow units are never out of the game
                elif lab == 'hunt tile': loc[p] = 'Used Hunt Tiles'
                elif 'card' in lab: loc[p] = 'discard'
                else: loc[p] = 'FP Casualties' if o == 'FP' else 'SA Casualties' if o == 'SH' else 'removed'
        elif v == 'allocates':                       # Shadow dice put in the Hunt box before the roll
            # Controls.372: one die set aside for the Hunt before rolling; Controls.350: after the roll, the Eyes join it
            for p in pcs:
                if ev['key'] == 'Controls.372' or faces.get(p) == 'Eye': loc[p] = 'The Hunt For The Ring'
            e['to'] = 'The Hunt For The Ring' if ev['key'] == 'Controls.372' else 'Shadow dice pool (Eyes to the Hunt box)'
        elif v == 'flips':
            for p in pcs: flipped.symmetric_difference_update([p])
        elif v == 'rolls':
            names = FP_FACE if s == 'FP' else SH_FACE
            fs = [names.get(x % 10, str(x)) for x in ev['extra'].get('faces', [])]
            if pcs and 'action die' in label(pcs[0]):
                e['faces'] = fs; rolled[s] = fs
                for p, f in zip(pcs, fs): faces[p] = f
            else:
                e['faces'] = ev['extra'].get('faces', [])
        elif v == 'changes die':
            faces[pcs[0]] = {'A': 'Army', 'C': 'Character', 'M': 'Muster', 'H': 'Muster/Army', 'P': 'Event', 'E': 'Eye', 'W': 'Will of the West'}.get(ev['extra']['face'], '?')
        stats['events'] += 1
        events.append(e)
    end_turn()
    with open(os.path.join(outdir, gid + '.events.jsonl'), 'w') as f:
        for e in events: f.write(json.dumps(e, ensure_ascii=False) + '\n')
    with open(os.path.join(outdir, gid + '.turns.jsonl'), 'w') as f:
        for t in turns: f.write(json.dumps(t, ensure_ascii=False) + '\n')
    board, tracks = snapshot(loc, flipped)
    sh_vp, fp_vp, corr, mordor = tracks.get('Shadow VP', 0), tracks.get('FP VP', 0), tracks.get('corruption', 0), tracks.get('Mordor step')
    # many games stop at a concession, so the last position is reported as it stands and the sheet's result kept beside it
    meta = dict(meta, shadow_vp=sh_vp, fp_vp=fp_vp, corruption=corr, mordor_step=mordor, captures=captures,
                ended='Ring destroyed' if mordor == 5 else 'Shadow conquest' if sh_vp >= 10 else 'Free Peoples conquest' if fp_vp >= 4 else 'corruption' if corr >= 12 else 'conceded or unclear')
    hdr = next((re.search(r'\[(.*?)\]', l).group(1) for l in raw if 'Controls.408' in l and '[' in l), '')
    return dict(id=gid, turns=turn, module=hdr, events=stats['events'],
                moves_consistent=stats['from_ok'] + stats['kill_ok'], moves_inconsistent=stats['from_mismatch'] + stats['kill_mismatch'], **meta)


def sheet_meta(paths):
    """log file id -> round, result code and expansion flag from the tournament sheet."""
    out = {}
    for p in paths:
        rnd = ''
        for row in csv.reader(open(p, encoding='utf-8')):
            cells = [c.strip() for c in row]
            head = next((c for c in cells if c), '')
            if re.match(r'(Round|Final|Semi|Quarter|Play)', head, re.I): rnd = head
            for c in cells:
                m = re.search(r'(?:id.|/d/)([A-Za-z0-9_-]{20,})', c) if 'drive.google' in c else None
                if m:
                    k = cells.index(c)
                    res = next((x for x in cells[:k] if re.fullmatch(r'[FS][RMC]\*?', x)), '')
                    out[m.group(1)] = dict(round=rnd, result=res, expansion=any(x.lower() == 'yes' for x in cells[:k]))
    return out


def community_id(name):
    """A game id without the player names for the community site's file names (date_time_FP_name_SP_name), else None."""
    m = re.match(r'(\d{4}-\d\d-\d\d_\d{6})_', name)
    return m.group(1) + '-' + hashlib.sha1(name.encode()).hexdigest()[:6] if m else None


def community_meta(path):
    """game id -> reported result and handicaps from the community site's export (gameReports.csv); no names, no comments."""
    out = {}
    for r in csv.DictReader(open(path, encoding='utf-8')):
        gid = community_id(os.path.basename(r['log_file'])[:-4]) if r['log_file'] else None
        if not gid: continue
        s = 'FP' if r['side'] == 'Free' else 'SP'
        res = 'Conceded %s won' % s if r['victory'] == 'Concession' else s + ('RV' if r['victory'] == 'Ring' else 'MV')   # SPRV = corruption, as in the ladder sheet
        num = lambda k: float(r[k] or 0)
        out[gid] = dict(result=res, sheet_turns=r['turns'], sheet_corruption=r['corruption'], sheet_mordor=r['mordor'], date=r['timestamp'][:10],
                        action_tokens=int(num('action_tokens')), dwarven_rings=int(num('dwarven_rings')), muster_points=num('muster_points'),
                        handicap=bool(num('action_tokens') or num('dwarven_rings') or num('muster_points')),
                        expansions=r['expansions'], rated=r['match'], competition=r['competition'], league=r['league'])
    return out


if __name__ == '__main__':
    import hashlib
    rawdir, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    reports = [a for a in sys.argv[3:] if os.path.basename(a) == 'gameReports.csv']
    meta = sheet_meta([a for a in sys.argv[3:] if a.endswith('.csv') and a not in reports])
    for a in reports: meta.update(community_meta(a))
    for a in sys.argv[3:]:
        if a.endswith('.json'): meta.update(json.load(open(a)))      # id -> result fields, e.g. from the ladder sheet
    games = []; skipped = collections.Counter()
    for p in sorted(glob.glob(os.path.join(rawdir, '**', '*.log'), recursive=True)):
        name = os.path.basename(p)[:-4]
        gid = community_id(name) or (name if re.fullmatch(r'[A-Za-z0-9_-]{25,}', name) else hashlib.sha1(name.encode()).hexdigest()[:12])   # file names carry player names
        if community_id(name): name = gid
        head = open(p, encoding='latin-1').read(20000)
        m = re.search(r'Controls\.408~>\[([^\]]*)', head)
        if not m: skipped['no game header'] += 1; continue
        if 'base2' not in m.group(1): skipped['not the 2nd-edition base game: ' + m.group(1).split()[-1]] += 1; continue
        try: g = decode(p, outdir, gid, dict(meta.get(gid, {}), file=name))
        except Exception as ex: skipped['decoder error %s' % type(ex).__name__] += 1; continue
        games.append(g)
        print('%s  turns %2d  %-34s sheet %-2s  Shadow VP %2d  FP VP %d  corruption %2d  Mordor step %-4s -> %-22s  consistent %4d, not %3d' % (gid[:8], g['turns'], g['module'], g.get('result', ''), g['shadow_vp'], g['fp_vp'], g['corruption'], g['mordor_step'], g['ended'], g['moves_consistent'], g['moves_inconsistent']))
    json.dump(games, open(os.path.join(outdir, 'games.json'), 'w'), indent=1)
    print('decoded %d games; skipped: %s' % (len(games), dict(skipped)))
