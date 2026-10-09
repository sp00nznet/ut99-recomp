"""Build RecompMenu.u, the Mod menu's "Recomp Graphics" window and the scaled
HUD fonts (mod/RecompMenu), with the game's own UCC.exe run as a tool.

The source is ours; the compiler is the original UCC.exe from your install,
run natively (`ucc make` needs Editor.dll, which is not lifted). The .u is
built on your machine into game/System and never committed.

  py -3 tools/build_mod.py
"""
import os, shutil, subprocess, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
GAME = os.path.join(ROOT, 'game')
SYS = os.path.join(GAME, 'System')
PKG = 'RecompMenu'


def main():
    src = os.path.join(ROOT, 'mod', PKG, 'Classes')
    dst = os.path.join(GAME, PKG, 'Classes')
    shutil.rmtree(os.path.join(GAME, PKG), ignore_errors=True)
    shutil.copytree(src, dst)

    # ucc make compiles the EditPackages it finds missing: the stock list, then ours
    ini = open(os.path.join(SYS, 'UnrealTournament.ini'), encoding='latin-1').read().split('\n')
    last = max(i for i, l in enumerate(ini) if l.startswith('EditPackages='))
    ini.insert(last + 1, 'EditPackages=%s\r' % PKG)
    open(os.path.join(SYS, 'RecompMake.ini'), 'w', encoding='latin-1', newline='').write('\n'.join(ini))

    out = os.path.join(SYS, PKG + '.u')
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run([os.path.join(SYS, 'UCC.exe'), 'make', 'ini=RecompMake.ini', '-silent'],
                       cwd=SYS, capture_output=True, text=True, errors='replace')
    log = r.stdout + r.stderr
    open(os.path.join(ROOT, 'work', 'build_mod.log'), 'w').write(log)
    os.remove(os.path.join(SYS, 'RecompMake.ini'))
    if not os.path.exists(out):
        sys.exit('%s.u was not built; the compiler said:\n%s' % (PKG, log[-3000:]))
    shutil.copy(os.path.join(ROOT, 'mod', PKG + '.int'), SYS)
    print('%s.u built (%d bytes), %s.int installed' % (PKG, os.path.getsize(out), PKG))


if __name__ == '__main__':
    main()
