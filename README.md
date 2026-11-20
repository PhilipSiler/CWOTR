# CWOTR — War of the Ring (2nd ed., base game) engine and self-play learner

Everything that matters is C11; Python is only used to generate data tables and fetch logs.

## Layout

| Path | What |
| --- | --- |
| `src/wotr.h` | Types, public API, internal declarations |
| `src/core.c` | Setup, unit/politics/control primitives, frame runner |
| `src/movegen.c` | Legal move generation |
| `src/flow.c` | Turn phases, action dice, simple action chains |
| `src/fellowship.c` | Fellowship phase, the Hunt, reveal, separation, Mordor |
| `src/battle.c` | Battles, sieges, sorties, the 38 combat cards |
| `src/cards.c` | The 96 event cards |
| `src/data.c`, `src/data.h` | Generated map and card tables (`tools/gen_data.py`) |
| `src/features.c`, `src/nn.c`, `src/agent.c`, `src/train.c` | State encoding, value network, search, self-play trainer |
| `tests/test_rules.c` | Scenario and statistical rules tests |
| `tests/fuzz.c` | Random-playout fuzzer with invariant checks at every decision |
| `tools/fetch_logs.py` | Downloads public game logs into `data/logs/games.jsonl` |
| `tools/oracle/` | Differential test of legal-move enumeration against the independent TypeScript engine in `../war-of-the-ring` |
| `tests/inject.c` | Loads a position from that engine into ours and lists the full generator's moves |
| `docs/` | Rulebook and FAQ as text |

## Build and run

```
make                 # bin/fuzz, bin/test_rules, bin/wotr
make test            # rules tests + 3000 fuzz games
./bin/wotr train runs/main 400 320 8     # self-play: <outdir> <iterations> <games/iter> <threads>; resumes from <outdir>/latest
./bin/wotr eval runs/main/latest prior 400 8   # first plays Free Peoples, second plays Shadow ("prior" = untrained)
./bin/wotr show runs/main/latest 7       # print one game move by move (seed 7)
```

## Engine design

A `State` (3.3 KB, plain data) always sits at a decision (`dkind`, `dplayer`) or at game end.
`wotr_moves` lists the legal moves, `wotr_apply` plays one and runs forward to the next decision,
resolving dice, tile and card draws with the RNG stored in the state. Rules are written as
resumable coroutines on an explicit frame stack (`CO_BEGIN / ASK / CALL`), so multi-step
procedures (a battle inside an event card inside an action) read top to bottom.

## How the learner works

* **Value learning.** Two small MLPs (one for positions where the Free Peoples decide, one for Shadow)
  predict P(Free Peoples win) plus auxiliary targets (final corruption, final Shadow VP,
  ring-vs-military ending, turns remaining).
* **Search.** For each candidate move: apply it, sample the dice, expand the mover's own follow-up
  choices where the intermediate position means nothing, evaluate with the network. Positions are
  evaluated incrementally against the root (only changed features touch the first layer). The
  opponent's hand is reshuffled before search so hidden cards cannot leak.
* **Move policy.** A linear scorer over the value net's hidden layer ranks all legal moves without
  applying them, so most self-play decisions search only its top few moves (see Speed).
* **Exploration persona.** In a quarter of self-play games Shadow's move choice is tilted toward the
  strategy guide's military priorities (victory points, sieges with leadership, garrisons, minions).
  Training targets always come from real outcomes, never from the tilt.
* **Strategy-guide prior.** The win logit is `linear(prior terms) + network`, and the network's output
  layer starts at zero. The prior terms come from `strategy.txt`: ring-quest margin, road left to
  Mount Doom, Shadow VP, danger to each side's settlements, Shadow force projection, extra action dice.
  This is potential-based reward shaping in its equivalent "initial value function" form, so the
  learned policy still optimizes the true win rate.
* **Training.** Self-play with TD(lambda) targets built from later search values and anchored on the
  real result; a quarter of the threads play against older checkpoints.

## Speed

Self-play runs at roughly 1,000 games/s on an M1 Pro (9 game threads plus fitting), about 250x the first
working version. What made the difference, in order of effect:

1. Structure-aware move generation (below): about a third of the candidate moves per decision.
2. A move policy that lets most decisions search only its top few moves. Decisions come in three tiers:
   full width (10%, these produce the policy's training targets), top 8 + 2 random, and a quick top 3 + 1.
3. Change-list evaluation. A searched position is encoded as its difference from the root (`feat_delta`):
   only regions whose pieces changed, the victory-point settlements near them, and the global blocks whose
   inputs changed are re-encoded, typically ~10 features. The first layer is updated with just those
   (`nn_forward_list`), and the root's first-layer sums are carried from one decision to the next.
4. Fitting runs on its own threads while the next batch of games is played with a frozen copy of the nets.

`tests/bench_nn.c` checks the change-list path against a full encode and forward pass on every child of
several hundred positions (outputs agree to ~3e-7) and prints the per-position costs.

## Structure-aware move generation (`wotr_prune = 1`, the default)

The raw move space is dominated by moves with no purpose. The generator keeps:

* **Army moves** that shorten the shortest path to an enemy-held victory-point settlement or to an own
  threatened settlement, merge with a friendly army, or enter a settlement. "Leave a garrison" is offered
  only from own settlements and sieges; "detach one unit" only to take or hold an empty settlement.
* **Second army move / second muster** in one canonical order (A-then-B, not also B-then-A).
* **Nazgul, Witch-king, Mouth** as an assignment: a handful of plans that send the leadership an army can
  actually use (re-rolls are capped by its dice) to one or two front-line armies, optionally with one
  Nazgul on the Fellowship; the rest stay put.
* **Companions** (moving or separating) only toward armies that may fight, settlements they can rouse,
  Strider's crowning cities, Fangorn, their home strongholds - or the reachable region nearest to one.
* **Fellowship declare** only at havens, the gates of Mordor, or the spot nearest Mordor; **reveal**
  only the Pareto front over (extra Hunt tiles on the path, distance to Mordor, Hunt re-rolls there).
* **Elven rings** only to gain an action no die offers; one "discard a die" option; the Witch-king
  enters at a front-line army.

`wotr_prune = 0` restores every legal move the engine models (used by the rules tests and for log replay).
Average branching drops from about 89 to 30 moves per decision.

## Known simplifications (deliberate)

* Army splits come from a small menu (all / all but one / one unit / half); leaders move all-or-none.
  `wotr_apply` accepts any unit counts, only the generator is restricted.
* Attack rearguard is none or one unit; leaders and characters always join the attack.
* Card-driven army moves relocate the whole army (with `wotr_prune = 0` *Shadows Gather* / *The Shadow Lengthens*
  offer a destination where only a part would fit; the split is then the caller's).
* Nazgul destinations are limited to frontline Shadow armies and the Fellowship's region.
* Companions separate as singles or all together (`wotr_prune = 0`: any group).
* The cheapest sufficient action die pays for an action.
* Auto-resolved minor choices: Mighty Attack / Anduril, Elite-over-Regular for card recruits, Stormcrow's casualty,
  the cards paid to discard A Power too Great.
* Army moves that would overfill a region (more than ten units) are not offered. Recruiting may overfill one; the
  owner then removes the excess when the action ends (RB p. 26-27).

The player's other choices are asked with the generic option question (`D_CARD_OPTION`, `State.qtag` naming it):
The White Rider, Words of Power, Heroic Death, Black Breath, Blade of Westernesse, Fateful Strike, Dread and Despair,
Flocks of Crebain, Balrog of Moria, Worn with Sorrow and Toil, The Nazgul Strike, Lure of the Ring, The Black Captain
Commands' and The Ringwraiths Are Abroad's choices, the excess units; the Guide on a tie and entering Mordor are
answers of the Fellowship phase's one question (`FSP_GUIDE + c`, `FSP_MORDOR`). There is no turn limit.

## Checking the legal-move list against an independent engine

```
cd tools/oracle && npm install --cache ./.npm-cache          # once: vite-node and the other engine's one dependency
npx vite-node record.mjs 60 100 /tmp/pos.jsonl 0.08          # play 60 games in the TypeScript engine, sample positions
cd ../.. && make bin/inject && python3 tools/oracle/compare.py /tmp/pos.jsonl
```

For every sampled action decision the two legal sets are compared. Moves only we list are put to the other
engine's own command validator (its `legalActions` samples some move families); moves only it lists are reported
unless they fall in a family our full generator leaves out on purpose (over-stacking merges, Elven Ring changes to a
dominated face). We follow that engine's ruling that an Event card which could change nothing may not be played
(`card_has_effect` in `src/cards.c` tries the play on a copy with every die rolling its best).
`show.py <file> <seed> <step> [prefix]` prints one position and its differences.
