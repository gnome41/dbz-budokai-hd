#!/usr/bin/env python3
"""Map import stub addresses -> NID -> sysPrxForUser name, to identify the event
functions the orchestration threads call (func_000F151C / func_000F14BC / func_000F20DC).

For each PRX import entry the NID array (nid_table_ptr) is parallel to the stub
array (stub_table_ptr).  Each stub entry is an OPD {code, toc}; the code field is
the lifted thunk address (e.g. 0xF151C)."""
import sys, struct
sys.path.insert(0, r"E:\Games\RecompLauncher\ps3recomp\tools")
from elf_parser import ELFFile, vaddr_to_offset
from nid_database import get_default_db

ELF = r"E:\Games\RecompLauncher\ps3recomp\game\EBOOT.elf"
TARGETS = {0xF151C, 0xF14BC, 0xF20DC}

elf = ELFFile(ELF)
raw = elf.raw_data
db = get_default_db()
phs = elf.program_headers

def u32(vaddr):
    o = vaddr_to_offset(phs, vaddr)
    if o is None or o + 4 > len(raw): return None
    return struct.unpack(">I", raw[o:o+4])[0]

print("module                       NID         stub/OPD    code      name")
for imp in elf.imports:
    total = imp.num_funcs
    sp = imp.stub_table_ptr
    if not sp or total == 0:
        continue
    for j in range(total):
        opd = u32(sp + j*4)
        nid = imp.nids[j] if j < len(imp.nids) else 0
        code = u32(opd) if opd else None
        nm = db.lookup_nid(nid)
        name = nm[1] if nm else f"nid_0x{nid:08X}"
        flag = "  <== TARGET" if (code in TARGETS or opd in TARGETS) else ""
        if flag or (code is not None and 0xF1000 <= code <= 0xF2200):
            print(f"{(imp.name_str or '?')[:26]:26}  0x{nid:08X}  0x{(opd or 0):08X}  0x{(code or 0):08X}  {name}{flag}")
