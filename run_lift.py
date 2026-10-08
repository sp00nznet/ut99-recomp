#!/usr/bin/env python3
"""Unreal Tournament lift driver: work/<module>.functions.json -> src/recomp/gen/.

Drives pcrecomp's shared `tools/lift/generate.py` + `lift32`, the same shape as
Red Alert 2's run_lift.py, once per module. UT is a launcher plus a dozen
DLLs, and each one lifts into its own directory, src/recomp/gen/<module>/, so
one module can be lifted again without the others. The declarations and the
dispatch table cover every module and are rebuilt from work/<module>.entries.json
on every run. The modules are rebased first (tools/rebase.py), so every
sub_XXXXXXXX is unique across them.

Every vtable slot RTTI names is injected as an entry (work/<module>.seeds.json):
MSVC's vtordisp adjustor thunks are reachable only through a vtable, so no
disassembler pass names them, and an unresolved slot answers eax = 0.

    py -3 run_lift.py                      # every module
    py -3 run_lift.py Core.dll Engine.dll  # just these
"""
import argparse
import json
import os
import re
import shutil
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks another toolkit checkout, the same knob CMakeLists.txt has:
# the lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'tools')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))
sys.path.insert(0, os.path.join(_HERE, 'tools'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from eh32 import resume_points                             # noqa: E402
import pefile                                              # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402
from rebase import MODULES                                 # noqa: E402

WORK = os.path.join(_HERE, 'work')
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')


def lift_module(name, split):
    img = os.path.join(WORK, 'rebased', name)
    catalog = os.path.join(WORK, name + '.functions.json')
    if not os.path.exists(catalog):
        sys.exit('no catalog for %s -- run tools/catalog.py first (README, Step by step)' % name)
    info = analyze_pe(img)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] %s base=0x%08X code=0x%08X-0x%08X IAT=%d' % (name, info.image_base, cs, ce, len(iat)))

    byaddr = {f['address']: f for f in json.load(open(catalog))['functions'] if cs <= f['address'] < ce}
    seeds = os.path.join(WORK, name + '.seeds.json')
    want = set()
    if os.path.exists(seeds):
        want = {e['address'] for e in json.load(open(seeds)) if cs <= e['address'] < ce} - set(byaddr)
    for a in want:
        byaddr[a] = {'address': a, 'entry_kind': 'start'}

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(img, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]
    # The whole image as mapped, for the EH tables in .rdata: nearly every UT
    # function is a guard/unguard try block, and a catch has to resume inside
    # the lifted body (pcrecomp eh32).
    mapped = pefile.PE(img).get_memory_mapped_image()
    base = info.image_base
    read = lambda va, n: mapped[va - base:va - base + n] if 0 <= va - base < len(mapped) else None  # noqa: E731
    # precise carry: adc/sbb take their carry from the flag state that set it
    # (64-bit adds and subtracts; ra2's run_lift.py has the history).
    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)

    out = os.path.join(OUT, os.path.splitext(name)[0])
    shutil.rmtree(out, ignore_errors=True)             # a smaller lift must not leave stale chunks
    os.makedirs(out)
    starts = {a for a in byaddr if byaddr[a].get('entry_kind') != 'alias'}
    starts -= find_splits(md, code, cs, ce, starts)   # still dispatchable, just not a wall
    entries, chunk, idx, errors, dirty, ehs, unimpl = [], [], 0, 0, 0, 0, {}
    # Where a function resumes after its own catch blocks. Nothing branches
    # there (the catch funclet returns the address), so the catalog often
    # starts a function at it and the parent's body stops short: the resume
    # point is not a wall, and it is walked as a root of its parent's body.
    eh_at = {}
    for a in byaddr:
        r = resume_points(md, read, list(md.disasm(read(a, 64) or b'', a))[:12])
        if r:
            eh_at[a] = r
    starts -= set().union(*eh_at.values()) if eh_at else set()

    # A direct branch or call to an address nothing catalogued is a tail call
    # or shared code the catalog missed; it has to be dispatchable, so each
    # round's outside targets are lifted in the next, until none are new.
    todo, added = sorted(byaddr), 0
    while todo:
        outside = set()
        for addr in todo:
            fname = 'sub_%08X' % addr
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind, called=called)
            for r in eh_at.get(addr, ()):
                if cs <= r < ce:
                    e2, _ = true_extent(md, code, cs, r, min(r + EXTENT_REACH, ce), starts,
                                        reached=reached, behind=behind, called=called)
                    end = max(end, e2)
            outside |= {t for t in behind if t not in reached} | called
            dirty += not clean
            try:
                lo = min(reached) if reached else addr
                insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                                  if end > addr else ([], None))
                if leaders is not None:
                    leaders.add(addr)
                eh = eh_at.get(addr)
                ehs += bool(eh)
                body = (lift_function_linear(lifter, fname, insns, leaders, addr, eh_resume=eh) if insns
                        else 'void %s(void) { }\n' % fname)
            except Exception as e:                  # noqa: BLE001 -- counted, not hidden
                body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (fname, e, fname)
                errors += 1
            for m in re.findall(r'UNIMPLEMENTED: (\w+)', body):   # lifted as a no-op: says so
                unimpl[m] = unimpl.get(m, 0) + 1
            chunk.append((body, addr, fname))
            entries.append(addr)
            if len(chunk) >= split:
                write_chunk(out, idx, chunk)
                idx, chunk = idx + 1, []
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t}
        added += len(todo)
    if chunk:
        write_chunk(out, idx, chunk)
        idx += 1
    json.dump(sorted(entries), open(os.path.join(WORK, name + '.entries.json'), 'w'))
    print('    %s: lifted %d (+%d outside the catalog), errors %d, no terminator %d, %d catch, %d files'
          % (name, len(entries), added, errors, dirty, ehs, idx), flush=True)
    if unimpl:
        print('      unimplemented: ' + ' '.join('%s %d' % kv for kv in sorted(unimpl.items(), key=lambda kv: -kv[1])))
    return {'lifted': len(entries), 'added': added, 'errors': errors, 'no_terminator': dirty,
            'catch': ehs, 'files': idx, 'unimplemented': unimpl}


def write_tables():
    """recomp_funcs.h and recomp_dispatch.c over every module lifted so far."""
    entries = []
    for name in MODULES:
        p = os.path.join(WORK, name + '.entries.json')
        if os.path.exists(p):
            entries += json.load(open(p))
    entries.sort()
    put(os.path.join(OUT, 'recomp_funcs.h'),
        '/* AUTO-GENERATED by run_lift.py */\n#pragma once\n#include <stdint.h>\n\n'
        + ''.join('void sub_%08X(void);\n' % a for a in entries))
    put(os.path.join(OUT, 'recomp_dispatch.c'),
        '/* AUTO-GENERATED by run_lift.py */\n#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
        'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n'
        + ''.join('    { 0x%08Xu, sub_%08X },\n' % (a, a) for a in entries)
        + '};\nconst uint32_t recomp_dispatch_count = %d;\n' % len(entries))
    return len(entries)


def put(path, text):
    """Write only a change: every chunk includes recomp_funcs.h, so rewriting
    it unchanged would rebuild all ~120 files after lifting one module."""
    if not os.path.exists(path) or open(path, newline='').read() != text:
        with open(path, 'w', newline='\n') as f:
            f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('modules', nargs='*', help='modules to lift (default: all in tools/rebase.py)')
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    args = ap.parse_args()
    t0 = time.time()
    stats_path = os.path.join(WORK, 'lift_stats.json')
    stats = json.load(open(stats_path)) if os.path.exists(stats_path) else {}
    os.makedirs(OUT, exist_ok=True)
    for name in args.modules or MODULES:
        stats[name] = lift_module(name, args.split)
    json.dump(stats, open(stats_path, 'w'), indent=1)
    n = write_tables()
    print('=' * 60)
    print('  %d functions in the dispatch table, lift errors %d, unimplemented instructions %d, %.0fs'
          % (n, sum(s['errors'] for s in stats.values()),
             sum(sum(s.get('unimplemented', {}).values()) for s in stats.values()), time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
