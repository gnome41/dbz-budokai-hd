#!/usr/bin/env python3
"""Extract the embedded SPURS-kernel SPU ELF from EBOOT.elf and build its LS image.

The kernel SPU ELF lives at guest 0x10BD00; EBOOT PT_LOAD seg0 has vaddr 0x10000
at file offset 0, so the file offset is guest_addr - 0x10000.

Writes the 256 KB LS image to _spurs_kernel_ls.bin and prints the SPU ELF
program headers + entry point.
"""
import struct, sys

EBOOT = r"E:\Games\RecompLauncher\ps3recomp\game\EBOOT.elf"
GUEST_ADDR = 0x10BD00
FILE_OFF = GUEST_ADDR - 0x10000
MAX_SIZE = 0x36800  # gap to next embedded ELF

data = open(EBOOT, "rb").read()
elf = data[FILE_OFF:FILE_OFF + MAX_SIZE]
assert elf[:4] == b"\x7fELF", f"no ELF magic at file offset 0x{FILE_OFF:X}"

ei_class = elf[4]   # 1 = 32-bit
ei_data = elf[5]    # 2 = big-endian
assert ei_class == 1 and ei_data == 2, f"unexpected SPU ELF class/data: {ei_class}/{ei_data}"

e_entry, e_phoff = struct.unpack(">II", elf[0x18:0x20])
e_phentsize, e_phnum = struct.unpack(">HH", elf[0x2A:0x2E])
print(f"SPU ELF: entry=0x{e_entry:X} phoff=0x{e_phoff:X} phnum={e_phnum}")

ls = bytearray(256 * 1024)
for i in range(e_phnum):
    ph = elf[e_phoff + i * e_phentsize: e_phoff + (i + 1) * e_phentsize]
    p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack(">8I", ph[:32])
    print(f"  PH{i}: type={p_type} off=0x{p_offset:X} vaddr=0x{p_vaddr:X} filesz=0x{p_filesz:X} memsz=0x{p_memsz:X}")
    if p_type == 1:  # PT_LOAD
        ls[p_vaddr:p_vaddr + p_filesz] = elf[p_offset:p_offset + p_filesz]

open("_spurs_kernel_ls.bin", "wb").write(ls)
print("wrote _spurs_kernel_ls.bin (256 KB LS image)")
