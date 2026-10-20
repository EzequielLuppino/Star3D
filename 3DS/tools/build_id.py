#!/usr/bin/env python3
"""Print the build id that Star 3D logs at startup ("build id XXXXXXXX") for an
ELF, to tell which build a hardware log came from.
Same hash as port/savestate_snap.c build_id(): FNV-1a over the 32-bit words
from __start__ + 0x20 to __exidx_start (skipping the 3dsx crt0 parameter
block, which the Homebrew Launcher fills in: bug 13).
Also prints the parts logged as "image hash: text .., gap .., rodata .." and,
if the build dir has them, the "shbin hash" of each shader blob (bugs 12/13).
Usage: python tools/build_id.py out/star3d.elf [build/us_3ds]
"""
import os
import struct
import subprocess
import sys

NM = "C:/devkitPro/devkitARM/bin/arm-none-eabi-nm"


def main():
    elf = sys.argv[1]
    syms = {}
    for line in subprocess.run([NM, elf], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    start, end = syms["__start__"], syms["__exidx_start"]
    data = open(elf, "rb").read()
    # Gather [start, end) from the loadable segments (zero fill between them)
    e_phoff, = struct.unpack_from("<I", data, 0x1C)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 0x2A)
    image = bytearray(end - start)
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from("<8I", data, e_phoff + i * e_phentsize)
        lo, hi = max(start, p_vaddr), min(end, p_vaddr + p_filesz)
        if p_type == 1 and lo < hi:
            image[lo - start: hi - start] = data[p_offset + lo - p_vaddr: p_offset + hi - p_vaddr]
    image = image[0x20:]
    h = fnv(image)
    print(f"{h or 1:08X}")
    if "__text_end" in syms:
        te, rs = syms["__text_end"] - start - 0x20, syms["__rodata_start"] - start - 0x20
        print(f"image hash: text {fnv(image[:te]):08X} ({te} B), gap {fnv(image[te:rs]):08X} ({rs - te} B), "
              f"rodata {fnv(image[rs:]):08X} ({len(image) - rs} B)")
    bdir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(elf), "..", "3DS", "build-docker", "us_3ds")
    parts = []
    for name in ("default", "bench", "hwtest"):
        path = os.path.join(bdir, name + ".shbin")
        if os.path.exists(path):
            blob = open(path, "rb").read()
            parts.append(f"{name} {fnv(blob):08X} ({len(blob)} B)")
    if parts:
        print("shbin hash: " + ", ".join(parts))


def fnv(buf):
    h = 2166136261
    for (w,) in struct.iter_unpack("<I", bytes(buf[: len(buf) & ~3])):
        h = ((h ^ w) * 16777619) & 0xFFFFFFFF
    return h


if __name__ == "__main__":
    main()
