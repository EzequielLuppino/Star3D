"""Find absolute relocations that 3DSX loaders will reject.

3dsxtool keeps relocated words as image-relative addresses and newer loaders
(Azahar, recent hbmenu) use their top 4 bits as a relocation sub-type. A word
whose linked value lies outside the program image (e.g. &table[-1]) turns into
an invalid sub-type and the whole .3dsx fails to load.

    python -I tools/check_relocs.py build/us_3ds/star3d.elf
"""
import bisect
import os
import shutil
import struct
import subprocess
import sys


def find_tools():
    """Prefix for the devkitARM binutils: $DEVKITARM, the default Windows
    install, or whatever is on PATH."""
    for bindir in (os.path.join(os.environ.get("DEVKITARM", ""), "bin"), "C:/devkitPro/devkitARM/bin"):
        prefix = os.path.join(bindir, "arm-none-eabi-")
        if shutil.which(prefix + "readelf"):
            return prefix
    return "arm-none-eabi-"


TOOLS = find_tools()


def run(*args):
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def main(elf_path):
    data = open(elf_path, "rb").read()

    # Section table: name -> (addr, file offset, size, type)
    sections = []
    for line in run(TOOLS + "readelf", "-SW", elf_path).splitlines():
        parts = line.replace("[ ", "[").split()
        if len(parts) > 6 and parts[0].startswith("[") and parts[0] != "[Nr]":
            name, stype, addr, off, size = parts[1], parts[2], int(parts[3], 16), int(parts[4], 16), int(parts[5], 16)
            sections.append((name, stype, addr, off, size))

    alloc = [s for s in sections if s[2] != 0]
    image_lo = min(s[2] for s in alloc)
    image_hi = max(s[2] + s[4] for s in alloc)

    def word_at(vaddr):
        for name, stype, addr, off, size in alloc:
            if stype != "NOBITS" and addr <= vaddr < addr + size:
                return struct.unpack_from("<I", data, off + vaddr - addr)[0]
        return None

    # Symbols for reporting where each bad word lives
    syms = []
    for line in run(TOOLS + "nm", "-n", "-C", elf_path).splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3:
            syms.append((int(parts[0], 16), parts[2]))
    sym_addrs = [a for a, _ in syms]

    def owner(vaddr):
        i = bisect.bisect_right(sym_addrs, vaddr) - 1
        return f"{syms[i][1]}+0x{vaddr - syms[i][0]:x}" if i >= 0 else "?"

    bad = 0
    in_alloc_section = False
    for line in run(TOOLS + "readelf", "-rW", elf_path).splitlines():
        if line.startswith("Relocation section"):
            # Only relocations applied to loaded sections, not .rel.debug_*
            in_alloc_section = "debug" not in line
            continue
        parts = line.split()
        if in_alloc_section and len(parts) >= 3 and parts[2] in ("R_ARM_ABS32", "R_ARM_TARGET1"):
            where = int(parts[0], 16)
            value = word_at(where)
            # 0 is an unresolved weak symbol (e.g. userAppInit), which is fine
            if value is None or value == 0 or image_lo <= value <= image_hi:
                continue
            target = parts[4] if len(parts) > 4 else "?"
            print(f"0x{where:08x} in {owner(where)}: value 0x{value:08x} (symbol {target})")
            bad += 1

    print(f"image 0x{image_lo:08x}-0x{image_hi:08x}: {bad} out-of-range absolute relocations")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
