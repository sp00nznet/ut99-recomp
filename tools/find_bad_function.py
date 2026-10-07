#!/usr/bin/env python3
"""Find the lifted function that breaks a module, by bisection against its original code.

The host can run one module's original machine code (`--native`) with a chosen
set of its functions lifted after all (`--lift-in-native FILE`, pcrecomp
native32_lift_in_native). If the picture is right with the module native and
wrong with it lifted, the bad function is in whichever half of its functions,
lifted, still breaks it; halve that half, and so on (docs/bringup.md 10).

"Right" is measured without lining frames up in time, because lifted and native
runs go at different speeds: every frame of a run is scored by its closest
match anywhere in a baseline run with the module wholly native (64x48 grey,
mean squared difference). The intro flyby always takes the same path, so a
clean frame has a near twin somewhere in the baseline and a broken one does
not. A run's score is the median over its frames; the threshold sits between
the all-native and all-lifted scores, measured first.

    py -3 tools/find_bad_function.py Render.dll                 # the whole search
    py -3 tools/find_bad_function.py Render.dll --only work\\bisect\\half.txt   # score one set
Needs ffmpeg and numpy; runs headless (nothing on screen).
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('UT99_EXE', os.path.join(ROOT, 'build-farm', 'ut99.exe'))
OUT = os.path.join(ROOT, 'work', 'bisect')
W, H = 64, 48
FRAMES = 1800            # 60 s at the recorder's 30 fps


def run(module, lifted, tag):
    """Record a headless run with `module` native except `lifted`; its frames, small and grey."""
    mp4 = os.path.join(OUT, tag + '.mp4')
    args = [EXE, '--run', '--headless', '--native', module, '--record', mp4,
            '--frames', str(FRAMES), '--watchdog', '150']
    if lifted is not None:
        lst = os.path.join(OUT, tag + '.txt')
        with open(lst, 'w') as f:
            f.writelines('0x%08X\n' % a for a in lifted)
        args += ['--lift-in-native', lst]
    try:
        os.remove(os.path.join(ROOT, 'game', 'System', 'Running.ini'))
    except OSError:
        pass
    with open(os.path.join(OUT, tag + '.log'), 'w') as log:
        subprocess.run(args, stdout=log, stderr=subprocess.STDOUT, timeout=240)
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', mp4, '-vf', 'scale=%d:%d' % (W, H),
                          '-pix_fmt', 'gray', '-f', 'rawvideo', '-'], capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, W * H).astype(np.float32)


def score(frames, base):
    """Median over frames of the distance to the nearest baseline frame.
    The first quarter is skipped: it is the dark first corridor, alike everywhere."""
    d = [((base - f) ** 2).mean(axis=1).min() for f in frames[len(frames) // 4::15]]
    return float(np.median(d))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('module')
    ap.add_argument('--only', help='score one list of VAs (lifted inside the native module)')
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    funcs = json.load(open(os.path.join(ROOT, 'work', args.module + '.entries.json')))
    base = run(args.module, None, 'native')
    if args.only:
        vas = [int(l, 16) for l in open(args.only) if l.startswith('0x')]
        print('score %.1f' % score(run(args.module, vas, 'only'), base))
        return
    good = score(run(args.module, [], 'native2'), base)
    bad = score(run(args.module, funcs, 'lifted'), base)
    print('all native %.1f, all lifted %.1f' % (good, bad), flush=True)
    if bad < good * 2:
        sys.exit('the module lifted is not measurably worse than native: nothing to bisect')
    limit = (good + bad) / 2
    suspects, step = funcs, 0
    while len(suspects) > 1:
        half = suspects[:len(suspects) // 2]
        s = score(run(args.module, half, 'step%02d' % step), base)
        broken = s > limit
        print('step %d: %d functions lifted, score %.1f -> %s' % (step, len(half), s,
              'broken: the bad one is among them' if broken else 'clean: it is in the other half'), flush=True)
        suspects = half if broken else suspects[len(suspects) // 2:]
        step += 1
    print('the function that breaks %s: sub_%08X' % (args.module, suspects[0]))


if __name__ == '__main__':
    main()
