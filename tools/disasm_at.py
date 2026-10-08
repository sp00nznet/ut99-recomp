#!/usr/bin/env python3
"""Disassemble a module's code at a VA, following a leading jmp thunk.

    py -3 tools/disasm_at.py SoftDrv.dll 0x10D0109B [count]
"""
import os
import sys

import capstone
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
pe = pefile.PE(os.path.join(ROOT, 'work', 'rebased', sys.argv[1]))
va, n = int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 80
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
code = lambda a: pe.get_data(a - base, 0x4000)
first = next(md.disasm(code(va), va))
if first.mnemonic == 'jmp' and first.op_str.startswith('0x'):
    print('; thunk -> ' + first.op_str)
    va = int(first.op_str, 16)
for i, ins in enumerate(md.disasm(code(va), va)):
    if i >= n:
        break
    print('%08X  %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
