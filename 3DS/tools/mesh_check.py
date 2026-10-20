"""Checks the polygon-reduced banks in a RomFS against the disc image.

    python -I tools/mesh_check.py <game.iso> <romfs_dir>

Run by "make mesh-check" (Docker: build.bat mesh-check / ./build.sh
mesh-check). For every MODELS.bin/.tab and modN.zlb.bin/.tab pair:
- the .tab keeps its shape (same length, empty slots, flags, terminator; the
  MODELS.tab size word matches the new .bin);
- every asset is still there with the same decompressed size;
- inside each asset only display lists and their size fields changed, every
  new size is a multiple of 32 and not larger than before.
Prints the triangle totals and exits with 1 on any mismatch.
"""
import importlib.util
import os
import struct
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS_DIR, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


extract_disc = load_module("extract_disc")
mesh_reduce = load_module("mesh_reduce")


def list_table(raw, kind):
    """[(offset of the size field, list offset, size)] of a model or map block."""
    if kind == "model":
        dl_off, count = struct.unpack_from(">I", raw, 0xD0)[0], raw[0xF5] + raw[0xF6]
    else:
        dl_off, count = struct.unpack_from(">I", raw, 0x68)[0], raw[0xA1]
    out = []
    for i in range(count):
        e = dl_off + i * 0x1C
        off, size = struct.unpack_from(">IH", raw, e)
        out.append((e + 4, off, size))
    return out


def check_asset(old, new, kind, where, errors):
    if len(old) != len(new):
        errors.append(f"{where}: size {len(old)} -> {len(new)}")
        return
    allowed = bytearray(len(old))  # bytes that may differ
    for (sf, off, size), (sf2, off2, size2) in zip(list_table(old, kind), list_table(new, kind)):
        if (sf, off) != (sf2, off2):
            errors.append(f"{where}: list table moved")
            return
        if size2 > size or size2 % 32:
            errors.append(f"{where}: list at 0x{off:x} size {size} -> {size2}")
        allowed[sf:sf + 2] = b"\1\1"
        allowed[off:off + size] = b"\1" * size
    for i in range(len(old)):
        if old[i] != new[i] and not allowed[i]:
            errors.append(f"{where}: byte 0x{i:x} outside the display lists changed")
            return


def check_tab(old, new, bin_size, kind, where, errors):
    a = struct.unpack(f">{len(old) // 4}I", old[:len(old) // 4 * 4])
    b = struct.unpack(f">{len(new) // 4}I", new[:len(new) // 4 * 4])
    if len(a) != len(b):
        errors.append(f"{where}: tab length changed")
        return
    for x, y in zip(a, b):
        if (x == 0) != (y == 0) or (x == 0xFFFFFFFF) != (y == 0xFFFFFFFF) or (x & 0xF0000000) != (y & 0xF0000000):
            errors.append(f"{where}: tab word 0x{x:08x} -> 0x{y:08x}")
            return
    if kind == "model" and 0xFFFFFFFF in a:
        i = a.index(0xFFFFFFFF)
        if i and b[i - 1] != bin_size:
            errors.append(f"{where}: size word 0x{b[i - 1]:x} != .bin size 0x{bin_size:x}")


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    iso_path, romfs = sys.argv[1], sys.argv[2]
    originals = {}
    with open(iso_path, "rb") as iso:
        boot = extract_disc.read_at(iso, 0, 0x440)
        fst_offset, fst_size = struct.unpack_from(">II", boot, 0x424)
        fst = extract_disc.read_at(iso, fst_offset, fst_size)
        for path, offset, length in extract_disc.parse_fst(fst):
            if mesh_reduce.is_model_bank(path) or mesh_reduce.is_map_bank(path):
                originals[path] = extract_disc.read_at(iso, offset, length)
    errors = []
    banks = changed = 0
    for path in sorted(originals):
        name = os.path.basename(path)
        is_model = name.upper() == "MODELS.BIN"
        m = mesh_reduce._MAP_RE.match(name)
        if not is_model and not (m and m.group(2).lower() == "zlb.bin"):
            continue
        tab_name = name[:-4] + ".tab" if is_model else f"mod{m.group(1)}.tab"
        tab = next((p for p in originals if p.lower() == os.path.join(os.path.dirname(path), tab_name).lower()), None)
        if tab is None:
            continue
        kind = "model" if is_model else "map"
        new_bin = open(os.path.join(romfs, "files", *path.lstrip("/").split("/")), "rb").read()
        new_tab = open(os.path.join(romfs, "files", *tab.lstrip("/").split("/")), "rb").read()
        banks += 1
        check_tab(originals[tab], new_tab, len(new_bin), kind, tab, errors)
        parse = mesh_reduce.parse_model_bank if is_model else mesh_reduce.parse_map_bank
        try:
            _, old_e = parse(originals[path], originals[tab])
            _, new_e = parse(new_bin, new_tab)
        except Exception as e:
            errors.append(f"{path}: does not parse ({e})")
            continue
        if len(old_e) != len(new_e):
            errors.append(f"{path}: {len(old_e)} -> {len(new_e)} assets")
            continue
        for o, n in zip(sorted(old_e), sorted(new_e)):
            old_raw = old_e[o][1] if is_model else old_e[o][0]
            new_raw = new_e[n][1] if is_model else new_e[n][0]
            if old_raw != new_raw:
                changed += 1
                check_asset(old_raw, new_raw, kind, f"{path}@0x{o:x}", errors)
    for e in errors[:50]:
        print("mesh_check: " + e)
    print(f"mesh_check: {banks} banks, {changed} assets changed, {len(errors)} problems")
    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
