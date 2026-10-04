#!/usr/bin/env python3
"""RTTI classes and the function catalog, for every module in tools/rebase.py.

Runs pcrecomp's cpp/rtti.py then disasm/disasm32.py on each rebased image, all
modules in parallel (one process each: disasm32 is single-threaded Python).
Exports seed disasm32 by themselves; RTTI vtable slots go in as seeds, since a
method only ever called virtually is named by no `call` anywhere.

    py -3 tools/catalog.py                 # every module, skipping finished ones
    py -3 tools/catalog.py Core.dll --force
Output: work/<module>.rtti.json, work/<module>.seeds.json, work/<module>.functions.json
"""
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from rebase import MODULES  # noqa: E402

TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(ROOT, '..', 'tools')), 'tools')
WORK = os.path.join(ROOT, 'work')


def one(name, force):
    img = os.path.join(WORK, 'rebased', name)
    rtti, seeds, cat = (os.path.join(WORK, name + s) for s in ('.rtti.json', '.seeds.json', '.functions.json'))
    if os.path.exists(cat) and not force:
        return name, 'skipped (done)'
    log = open(os.path.join(WORK, name + '.catalog.log'), 'w')
    run = lambda *a: subprocess.run([sys.executable, *a], stdout=log, stderr=subprocess.STDOUT).returncode
    if run(os.path.join(TOOLS, 'cpp', 'rtti.py'), img, '-o', rtti, '--seeds', seeds):
        return name, 'rtti.py failed, see work/%s.catalog.log' % name
    extra = ['--seed-functions', seeds] if os.path.exists(seeds) else []
    if run(os.path.join(TOOLS, 'disasm', 'disasm32.py'), img, '-o', cat, *extra):
        return name, 'disasm32.py failed, see work/%s.catalog.log' % name
    tail = [l for l in open(log.name) if 'Functions:' in l or 'coverage' in l]
    return name, ' '.join(l.strip() for l in tail)


def main():
    force = '--force' in sys.argv
    names = [a for a in sys.argv[1:] if not a.startswith('--')] or list(MODULES)
    with ThreadPoolExecutor(max(1, os.cpu_count() // 2)) as ex:
        for name, msg in ex.map(lambda n: one(n, force), names):
            print('%-22s %s' % (name, msg), flush=True)


if __name__ == '__main__':
    main()
