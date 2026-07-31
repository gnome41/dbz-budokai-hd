#!/usr/bin/env python3
"""Parse the PS3 EBOOT import table; map stub-table slots -> (module, NID)."""
import struct, hashlib

EBOOT = r"..\game\EBOOT.elf"
d = open(EBOOT, "rb").read()

def g2f(v): return v - 0x10000
def u16(v): return struct.unpack(">H", d[g2f(v):g2f(v)+2])[0]
def u32(v): return struct.unpack(">I", d[g2f(v):g2f(v)+4])[0]
def cstr(v):
    o = g2f(v); e = d.index(b"\0", o); return d[o:e].decode("latin1")

MI = 0x1547a8
name = d[g2f(MI)+4:g2f(MI)+4+28].split(b"\0")[0].decode("latin1")
gp, exp_s, exp_e, imp_s, imp_e = struct.unpack(">5I", d[g2f(MI)+32:g2f(MI)+32+20])
print(f"module '{name}' gp={gp:#x} imports {imp_s:#x}..{imp_e:#x}")

CANDIDATES = [
 "cellGcmSetVBlankHandler","cellGcmSetFlipHandler","cellGcmGetFlipStatus",
 "cellGcmResetFlipStatus","cellGcmSetFlipMode","cellGcmGetLastFlipTime",
 "cellGcmGetVBlankCount","cellGcmSetSecondVFrequency","cellGcmGetCurrentField",
 "cellGcmSetWaitFlip","cellGcmSetFlip","cellGcmSetPrepareFlip","cellGcmGetTiledPitchSize",
 "cellGcmGetControlRegister","cellGcmGetLabelAddress","cellGcmGetReportDataAddressLocation",
 "cellGcmInit","cellGcmGetConfiguration","cellGcmBindTile","cellGcmUnbindTile",
 "cellGcmSetDisplayBuffer","cellGcmSetTileInfo","_cellGcmSetFlipCommand",
 "cellGcmGetCurrentBuffer","cellGcmSetDefaultCommandBuffer","cellGcmGetFlipStatus",
 "cellGcmSetVBlankFrequency","cellGcmSetFlipHandler","cellGcmSetSecondVHandler",
 "cellGcmSetUserHandler","cellGcmSetGraphicsHandler","cellGcmSetQueueHandler",
 "sys_ppu_thread_create","sys_ppu_thread_exit","sys_ppu_thread_join","sys_ppu_thread_yield",
 "sys_timer_usleep","sys_timer_sleep","sys_lwmutex_create","sys_lwmutex_lock",
 "sys_lwmutex_unlock","sys_lwcond_create","sys_lwcond_wait","sys_lwcond_signal",
 "sys_event_queue_create","sys_event_queue_receive","sys_event_port_create",
 "sys_event_port_connect_local","sys_event_port_send","sys_semaphore_create",
 "sys_semaphore_wait","sys_semaphore_post","sys_ppu_thread_get_id","sys_ppu_thread_once",
 "_sys_ppu_thread_create","cellGcmAddressToOffset","cellGcmMapMainMemory",
]
nidmap = {}
for nm in CANDIDATES:
    nid = struct.unpack(">I", hashlib.sha1(nm.encode()).digest()[:4])[0]
    nidmap[nid] = nm

TARGETS = {0x160738,0x16073C,0x160740,0x160744,0x160748,0x16074C,0x160B1C,0x160B00,0x160B20}
p = imp_s
while p < imp_e:
    structsize = d[g2f(p)]
    if structsize < 0x24:
        p += 4; continue
    version, attr, num_func, num_var, num_tls = struct.unpack(">HHHHH", d[g2f(p)+2:g2f(p)+12])
    libname_a = u32(p+0x10); fnid_a = u32(p+0x14); fstub_a = u32(p+0x18)
    try: libname = cstr(libname_a)
    except Exception: libname = f"?{libname_a:#x}"
    hit = any((fstub_a <= t < fstub_a+num_func*4) for t in TARGETS)
    print(f"[{libname}] num_func={num_func} nid@{fnid_a:#x} stub@{fstub_a:#x}{'  <== covers a TARGET' if hit else ''}")
    for i in range(num_func):
        stub_slot = fstub_a + i*4
        nid = u32(fnid_a + i*4)
        nm = nidmap.get(nid, "")
        if stub_slot in TARGETS:
            print(f"    [{i}] stub={stub_slot:#x} nid={nid:#010x} {nm or '(unknown NID)'}   <<==== TARGET")
    p += structsize
