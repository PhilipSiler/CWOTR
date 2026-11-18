#!/usr/bin/env python3
"""Plays games between our player and an outside bot through the local referee (bridge referee).

  python3 play.py <alpha|alpha2|random> <player|prior> <games> [first seed=1] [clock ms=10000] [sims=0: the bot's default]

Each game is its own pair of processes: the referee (our engine, with our player on one side) and the outside bot,
joined by their standard input and output. The sides go by seed: the outside bot has the Free Peoples on odd seeds.
The referee's log of game N goes to games/<name>-<seed>.log, the bot's own log to games/<name>-<seed>.bot.log, and
one line per finished game to games/results.txt. Nothing is sent over the network.
With WOTR_STATES=1 in the environment the referee also writes games/<name>-<seed>.states: its true engine State,
raw (sizeof(State) bytes each), at the first decision of every turn, and <name>-<seed>.states.txt, one line per record.
With WOTR_DECISIONS=1 it writes games/<name>-<seed>.decisions: every decision of both sides, as the true State just
before it followed by the 8-byte Move played, and <name>-<seed>.decisions.txt, one line per record.
alpha2 is the new AlphaWOTR release through alpha2_bot.py (new networks, ordinary search); its profile comes from the
environment: ALPHA2_FP=win|mil (FP-Win, FPMV) and ALPHA2_SH=win|cor (SP-Win, SP corruption), both "win" when not set.
ALPHA2_BACKEND=native uses its compiled search; then set ALPHA2_ROOT to our copy of the release (proto/alpha2-copy).
"""
import os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
BOTS = {
    # AlphaWOTR needs the package "shapely", which is installed in pydeps/ here (not in the system Python)
    "alpha": (["python3", "protocol/wotr_protocol_bot.py", "--verbose"], os.path.join(ROOT, "AlphaWOTR")),
    # the new release, run by its own Python (which has its packages), through our wrapper
    "alpha2": ([os.path.join(ROOT, "AlphaWOTR 2", "python", "bin", "python3"), "-B", os.path.join(HERE, "alpha2_bot.py"),
                "--root", os.environ.get("ALPHA2_ROOT", os.path.join(ROOT, "AlphaWOTR 2")), "--backend", os.environ.get("ALPHA2_BACKEND", "python"),
                "--fp", os.environ.get("ALPHA2_FP", "win"),
                "--sh", os.environ.get("ALPHA2_SH", "win"), "--verbose"], HERE),
    "random": (["python3", os.path.join(ROOT, "wotr-protocol", "examples", "python", "random_bot.py"), "--events"], HERE),
}

def main():
    name, player, games = sys.argv[1], sys.argv[2], int(sys.argv[3])
    seed0 = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    clock = sys.argv[5] if len(sys.argv) > 5 else "10000"
    sims = sys.argv[6] if len(sys.argv) > 6 else "0"
    cmd, cwd = BOTS[name]
    out = os.path.join(HERE, "games")
    os.makedirs(out, exist_ok=True)
    for g in range(games):
        seed = seed0 + g
        side = "fp" if seed % 2 == 1 else "sh"   # odd seed: the outside bot has the Free Peoples
        base = os.path.join(out, "%s-%d" % (name, seed))
        t0 = time.time()
        with open(base + ".log", "w") as rlog, open(base + ".bot.log", "w") as blog:
            renv = dict(os.environ)
            if os.environ.get("WOTR_STATES"):
                renv["WOTR_STATES_FILE"] = base + ".states"
            if os.environ.get("WOTR_DECISIONS"):
                renv["WOTR_DECISIONS_FILE"] = base + ".decisions"
            ref = subprocess.Popen([os.path.join(HERE, "bridge"), "referee", player, side, str(seed), clock, sims],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=rlog, env=renv)
            env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")   # leave the bot's folder as it is
            if name != "alpha2":
                env["PYTHONPATH"] = os.path.join(HERE, "pydeps")
            bot = subprocess.Popen(cmd, cwd=cwd, stdin=ref.stdout, stdout=ref.stdin, stderr=blog, env=env)
            ref.stdout.close(); ref.stdin.close()
            code = ref.wait()
            try:
                bot.wait(timeout=30)
            except subprocess.TimeoutExpired:
                bot.kill()
        result = [l for l in open(base + ".log") if l.startswith("RESULT")]
        line = (result[0].strip() if result else "NO RESULT game=%s-%d referee_exit=%d" % (name, seed, code)) + " seconds=%d" % (time.time() - t0)
        with open(os.path.join(out, "results.txt"), "a") as f:
            f.write(line + "\n")
        print(line, flush=True)

if __name__ == "__main__":
    main()
