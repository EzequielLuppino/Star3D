#!/usr/bin/env python3
"""Decode a PICA200 command list dumped by S3D_GPU_DUMP (gx_init.c).

Input: the "gpucmd" log lines (with or without the log prefix), e.g.
  grep gpucmd Star3D.log | python tools/pica_decode.py
Register names come from libctru's 3ds/gpu/registers.h. Prints one line per
register write, in order (later writes win).
"""
import os
import re
import sys

REGS_REL = "libctru/include/3ds/gpu/registers.h"
# DEVKITPRO may be an msys path (/opt/devkitpro) that Windows Python cannot open
REGS_H = next((p for p in (os.path.join(os.environ.get("DEVKITPRO", ""), REGS_REL), os.path.join("C:/devkitPro", REGS_REL))
               if os.path.isfile(p)), REGS_REL)


def load_names():
    names = {}
    try:
        for m in re.finditer(r"#define\s+GPUREG_(\w+)\s+0x([0-9A-Fa-f]+)", open(REGS_H).read()):
            # Real names win over the GPUREG_0041-style placeholders
            if not re.fullmatch(r"[0-9A-F]{4}", m.group(1)) or int(m.group(2), 16) not in names:
                names[int(m.group(2), 16)] = m.group(1)
    except OSError:
        pass
    return names


def main():
    names = load_names()
    words = []
    for line in sys.stdin:
        m = re.search(r"([0-9A-F]{4}):((?:\s+[0-9A-F]{8})+)", line)
        if m:
            words += [int(w, 16) for w in m.group(2).split()]
        m = re.search(r"end \((\d+) words\)", line)
        if m:
            words = words[: int(m.group(1))]
    i = 0
    while i + 1 < len(words):
        param, header = words[i], words[i + 1]
        reg = header & 0xFFFF
        mask = (header >> 16) & 0xF
        extra = (header >> 20) & 0x7FF
        consecutive = header >> 31
        params = [param] + words[i + 2 : i + 2 + extra]
        for k, p in enumerate(params):
            r = reg + k if consecutive else reg
            name = names.get(r, "")
            print(f"{i:04X}  {r:03X} {name:<28} mask {mask:X}  {p:08X}")
        i += 2 + extra
        if i & 1:
            i += 1  # Commands are 8-byte aligned


if __name__ == "__main__":
    main()
