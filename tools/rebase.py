#!/usr/bin/env python3
"""Map every module the game runs to a fixed, non-overlapping VA.

Epic linked each UT99 DLL at its own base, but not all of them fit together:
Engine.dll (0x10300000, 2.8 MB) runs over Fire.dll's 0x10500000, and the
render and net drivers share 0x10000000 and 0x10700000. The real loader
relocates the losers at run time; a static recompilation cannot, because the
lifted C has every absolute address baked in. So the losers are relocated
here, once, before disassembly, and the host maps each copy at exactly this
base (src/runtime/modules.c holds the same table).

The Steam build's UnrealTournament.exe is wrapped in SteamStub 2.0 (its
`.text` is encrypted); pcrecomp's drm/steamstub.py unwraps it first, in memory.

    py -3 tools/rebase.py            # game/System -> work/rebased/
"""
import os
import sys
import pefile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(os.environ.get('PCRECOMP', os.path.join(ROOT, '..', 'tools')),
                                'tools', 'drm'))

# file in game/System -> base. Keep in step with src/runtime/modules.c.
MODULES = {
    'UnrealTournament.exe': 0x10900000,   # linked base: unchanged
    'UCC.exe':              0x00400000,
    'Core.dll':             0x10100000,
    'Engine.dll':           0x10300000,
    'Galaxy.dll':           0x10600000,
    'IpDrv.dll':            0x10700000,
    'Fire.dll':             0x10800000,   # 0x10500000 is inside Engine
    'UWeb.dll':             0x10A00000,   # 0x10700000 is IpDrv's
    'Render.dll':           0x10B00000,
    'SoftDrv.dll':          0x10D00000,
    'Window.dll':           0x11000000,
    'WinDrv.dll':           0x11100000,
    'OpenGlDrv.dll':        0x11200000,   # 0x10000000 is D3DDrv's
    'D3DDrv.dll':           0x11300000,
}


def rebase(path, base):
    """The file's bytes with every HIGHLOW fixup applied for `base`.

    By hand rather than pefile's relocate_image(), which also rewrites the
    import thunks it parsed (gunman's tools/rebase.py has the story).
    """
    raw = open(path, 'rb').read()
    if pefile.PE(data=raw, fast_load=True).sections[-1].Name.rstrip(b'\0') == b'.bind':
        from steamstub import unwrap        # SteamStub: decrypt .text, restore the OEP
        raw = unwrap(raw)[0]
    pe = pefile.PE(data=raw)
    data = bytearray(pe.__data__)
    delta = (base - pe.OPTIONAL_HEADER.ImageBase) & 0xFFFFFFFF
    if delta:
        for block in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', ()):
            for e in block.entries:
                if e.type == 3:                      # IMAGE_REL_BASED_HIGHLOW
                    off = pe.get_offset_from_rva(e.rva)
                    v = int.from_bytes(data[off:off + 4], 'little')
                    data[off:off + 4] = ((v + delta) & 0xFFFFFFFF).to_bytes(4, 'little')
                elif e.type != 0:                    # ABSOLUTE is padding
                    raise ValueError('%s: relocation type %d' % (path, e.type))
        off = pe.OPTIONAL_HEADER.get_file_offset() + 28   # ImageBase
        data[off:off + 4] = base.to_bytes(4, 'little')
    return bytes(data), pe.OPTIONAL_HEADER.SizeOfImage


def main():
    out = os.path.join(ROOT, 'work', 'rebased')
    os.makedirs(out, exist_ok=True)
    spans = []
    for name, base in MODULES.items():
        data, size = rebase(os.path.join(ROOT, 'game', 'System', name), base)
        spans.append((base, base + size, name))
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)
        print(f'{name:22} {base:#010x}-{base + size:#010x}')
    spans.sort()
    for (a0, a1, n), (b0, _, m) in zip(spans, spans[1:]):
        if a1 > b0:
            sys.exit(f'{n} overlaps {m}')


if __name__ == '__main__':
    sys.exit(main())
