"""The launcher's own ini pair: UT's OpenGL renderer, a widescreen window, a
Hor+ field of view, the host's rendering settings (8x MSAA, 16x anisotropic,
a 200 fps cap) and scaled HUD fonts, without touching the stock
UnrealTournament.ini and User.ini (headless runs and the conformance harness
keep the software renderer). The launcher passes them as INI= and USERINI=.

The Mod menu's "Recomp Graphics" window (mod/RecompMenu) changes the same
settings from inside the game. A rerun keeps what is already in the pair and
adds only what is missing; --reset starts over from the stock files.

  py -3 tools/hd_ini.py [--reset] [WIDTHxHEIGHT]    default: 16:9, 80% of the screen
"""
import ctypes, math, os, sys

SYS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'System')


def set_keys(text, section, keys, keep=False):
    """Set keys under [section] (added at the end if it is missing), keeping
    everything else as it is; keep=True leaves keys already there alone."""
    out, inside, found, done = [], False, False, set()
    for line in text.splitlines():
        if line.startswith('['):
            if inside:
                out += ['%s=%s' % (k, v) for k, v in keys.items() if k not in done]
            inside = line.strip() == '[%s]' % section
            found |= inside
        elif inside and '=' in line and line.split('=', 1)[0] in keys:
            k = line.split('=', 1)[0]
            if not keep:
                line = '%s=%s' % (k, keys[k])
            done.add(k)
        out.append(line)
    if inside:
        out += ['%s=%s' % (k, v) for k, v in keys.items() if k not in done]
    if not found:
        out += ['', '[%s]' % section] + ['%s=%s' % kv for kv in keys.items()]
    return '\r\n'.join(out) + '\r\n'


def hor_plus(w, h):
    """UT's 90 degrees is horizontal at 4:3; keep the vertical view at other aspects."""
    return math.degrees(2 * math.atan(math.tan(math.radians(45)) * (w / h) / (4 / 3)))


def default_size():
    u = ctypes.windll.user32
    sw, sh = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
    w = int(min(sw * 0.8, sh * 0.8 * 16 / 9)) // 16 * 16
    return w, w * 9 // 16


def main(args):
    reset = '--reset' in args
    size = [a for a in args if a != '--reset']
    path = lambda n: os.path.join(SYS, n)
    read = lambda n: open(path(n), encoding='latin-1').read()
    fresh = reset or size or not os.path.exists(path('UTRecomp.ini'))
    keep = not fresh   # an existing pair keeps its values; only missing keys are added
    w, h = map(int, size[0].lower().split('x')) if size else default_size()
    fov = '%.6f' % hor_plus(w, h)

    ini = read('UnrealTournament.ini' if fresh else 'UTRecomp.ini')
    first = '[RecompMenu.RecompGraphicsClient]' not in ini   # these settings' first install
    ini = set_keys(ini, 'Engine.Engine', {'GameRenderDevice': 'OpenGLDrv.OpenGLRenderDevice',
                                          'WindowedRenderDevice': 'OpenGLDrv.OpenGLRenderDevice'}, keep)
    ini = set_keys(ini, 'WinDrv.WindowsClient', {'WindowedViewportX': w, 'WindowedViewportY': h,
                                                'WindowedColorBits': 32}, keep)
    ini = set_keys(ini, 'RecompMenu.RecompGraphicsClient', {'MSAA': 8, 'Aniso': 16, 'FPSCap': 200, 'VSync': -1}, True)
    if fresh and h >= 1080:   # menus at twice the size: still 540 lines of UWindow
        ini = set_keys(ini, 'UMenu.UMenuRootWindow', {'GUIScale': '2.000000'})

    user = read('User.ini' if fresh or not os.path.exists(path('UTRecompUser.ini')) else 'UTRecompUser.ini')
    user = set_keys(user, 'Engine.PlayerPawn', {'DesiredFOV': fov, 'DefaultFOV': fov}, keep)
    if first:   # after that, the Mod menu's choice stands
        user = set_keys(user, 'Botpack.ChallengeHUD', {'FontInfoClass': 'RecompMenu.RecompFontInfo'})
    for name, text in (('UTRecomp.ini', ini), ('UTRecompUser.ini', user)):
        open(path(name), 'w', encoding='latin-1', newline='').write(text)
    print('UTRecomp.ini: OpenGL, %s; UTRecompUser.ini: FOV %s, scaled HUD fonts'
          % ('%dx%d' % (w, h) if fresh else 'kept your settings, added what was missing', fov if fresh else 'kept'))


if __name__ == '__main__':
    assert round(hor_plus(640, 480), 3) == 90 and round(hor_plus(1920, 1080), 2) == 106.26
    assert set_keys('[A]\r\nx=1\r\n[B]\r\nx=1\r\n', 'B', {'x': 2, 'y': 3}) == '[A]\r\nx=1\r\n[B]\r\nx=2\r\ny=3\r\n'
    assert set_keys('[B]\r\nx=1\r\n', 'B', {'x': 2, 'y': 3}, True) == '[B]\r\nx=1\r\ny=3\r\n'
    assert set_keys('[A]\r\n', 'B', {'x': 2}) == '[A]\r\n\r\n[B]\r\nx=2\r\n'
    main(sys.argv[1:])
