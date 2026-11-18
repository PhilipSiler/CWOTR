#!/usr/bin/env python3
"""Fetch finished-game move logs from the war-of-the-ring.pages.dev public report API.
Polite by design: 100 games per request (the API maximum), a pause between requests, resumable.
Output: data/logs/games.jsonl (one game per line)."""
import json, os, subprocess, sys, time, urllib.parse
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'data', 'logs', 'games.jsonl')
BASE = 'https://war-of-the-ring.pages.dev/api/reports'
seen, since = set(), None
if os.path.exists(OUT):
    for line in open(OUT):
        g = json.loads(line); seen.add(g['reportId'])
        if since is None or g['createdAt'] > since: since = g['createdAt']
out = open(OUT, 'a')
for page in range(40):
    q = {'category': 'wotr-gamelog', 'full': '1', 'limit': '100', 'order': 'asc'}
    if since: q['since'] = since
    # curl rather than urllib: the python.org macOS build ships without a CA bundle
    raw = subprocess.run(['curl', '-sS', '-m', '180', '-A', 'wotr-research-client', BASE + '?' + urllib.parse.urlencode(q)],
                         check=True, capture_output=True).stdout
    d = json.loads(raw)
    new = [g for g in d['reports'] if g['reportId'] not in seen]
    for g in new:
        seen.add(g['reportId']); out.write(json.dumps(g) + '\n')
        if since is None or g['createdAt'] > since: since = g['createdAt']
    out.flush()
    print(f'page {page}: {len(d["reports"])} returned, {len(new)} new, total saved {len(seen)}, server total {d.get("total")}', flush=True)
    if not new: break
    time.sleep(8)
