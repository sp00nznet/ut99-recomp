"""The launcher's own ini pair: UT's OpenGL renderer, a widescreen window and
a Hor+ field of view, without touching the stock UnrealTournament.ini and
User.ini (headless runs and the conformance harness keep the software
renderer). The launcher passes them as INI= and USERINI=.

  py -3 tools/hd_ini.py [WIDTHxHEIGHT]    default: 16:9, 80% of the screen
"""
import ctypes, math, os, sys

SYS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'game', 'System')


def set_keys(text, section, keys):
    """Set keys under [section], keeping everything else as it is."""
    out, inside, done = [], False, set()
    for line in text.splitlines():
        if line.startswith('['):
            if inside:
                out += ['%s=%s' % (k, v) for k, v in keys.items() if k not in done]
            inside = line.strip() == '[%s]' % section
        elif inside and '=' in line and line.split('=', 1)[0] in keys:
            k = line.split('=', 1)[0]
            line = '%s=%s' % (k, keys[k])
            done.add(k)
        out.append(line)
    if inside:
        out += ['%s=%s' % (k, v) for k, v in keys.items() if k not in done]
    return '\r\n'.join(out) + '\r\n'


def hor_plus(w, h):
    """UT's 90 degrees is horizontal at 4:3; keep the vertical view at other aspects."""
    return math.degrees(2 * math.atan(math.tan(math.radians(45)) * (w / h) / (4 / 3)))


def default_size():
    u = ctypes.windll.user32
    sw, sh = u.GetSystemMetrics(0), u.GetSystemMetrics(1)
    w = int(min(sw * 0.8, sh * 0.8 * 16 / 9)) // 16 * 16
    return w, w * 9 // 16


def main():
    w, h = map(int, sys.argv[1].lower().split('x')) if len(sys.argv) > 1 else default_size()
    fov = '%.6f' % hor_plus(w, h)
    read = lambda n: open(os.path.join(SYS, n), encoding='latin-1').read()
    ini = read('UnrealTournament.ini')
    ini = set_keys(ini, 'Engine.Engine', {'GameRenderDevice': 'OpenGLDrv.OpenGLRenderDevice',
                                          'WindowedRenderDevice': 'OpenGLDrv.OpenGLRenderDevice'})
    ini = set_keys(ini, 'WinDrv.WindowsClient', {'WindowedViewportX': w, 'WindowedViewportY': h,
                                                'WindowedColorBits': 32})
    user = set_keys(read('User.ini'), 'Engine.PlayerPawn', {'DesiredFOV': fov, 'DefaultFOV': fov})
    for name, text in (('UTRecomp.ini', ini), ('UTRecompUser.ini', user)):
        open(os.path.join(SYS, name), 'w', encoding='latin-1', newline='').write(text)
    print('UTRecomp.ini: OpenGL, %dx%d; UTRecompUser.ini: FOV %s' % (w, h, fov))


if __name__ == '__main__':
    assert round(hor_plus(640, 480), 3) == 90 and round(hor_plus(1920, 1080), 2) == 106.26
    assert set_keys('[A]\r\nx=1\r\n[B]\r\nx=1\r\n', 'B', {'x': 2, 'y': 3}) == '[A]\r\nx=1\r\n[B]\r\nx=2\r\ny=3\r\n'
    main()
