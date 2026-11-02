#!/usr/bin/env python3
import struct, sys, os

# Luma3DS ExceptionDumpHeader (k11_extension/include/fatalExceptionHandlers.h)
def decode(path):
    d = open(path, 'rb').read()
    magic0, magic1 = struct.unpack_from('<II', d, 0)
    vminor, vmajor, proc, core, typ = struct.unpack_from('<HHHHH', d, 8)
    total, regs_sz, code_sz, stack_sz, add_sz = struct.unpack_from('<IIIII', d, 20)
    off = 40
    regs = struct.unpack_from('<23I', d, off); off += regs_sz
    code = d[off:off+code_sz]; off += code_sz
    stack = d[off:off+stack_sz]; off += stack_sz
    add = d[off:off+add_sz]
    names = ['r0','r1','r2','r3','r4','r5','r6','r7','r8','r9','r10','r11','r12','sp','lr','pc-fault-lr','cpsr','dfsr','ifsr','far','fpexc','fpinst','fpinst2']
    types = {0:'FIQ',1:'undefined instr',2:'prefetch abort',3:'data abort'}
    print(f"== {os.path.basename(path)}: {types.get(typ,typ)} core {core} ARM11, version {vmajor}.{vminor}")
    for n, v in zip(names, regs):
        print(f"   {n:10s} {v:08X}")
    cpsr = regs[16]
    lr15 = regs[15]
    thumb = (cpsr & 0x20) != 0
    pc = lr15 - (2 if (thumb and typ == 1) else 4) if typ < 3 else lr15 - 8
    print(f"   -> fault pc   {pc:08X}  (thumb={thumb}, cpsr mode 0x{cpsr&0x1F:x})")
    print(f"   -> stack words (sp={regs[13]:08X}):")
    sp = regs[13]
    for i in range(0, stack_sz, 16):
        w = struct.unpack_from('<4I', stack, i)
        print(f"      {sp+i:08X}: {' '.join(f'{x:08X}' for x in w)}")
    if add_sz:
        name = add[:8].rstrip(b'\0').decode('latin1')
        tid = struct.unpack_from('<Q', add, 8)[0]
        print(f"   process: {name} titleId {tid:016X}")
    return regs, stack, pc

for f in sys.argv[1:]:
    decode(f)
    print()
