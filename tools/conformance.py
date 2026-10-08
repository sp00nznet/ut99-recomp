#!/usr/bin/env python3
"""Unreal Tournament conformance harness (REPO_RULES section 9).

Two counts, compared against the committed baseline in conformance.json; a
regression fails the run:

* **Boot milestones**: three headless runs of the host, scored by the engine's
  own log (`--log`), the frames it drew, two looks at the picture, and a
  server-browser query. Each milestone is something the original does on
  every start (docs/bringup.md has the run that showed each one):
    - intro: the launcher's default, the city flyby;
    - dm: a Deathmatch on DM-Deck16][ against four bots, started from the
      command line, the player clicking fire to join (`--key fire@S`);
    - server: UCC's dedicated server, asked what it runs (tools/query.py).
* **Lift health**: work/lift_stats.json from run_lift.py: lift errors,
  bodies with no terminator, instructions lifted as nothing.

The game is not in the repo. Without game/ and a built host this skips with a
message and exits 0, so it can sit in CI without the corpus.

    py -3 tools/conformance.py              # run, compare, print the table
    py -3 tools/conformance.py --update     # ...and accept the result as the baseline
Needs ffmpeg and numpy (the picture checks). About two minutes. Frame counts are only
"it renders": they depend on what else the machine is doing.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from query import query  # noqa: E402

HOSTS = [os.path.join(ROOT, 'build-farm', 'ut99.exe'), os.path.join(ROOT, 'build', 'ut99.exe')]
OUT = os.path.join(ROOT, 'work', 'conformance')
BASELINE = os.path.join(ROOT, 'conformance.json')
DM = 'DM-Deck16][?game=Botpack.DeathMatchPlus?numbots=4'

RUNS = {
    'intro': ['--frames', '900'],
    'dm': ['--frames', '1500', '--key', 'fire@14', '--key', 'fire@17', '--key', 'fire@20', '--', DM],
}


def drawn(out):
    m = re.search(r'the game drew (\d+)', out)
    return int(m.group(1)) if m else 0


def frames(mp4):
    """The recording at 640x480 grey, two frames a second."""
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', mp4, '-vf', 'scale=640:480,fps=2',
                          '-pix_fmt', 'gray', '-f', 'rawvideo', '-'], capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, 480, 640)


def lit(mp4):
    """The flyby is drawn: by 30 s it leaves the dark corridor for the city, so
    the picture both moves and gets bright (a dead renderer does neither)."""
    f = frames(mp4)
    m = f.reshape(len(f), -1).mean(axis=1) if len(f) else np.zeros(1)
    return m.max() > 25 and m.std() > 3


def hud_translucent(mp4):
    """The HUD's speech area (top-left) is translucent, as in the original: it is
    never a flat black box. It was, while MMX paddusb lifted as nothing
    (docs/bringup.md 16). Scored over the match, after the first 40%."""
    f = frames(mp4)
    f = f[len(f) * 2 // 5:, 4:56, 70:370].astype(np.float32)
    if not len(f):
        return False
    flat = (f.mean(axis=(1, 2)) < 6) & (f.std(axis=(1, 2)) < 4)
    return flat.mean() < 0.1


# (name, run, check): check is a regex on the run's output, or a function of
# (output, recording). Order is the order things happen.
MILESTONES = [
    ('every module mapped and bound, 0 unresolved', 'intro',
     lambda out, _: out.count('[bind]') >= 13 and not re.search(r'[1-9]\d* unresolved', out)),
    ('engine initialized', 'intro', r'\[log\] Unreal engine initialized'),
    ('intro level up for play', 'intro', r'Bringing Level CityIntro\.MyLevel up for play'),
    ('sound: Galaxy initialized on DirectSound', 'intro', r'\[log\] Galaxy initialized'),
    # It read -2147483648 while _ftol ran on the real FPU (docs/bringup.md 11).
    ('the clock runs (startup time a few seconds)', 'intro', r'Startup time: \d{1,2}\.\d+ seconds'),
    ('intro: 200 frames drawn', 'intro', lambda out, _: drawn(out) >= 200),
    ('intro: the flyby reaches the lit city', 'intro', lambda _, mp4: lit(mp4)),
    ('Deathmatch level up for play', 'dm', r"Game class is 'DeathMatchPlus'[\s\S]*Bringing Level DM-Deck16\]\[\.MyLevel up for play"),
    ('match: 300 frames drawn, no crash', 'dm',
     lambda out, _: drawn(out) >= 300 and '=== fault' not in out and '[exit] 0' in out),
    ('match: the HUD speech area is translucent', 'dm', lambda _, mp4: hud_translucent(mp4)),
    ('dedicated server answers a query with its map', 'server',
     lambda out, _: 'mapname DM-Deck16][' in out),
]


def clear_marker():
    try:
        os.remove(os.path.join(ROOT, 'game', 'System', 'Running.ini'))   # else Recovery Mode
    except OSError:
        pass


def run(host, name):
    mp4 = os.path.join(OUT, name + '.mp4')
    args = [host, '--run', '--headless', '--log', '--watchdog', '150', '--record', mp4] + RUNS[name]
    clear_marker()
    try:
        p = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, errors='replace', timeout=200)
        out = p.stdout + p.stderr + '\n[exit] %d\n' % p.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode(errors='replace') + '\n[exit] timeout\n'
    return out, mp4


def server(host):
    """UCC's dedicated server, queried once a second for up to 40 s."""
    clear_marker()
    log = os.path.join(OUT, 'server.out')
    with open(log, 'w') as f:      # a file, not a pipe: nothing reads a pipe while it runs
        p = subprocess.Popen([host, '--run', '--ucc', '--headless', '--log', '--watchdog', '90', '--',
                              'server', 'DM-Deck16][?game=Botpack.DeathMatchPlus'],
                             cwd=ROOT, stdout=f, stderr=subprocess.STDOUT)
        info = None
        for _ in range(60):
            time.sleep(1)
            info = query(timeout=1.0)
            if info or p.poll() is not None:
                break
        p.kill()
        p.wait()
    out = open(log, encoding='utf-8', errors='replace').read()
    return out + ('\nmapname %s\n' % info.get('mapname') if info else '\nno answer\n'), None


def lift_health():
    stats = json.load(open(os.path.join(ROOT, 'work', 'lift_stats.json')))
    total = lambda k: sum(s.get(k, 0) for s in stats.values())
    return {'lifted': total('lifted'), 'errors': total('errors'), 'no_terminator': total('no_terminator'),
            'unimplemented': sum(sum(s.get('unimplemented', {}).values()) for s in stats.values())}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--update', action='store_true', help='accept this run as the baseline')
    ap.add_argument('--host', help='the host to run (default build-farm/ut99.exe, else build/ut99.exe)')
    args = ap.parse_args()
    host = args.host or next((h for h in HOSTS if os.path.exists(h)), None)
    if not (host and os.path.exists(host) and os.path.isdir(os.path.join(ROOT, 'game', 'System'))):
        print('conformance: skipped -- needs game/ (your copy) and a built ut99.exe '
              '(README, Building from source)')
        return 0
    host = os.path.abspath(host)
    os.makedirs(OUT, exist_ok=True)

    results = {name: run(host, name) for name in RUNS}
    results['server'] = server(host)
    for name, (out, _) in results.items():
        open(os.path.join(OUT, name + '.log'), 'w', encoding='utf-8').write(out)
    passed = []
    for name, which, check in MILESTONES:
        out, mp4 = results[which]
        if (re.search(check, out) if isinstance(check, str) else check(out, mp4)):
            passed.append(name)
    health = lift_health()
    now = {'milestones': len(passed), 'of': len(MILESTONES), **health}
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else None

    print('boot milestones: %d/%d' % (len(passed), len(MILESTONES)))
    for name, which, _ in MILESTONES:
        print('  [%s] %-6s %s' % ('x' if name in passed else ' ', which, name))
    print('  frames drawn: intro %d, match %d (logs and recordings in work/conformance)'
          % (drawn(results['intro'][0]), drawn(results['dm'][0])))
    print('lift: %(lifted)d functions, %(errors)d errors, %(no_terminator)d with no terminator, '
          '%(unimplemented)d instructions lifted as nothing' % health)

    worse = []
    if base:
        if now['milestones'] < base['milestones']:
            worse.append('milestones %d -> %d' % (base['milestones'], now['milestones']))
        for k in ('errors', 'no_terminator', 'unimplemented'):
            if now[k] > base[k]:
                worse.append('%s %d -> %d' % (k, base[k], now[k]))
    if args.update or not base:
        json.dump(now, open(BASELINE, 'w'), indent=1)
        print('baseline written to conformance.json')
    if worse:
        print('REGRESSION: ' + '; '.join(worse))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
