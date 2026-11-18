#!/usr/bin/env python3
"""The new AlphaWOTR release (v0.2.0) through the WotR Bot Protocol, with its new networks and its ordinary search
(the release's strength 1: no rest-of-turn search, so no minute-long decisions).

The release's own protocol bot still loads the old corpus_live pair.  This file runs that bot unchanged except for
one step: where it searches, the search class of the release's own server is used (wotr_bridge/core/own_model.py,
OwnModelSearch), in its pure-Python mode, with the network of the chosen profile.  Nothing is written in the release.

  python -B alpha2_bot.py --root "<AlphaWOTR 2 folder>" --fp win|mil --sh win|cor [the bot's own arguments]
  python -B alpha2_bot.py --root "<folder>" --fp win --sh win --load-only     loads both networks and stops
  --seconds S         aim each search at S seconds (as many searches as fit, up to the --sims cap)
  --backend native    the release's compiled C++ search instead of the Python one (use a copy of the release as root:
                      the compiled search may write beside its library)

Profiles: --fp win = FP-Win, --fp mil = FPMV; --sh win = SP-Win, --sh cor = SP corruption.  A seat opens only its own
side's file, so the other side's choice does not matter in a game.
Run it with the release's own Python (python/bin/python3 in its folder), which has the packages it needs.
"""
import importlib.util
import os
import sys
import time


def take(argv, name, default=None):
    """Removes `name value` from argv and returns the value."""
    if name in argv:
        i = argv.index(name)
        value = argv[i + 1]
        del argv[i:i + 2]
        return value
    return default


def main():
    argv = sys.argv[1:]
    root = os.path.abspath(take(argv, '--root', os.environ.get('ALPHA2_ROOT', '')))
    fp, sh = take(argv, '--fp', 'win'), take(argv, '--sh', 'win')
    seconds = float(take(argv, '--seconds', os.environ.get('ALPHA2_SECONDS', '0')) or 0)   # aim per decision; 0: the bot's own clock rule
    backend = take(argv, '--backend', 'python')     # python, or native: the release's compiled C++ search
    load_only = '--load-only' in argv
    if load_only:
        argv.remove('--load-only')
    if fp not in ('win', 'mil') or sh not in ('win', 'cor') or not os.path.isdir(os.path.join(root, 'wotr_bridge')):
        sys.exit('usage: alpha2_bot.py --root <AlphaWOTR 2 folder> --fp win|mil --sh win|cor [bot arguments]')
    tag = 'mc200_fp%s_sh%s' % (fp, sh)

    # the release's launch environment (wotr_bridge/launch_head.py HEAD_ENV), except: the search in Python (no C++
    # library to build) and one thread per process, since several games run side by side here
    sys.path.insert(0, os.path.join(root, 'wotr_bridge'))
    import launch_head
    for key, value in launch_head.HEAD_ENV:
        os.environ[key] = value
    os.environ['WOTR_LIVE_SEARCH_BACKEND'] = backend
    os.environ['WOTR_LIVE_POLICY_THREADS'] = '1'

    spec = importlib.util.spec_from_file_location('wotr_protocol_bot', os.path.join(root, 'protocol', 'wotr_protocol_bot.py'))
    bot = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(bot)

    game = {'n': 0, 'seed': 0}          # the referee's seed for the bot, new with every game
    memo_init = bot.GameMemo.__init__

    def memo(self, side, seed, *more, **named):
        game['n'] += 1
        game['seed'] = int(seed)
        memo_init(self, side, seed, *more, **named)
    bot.GameMemo.__init__ = memo

    class Searcher(bot.Searcher):
        """bot.Searcher with the server's search and the profile's network in place of the corpus_live pair."""

        def __init__(self, sims, max_seconds, threads=1):
            from core import own_model
            self.own_model = own_model
            self.sims = int(sims)
            self.max_seconds = max_seconds
            self.rates = {}
            self.recent = {}
            self.seeded = -1
            ai = bot.ai
            ai.torch.set_num_threads(int(threads))
            t0 = time.time()
            self.own = {}
            self.redo = {}
            self.redone = 0
            self.speeds = {}; self.took = []; self.simsum = 0
            self.asked = [0, 0, 0]              # searches; those run with fewer searches than asked for; with under half
            import atexit
            atexit.register(lambda: bot.log('SEARCHES %d, cut short by the clock %d, to under half %d; seconds a search: mean %.2f, longest %.2f, over 1.5 s: %d; simulations a search: %.0f' % (tuple(self.asked) + (sum(self.took) / max(1, len(self.took)), max(self.took or [0]), sum(t > 1.5 for t in self.took), self.simsum / max(1, self.asked[0])))))
            for side in (bot.FP, bot.SH):
                net = own_model.load_own_net(tag, side)
                self.own[side] = own_model.OwnModelSearch(side, net, seed=0, backend=backend)
                # the compiled search makes its own menus, so it can pick a move the referee has no option for;
                # that decision is then searched again in Python, where such moves are left out
                self.redo[side] = self.own[side] if backend == 'python' else own_model.OwnModelSearch(side, net, seed=0, backend='python')
            ai.torch.set_num_threads(int(threads))
            bot.log('nets %s loaded in %.1f s (profiles: fp %s, sh %s)' % (
                tag, time.time() - t0, self.own[bot.FP].profile, self.own[bot.SH].profile))

        def budget(self, kind, left_ms):
            """With --seconds S: as many searches as this kind of decision has lately managed in S seconds (the middle
            of its last five speeds), up to the --sims cap; a search cannot be stopped midway, so this is an aim.
            Without it: the bot's own rule (40% of the referee's clock at the slowest of the last three speeds)."""
            if not seconds:
                return bot.Searcher.budget(self, kind, left_ms)
            full = bot.engine_play._sims_for_kind(kind, self.sims)
            past = self.speeds.get(kind) or [x for v in self.speeds.values() for x in v]
            rate = sorted(past)[len(past) // 2] if past else 60.0
            return max(8, min(full, int(rate * seconds)))

        def choose(self, st, side, kind, banned, flag=None, left_ms=None):
            ai = bot.ai
            if self.seeded != game['n']:        # a new game: the searches start from the referee's seed
                self.seeded = game['n']
                for s in self.own:
                    for own in {id(o): o for o in (self.own[s], self.redo[s])}.values():
                        own.seed = self.own_model.seat_seed(game['seed'], s)
                        own.index = 0
            sims = self.budget(kind, left_ms)
            full = bot.engine_play._sims_for_kind(kind, self.sims)
            self.asked[0] += 1
            self.simsum += sims
            self.asked[1] += sims < full            # the clock cut this search short
            self.asked[2] += sims * 2 < full
            dump = os.environ.get('ALPHA2_DUMP')    # keep the positions searched, for timing the search outside a game
            if dump:
                import pickle
                with open(os.path.join(dump, 'pos-%d-%04d.pkl' % (game['seed'], self.asked[0])), 'wb') as f:
                    pickle.dump({'state': st, 'side': side, 'kind': kind, 'flag': flag, 'full': full}, f)
            ai._TREE_CACHE['child'] = None
            ai._TREE_CACHE['sig'] = None
            orig = ai.get_legal_actions

            def legal(st9, side9):
                out = orig(st9, side9)
                if side9 is side and banned:
                    kept = [a for a in out if bot.act_key(a) not in banned]
                    if kept:
                        return kept
                return out

            ai.get_legal_actions = legal
            t0 = time.time()
            try:
                with bot._Phase(st, flag):
                    try:
                        action, info = self.own[side].choose(st, side, num_simulations=sims)
                    except Exception as error:      # a position the compiled search refuses: searched in Python
                        if self.redo[side] is self.own[side]:
                            raise
                        self.redone += 1
                        bot.log('  compiled search failed (%s); searched again in Python (%d so far)' % (str(error)[:120], self.redone))
                        action, info = self.redo[side].choose(st, side, num_simulations=sims)
                    if action is not None and banned and bot.act_key(action) in banned and self.redo[side] is not self.own[side]:
                        self.redone += 1
                        bot.log('  compiled search picked a move without a referee option; searched again in Python (%d so far)' % self.redone)
                        action, info = self.redo[side].choose(st, side, num_simulations=sims)
            finally:
                ai.get_legal_actions = orig
            secs = time.time() - t0
            if secs > 0.02:
                self.speeds[kind] = (self.speeds.get(kind) or [])[-4:] + [sims / secs]
                self.took.append(secs)
            if secs > 0.05 and sims >= 8:
                recent = (self.recent.get(kind) or [])[-2:] + [sims / secs]
                self.recent[kind] = recent
                self.rates[kind] = min(recent)
            value = None
            try:
                if info is not None:
                    value = float(info['q_values'][int(info['played'])])
            except Exception:
                value = None
            return action, {'sims': sims, 'secs': round(secs, 2), 'value': value}

    bot.Searcher = Searcher

    # which of the engine's moves have no referee option: counted by question kind and move type, printed at the end
    import atexit
    import collections
    seen, missing = collections.Counter(), collections.Counter()
    map_action = bot.AlphaWotrBot.map_action

    def counted(self, st, dec, a):
        key = (dec.get('kind'), a.action_type)
        if a.action_type in ('army_move', 'army_attack') and dec.get('kind') == 'ACTION':
            # the die it would pay with, and whether the referee offers the same kind of action on another die
            try:
                dice = st.dice[self.memo.side]
                face = dice[a.die_idx].current.name if 0 <= a.die_idx < len(dice) else '?'
            except Exception:
                face = '?'
            want = ('move_armies', 'move_army_with_leader') if a.action_type == 'army_move' else ('attack', 'attack_with_leader')
            other = any(o.get('kind') == 'use_die' and o.get('action') in want for o in dec.get('options') or [])
            key = (dec.get('kind'), '%s die=%s%s' % (a.action_type, face, ' (offered on another die)' if other else ' (not offered on any die)'))
        seen[key] += 1
        try:
            mt = map_action(self, st, dec, a)
        except Exception:
            missing[key] += 1
            raise
        if mt is None:
            missing[key] += 1
        return mt
    bot.AlphaWotrBot.map_action = counted

    def report():
        for key, n in missing.most_common(40):
            bot.log('NO-OPTION %-16s %-62s %6d of %6d' % (key[0], key[1], n, seen[key]))
    atexit.register(report)
    bot.BOT_VERSION = 'v0.2.0-%s-strength1/protocol-1' % tag
    if load_only:
        import argparse
        os.dup2(2, 1)
        bot.import_engine(os.path.join(root, 'src'))
        bot.Names()
        Searcher(300, None)
        import resource
        bot.log('load-only: ok; peak memory %.0f MB' % (resource.getrusage(resource.RUSAGE_SELF).ru_maxrss / 2 ** 20))
        return
    bot.main(argv + ['--src', os.path.join(root, 'src')])


if __name__ == '__main__':
    main()
