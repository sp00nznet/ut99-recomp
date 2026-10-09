#!/usr/bin/env python3
"""Scripted clicks into the original game, for reference recordings.

The recompiled host scripts input itself (`--key fire@S`); the original has no
such switch, so this posts the same mouse clicks to its window, found by its
title. Start it, then the original under offstage (which keeps only the
program it starts, and that program's windows, on its virtual monitor):

    start py -3 tools\\click_original.py 14 17 20
    offstage run --size 1280x720 --record ref.mp4 -- work\\ref\\System\\UnrealTournament.exe "DM-Deck16][?game=Botpack.DeathMatchPlus?numbots=4"

Times are seconds from when the window first appears; the game is closed
--stay seconds (default 30) after the last click. A click is in the middle of
the window, or at client pixel X,Y as T@X,Y (a mouse move there first, which
is how UWindow's menus follow the cursor). It drives the recompiled game's
menus the same way (the window has the same title).
"""
import ctypes
import sys
import time

user32 = ctypes.windll.user32
WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_CLOSE, MK_LBUTTON = 0x200, 0x201, 0x202, 0x10, 1
KEYS = {'esc': 0x1B, 'enter': 0x0D, 'left': 0x25, 'up': 0x26, 'right': 0x27, 'down': 0x28}


def main():
    argv = sys.argv[1:]
    stay = 30.0
    if argv[:1] == ['--stay']:
        stay, argv = float(argv[1]), argv[2:]
    clicks = [(float(a.split('@')[0]), a.split('@')[1] if '@' in a else None) for a in argv]
    hwnd = None
    for _ in range(600):
        hwnd = user32.FindWindowW(None, 'Unreal Tournament')
        if hwnd:
            break
        time.sleep(0.1)
    if not hwnd:
        sys.exit('no Unreal Tournament window')
    t0 = time.time()
    for at, xy in clicks:
        time.sleep(max(0, t0 + at - time.time()))
        r = (ctypes.c_long * 4)()
        user32.GetClientRect(hwnd, r)
        if xy == 'altenter':   # T@altenter: Alt+Enter, a system key
            sc = user32.MapVirtualKeyW(0x0D, 0)
            user32.PostMessageW(hwnd, 0x104, 0x0D, 1 | sc << 16 | 1 << 29)
            time.sleep(0.05)
            user32.PostMessageW(hwnd, 0x105, 0x0D, 1 | sc << 16 | 1 << 29 | 0xC0000000)
            print('[key] %.1f s alt+enter' % (time.time() - t0), flush=True)
            continue
        if xy in KEYS:   # T@esc, T@right, ...: a key, as UWindow's menus take them
            vk = KEYS[xy]
            sc = user32.MapVirtualKeyW(vk, 0)
            user32.PostMessageW(hwnd, 0x100, vk, 1 | sc << 16)
            time.sleep(0.05)
            user32.PostMessageW(hwnd, 0x101, vk, 1 | sc << 16 | 0xC0000000)
            print('[key] %.1f s %s' % (time.time() - t0, xy), flush=True)
            continue
        x, y = map(int, xy.split(',')) if xy else (r[2] // 2, r[3] // 2)
        pos = y << 16 | x
        if xy:
            user32.PostMessageW(hwnd, WM_MOUSEMOVE, 0, pos)
            time.sleep(0.2)
        user32.PostMessageW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos)
        time.sleep(0.05)
        user32.PostMessageW(hwnd, WM_LBUTTONUP, 0, pos)
        print('[click] %.1f s %s' % (time.time() - t0, xy or 'centre'), flush=True)
    time.sleep(max(0, t0 + (clicks[-1][0] if clicks else 0) + stay - time.time()))
    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)


if __name__ == '__main__':
    main()
