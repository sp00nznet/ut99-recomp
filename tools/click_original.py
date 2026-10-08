#!/usr/bin/env python3
"""Scripted clicks into the original game, for reference recordings.

The recompiled host scripts input itself (`--key fire@S`); the original has no
such switch, so this posts the same mouse clicks to its window, found by its
title. Start it, then the original under offstage (which keeps only the
program it starts, and that program's windows, on its virtual monitor):

    start py -3 tools\\click_original.py 14 17 20
    offstage run --size 1280x720 --record ref.mp4 -- work\\ref\\System\\UnrealTournament.exe "DM-Deck16][?game=Botpack.DeathMatchPlus?numbots=4"

Times are seconds from when the window first appears; the game is closed
--stay seconds (default 30) after the last click.
"""
import ctypes
import sys
import time

user32 = ctypes.windll.user32
WM_LBUTTONDOWN, WM_LBUTTONUP, WM_CLOSE, MK_LBUTTON = 0x201, 0x202, 0x10, 1


def main():
    argv = sys.argv[1:]
    stay = 30.0
    if argv[:1] == ['--stay']:
        stay, argv = float(argv[1]), argv[2:]
    clicks = [float(a) for a in argv]
    hwnd = None
    for _ in range(600):
        hwnd = user32.FindWindowW(None, 'Unreal Tournament')
        if hwnd:
            break
        time.sleep(0.1)
    if not hwnd:
        sys.exit('no Unreal Tournament window')
    t0 = time.time()
    for at in clicks:
        time.sleep(max(0, t0 + at - time.time()))
        r = (ctypes.c_long * 4)()
        user32.GetClientRect(hwnd, r)
        pos = (r[3] // 2) << 16 | (r[2] // 2)
        user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos)
        time.sleep(0.05)
        user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, pos)
        print('[click] %.1f s' % (time.time() - t0), flush=True)
    time.sleep(max(0, t0 + (clicks[-1] if clicks else 0) + stay - time.time()))
    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)


if __name__ == '__main__':
    main()
