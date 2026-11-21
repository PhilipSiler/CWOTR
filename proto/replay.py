#!/usr/bin/env python3
"""Offline replay of a recorded game into Hambot: replay.py RECORD.protocol.jsonl SIDE | hambot PLAYER

Sends Hambot the lines the referee sent SIDE, and before every decision of either side a {"type":"check"} line with
that decision's view: Hambot then compares its copy with it in a child process (the log line CHECK) without changing
its own course. Hambot's answers are not read: the record's events drive the game."""
import json, sys
rec, side = sys.argv[1], sys.argv[2]
out = sys.stdout
for raw in open(rec):
    r = json.loads(raw)
    if r.get("dir") != "out":
        continue
    m = json.loads(r["line"])
    d = None
    if m.get("type") == "decide" and m.get("view"):
        d = m.get("decision") or {}
        if d.get("kind") not in ("ACTION", "HUNT_ALLOCATION", "FELLOWSHIP_PHASE"):
            d = None                     # mid-move, mid-card, mid-Hunt: the copy and the view may be half-way apart
    if m.get("type") == "decide" and m.get("view") and d is not None:
        at = "%s %s #%s" % (r["side"], d.get("kind"), m.get("id"))
        out.write(json.dumps({"type": "check", "at": at, "view": m["view"]}) + "\n")
    if r["side"] == side:
        out.write(r["line"] + "\n")
out.write('{"type":"quit"}\n')
