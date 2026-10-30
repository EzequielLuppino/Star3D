"""Build-time texture conversion for the TEX0/TEX1/TEXPRE banks
(GRAPHICS_PLAN.md section 2). Used by extract_disc.py.

Bank layout (big-endian, see pi_dolphin.c tex0GetFrame/tex1GetFrame):
    .tab  u32 words, 0xFFFFFFFF-terminated. Bits 31-30: map slot flags,
          bits 29-24: frame count, bits 23-0: entry offset / 2.
    .bin  entries, 32-byte aligned. One frame: a "ZLB" block (16-byte header +
          deflate data) or a "DIR" block (32-byte header + raw data). Several
          frames (animated textures): a table of count+1 u32 offsets relative
          to the entry (the last is the end), then the frame blocks.
Each frame decompresses to one game texture (0x60-byte header + GX data).

Every texture is converted by tools/native/texconv.c (loaded with ctypes)
into PICA-ready data, re-compressed as ZLB (the game keeps whole banks in
memory, so the bank files must stay small until on-demand loading lands), and
the tab offsets are rewritten. Results are cached by content hash in
<build>/cache/tex, so rebuilds only convert changed textures.
"""
import ctypes
import hashlib
import multiprocessing
import os
import struct
import sys
import zlib

TEXTURE_BANKS = ("TEX0", "TEX1", "TEXPRE")
PICA_FORMAT_NAMES = {0: "RGBA8", 2: "RGBA5551", 3: "RGB565", 4: "RGBA4", 5: "LA8", 7: "L8", 8: "A8",
                     9: "LA4", 0xC: "ETC1", 0xD: "ETC1A4"}
GX_FORMAT_NAMES = {0: "I4", 1: "I8", 2: "IA4", 3: "IA8", 4: "RGB565", 5: "RGB5A3", 6: "RGBA8", 14: "CMPR"}
QUALITY_LEVELS = {"high": 0, "balanced": 1, "small": 2}
STATS_FORMAT = "<18I16d2I2Q"
PT_MAGIC = 0x31543350


class BankError(Exception):
    pass


def is_texture_bank(path):
    """True for .../TEX0.bin, TEX1.bin, TEXPRE.bin (any case) and their .tab."""
    name = os.path.basename(path)
    stem, ext = os.path.splitext(name)
    return stem.upper() in TEXTURE_BANKS and ext.lower() in (".bin", ".tab")


def parse_block(data, off):
    """Returns (texture bytes, header word 4) of the ZLB/DIR block at off."""
    tag = data[off:off + 3]
    if tag == b"ZLB":
        unk4, dsize, csize = struct.unpack_from(">III", data, off + 4)
        tex = zlib.decompress(data[off + 16:off + 16 + csize])
        if len(tex) != dsize:
            raise BankError(f"ZLB block at 0x{off:x}: size {len(tex)} != {dsize}")
        return tex, unk4
    if tag == b"DIR":
        unk4, dsize = struct.unpack_from(">II", data, off + 4)
        return data[off + 0x20:off + 0x20 + dsize], unk4
    raise BankError(f"unknown block tag {tag!r} at 0x{off:x}")


def parse_bank(bin_data, tab_data):
    """Returns (tab words, {entry offset: [texture bytes per frame]}, block unk4)."""
    words = []
    for i in range(len(tab_data) // 4):
        w = struct.unpack_from(">I", tab_data, i * 4)[0]
        words.append(w)
        if w == 0xFFFFFFFF:
            break
    entries = {}
    unk4 = 1
    for w in words:
        if w == 0xFFFFFFFF:
            break
        off = (w & 0xFFFFFF) * 2
        count = (w >> 24) & 0x3F
        if off >= len(bin_data) or off in entries:
            continue
        if count > 1:
            table = struct.unpack_from(f">{count + 1}I", bin_data, off)
            frames = []
            for i in range(count):
                tex, unk4 = parse_block(bin_data, off + table[i])
                frames.append(tex)
        else:
            tex, unk4 = parse_block(bin_data, off)
            frames = [tex]
        entries[off] = frames
    return words, entries, unk4


_zlb_memo = {}


def zlb_block(tex, unk4, level):
    # The same texture appears in many map banks: compress it once
    key = (tex, unk4, level)
    block = _zlb_memo.get(key)
    if block is None:
        comp = zlib.compress(tex, level)
        block = b"ZLB\0" + struct.pack(">III", unk4, len(tex), len(comp)) + comp
        _zlb_memo[key] = block
    return block


def build_bank(words, entries, converted, unk4, level, old_size):
    """converted: {original texture bytes -> new texture bytes}. old_size: size
    of the original .bin (tab entries past its end stay past the new end)."""
    out = bytearray()
    remap = {}
    for off in sorted(entries):
        frames = entries[off]
        while len(out) % 32:
            out.append(0)
        start = len(out)
        if start // 2 > 0xFFFFFF:
            raise BankError("bank larger than 32 MB")
        remap[off] = start
        blocks = [zlb_block(converted.get(f, f), unk4, level) for f in frames]
        if len(frames) > 1:
            table_size = 4 * (len(frames) + 1)
            rel = []
            pos = table_size
            for b in blocks:
                rel.append(pos)
                pos += len(b)
                pos = (pos + 3) & ~3
            rel.append(pos)
            out += struct.pack(f">{len(rel)}I", *rel)
            for b in blocks:
                out += b
                while (len(out) - start) % 4:
                    out.append(0)
        else:
            out += blocks[0]
    while len(out) % 32:
        out.append(0)
    tab = bytearray()
    for w in words:
        if w == 0xFFFFFFFF:
            tab += struct.pack(">I", w)
            break
        off = (w & 0xFFFFFF) * 2
        if off in remap:
            w = (w & 0xFF000000) | (remap[off] // 2)
        elif off >= old_size:
            # Unused entry pointing past the end of the original file: keep
            # it past the end of the new one (the bank may have grown)
            w = (w & 0xFF000000) | (min(len(out) + off - old_size, 0x1FFFFFE) // 2)
        tab += struct.pack(">I", w)
    return bytes(out), bytes(tab)


# --- Worker processes (each loads the converter library once) ---------------

_lib = None
_opts = None


def _worker_init(lib_path, quality, mipmaps, max_size, scale):
    global _lib, _opts
    _lib = ctypes.CDLL(lib_path)
    _lib.tc_convert.restype = ctypes.c_int
    _lib.tc_convert.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32,
                                ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    _opts = (quality, mipmaps, max_size, scale)


def _convert_one(tex):
    stats = ctypes.create_string_buffer(struct.calcsize(STATS_FORMAT))
    cap = max(len(tex) * 8, 1 << 20)
    out = ctypes.create_string_buffer(cap)
    n = _lib.tc_convert(tex, len(tex), out, cap, _opts[0], _opts[1], _opts[2], _opts[3],
                        ctypes.cast(stats, ctypes.c_void_p))
    if n < 0:
        cap = -n
        out = ctypes.create_string_buffer(cap)
        stats = ctypes.create_string_buffer(struct.calcsize(STATS_FORMAT))
        n = _lib.tc_convert(tex, len(tex), out, cap, _opts[0], _opts[1], _opts[2], _opts[3],
                            ctypes.cast(stats, ctypes.c_void_p))
    st = struct.unpack(STATS_FORMAT, stats.raw)
    psnr = max(st[18:34])  # Only this texture's format slot is set
    return (out.raw[:n] if n > 0 else b""), psnr


class TexConverter:
    def __init__(self, lib_path, cache_dir, quality="balanced", mipmaps=True, max_size=1024, scale=0, jobs=None):
        if quality not in QUALITY_LEVELS:
            raise ValueError(f"TEX_QUALITY must be one of {', '.join(QUALITY_LEVELS)}")
        self.lib_path = lib_path
        self.cache_dir = cache_dir
        self.quality = QUALITY_LEVELS[quality]
        self.mipmaps = 1 if mipmaps else 0
        self.max_size = max_size
        self.scale = int(scale)
        self.jobs = jobs or os.cpu_count() or 1
        self.tag = f"q{self.quality}m{self.mipmaps}s{self.max_size}x{self.scale}"
        os.makedirs(cache_dir, exist_ok=True)
        lib = ctypes.CDLL(lib_path)
        if lib.tc_stats_size() != struct.calcsize(STATS_FORMAT):
            raise RuntimeError("texconv library and tex_convert.py disagree on TcStats")

    def _cache_path(self, digest):
        return os.path.join(self.cache_dir, digest[:2], f"{digest}_{self.tag}.bin")

    def convert_textures(self, textures):
        """textures: iterable of texture bytes. Returns {bytes: converted bytes}
        (textures the converter skips map to themselves) and a report."""
        unique = {}
        for t in textures:
            unique.setdefault(t, None)
        todo = []
        result = {}
        psnrs = {}
        for t in unique:
            digest = hashlib.sha1(t).hexdigest()
            path = self._cache_path(digest)
            if os.path.isfile(path):
                with open(path, "rb") as f:
                    data = f.read()
                result[t] = data[8:] if len(data) > 8 else t
                if len(data) >= 8:
                    psnrs[t] = struct.unpack("<d", data[:8])[0]
            else:
                todo.append((t, path))
        if todo:
            print(f"tex_convert: converting {len(todo)} textures ({len(unique) - len(todo)} cached) "
                  f"on {self.jobs} processes", flush=True)
            with multiprocessing.Pool(self.jobs, _worker_init,
                                      (self.lib_path, self.quality, self.mipmaps, self.max_size, self.scale)) as pool:
                for (t, path), (conv, psnr) in zip(todo, pool.imap(_convert_one, [t for t, _ in todo], 16)):
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with open(path + ".tmp", "wb") as f:
                        f.write(struct.pack("<d", psnr) + conv)
                    os.replace(path + ".tmp", path)
                    result[t] = conv if conv else t
                    psnrs[t] = psnr
        return result, psnrs


def pica_header(tex):
    """(format, levels, width, height, flags) of a converted texture, or None."""
    if len(tex) < 0x60 + 32:
        return None
    magic, key, w, h, fmt, levels, flags = struct.unpack_from("<IIHHBBB", tex, 0x60)
    if magic != PT_MAGIC:
        return None
    return fmt, levels, w, h, flags


def verify_bank(old_words, old_entries, bin_data, tab_data, converted):
    """Re-parses a rewritten bank and compares it with the original."""
    words, entries, _ = parse_bank(bin_data, tab_data)
    if len(words) != len(old_words):
        raise BankError("tab length changed")
    for ow, nw in zip(old_words, words):
        if (ow & 0xFF000000) != (nw & 0xFF000000):
            raise BankError("tab flags/frame counts changed")
    if len(entries) != len(old_entries):
        raise BankError(f"entry count changed ({len(old_entries)} -> {len(entries)})")
    old_list = [old_entries[o] for o in sorted(old_entries)]
    new_list = [entries[o] for o in sorted(entries)]
    for of, nf in zip(old_list, new_list):
        if len(of) != len(nf):
            raise BankError("frame count changed")
        for a, b in zip(of, nf):
            if converted.get(a, a) != b:
                raise BankError("frame data mismatch after rewrite")
            if a[0x0A:0x0E] != b[0x0A:0x0E] or a[0x16] != b[0x16]:
                raise BankError("texture header (size/format) changed")


def convert_banks(files, converter, zlib_level=9, log=print):
    """files: {romfs path: bytes}. Converts every TEX bank pair in place."""
    pairs = {}
    for path in files:
        if is_texture_bank(path):
            stem, ext = os.path.splitext(path)
            pairs.setdefault(stem, {})[ext.lower()] = path
    parsed = {}
    all_textures = []
    for stem, p in sorted(pairs.items()):
        if ".bin" not in p or ".tab" not in p:
            continue
        try:
            words, entries, unk4 = parse_bank(files[p[".bin"]], files[p[".tab"]])
        except (BankError, struct.error, zlib.error) as e:
            log(f"tex_convert: {stem}: left unconverted ({e})")
            continue
        parsed[stem] = (words, entries, unk4)
        for frames in entries.values():
            all_textures.extend(frames)

    converted, psnrs = converter.convert_textures(all_textures)

    total_in = total_out = 0
    fmt_count = {}
    gx_to_pica = {}
    worst = {}
    for t, c in converted.items():
        h = pica_header(c) if c is not t else None
        if not h:
            fmt_count["unconverted"] = fmt_count.get("unconverted", 0) + 1
            continue
        name = PICA_FORMAT_NAMES.get(h[0], hex(h[0]))
        fmt_count[name] = fmt_count.get(name, 0) + 1
        key = (GX_FORMAT_NAMES.get(t[0x16], hex(t[0x16])), name)
        gx_to_pica[key] = gx_to_pica.get(key, 0) + 1
        p = psnrs.get(t, 99.0)
        if name not in worst or p < worst[name]:
            worst[name] = p

    for stem, (words, entries, unk4) in parsed.items():
        p = pairs[stem]
        old_bin, old_tab = files[p[".bin"]], files[p[".tab"]]
        new_bin, new_tab = build_bank(words, entries, converted, unk4, zlib_level, len(old_bin))
        verify_bank(words, entries, new_bin, new_tab, converted)
        files[p[".bin"]], files[p[".tab"]] = new_bin, new_tab
        total_in += len(old_bin)
        total_out += len(new_bin)

    log(f"tex_convert: {len(parsed)} banks, {len(converted)} unique textures, "
        f"bank data {total_in / 2**20:.1f} MB -> {total_out / 2**20:.1f} MB")
    log("tex_convert: formats " + ", ".join(f"{k} {v}" for k, v in sorted(fmt_count.items())))
    log("tex_convert: GX->PICA " + ", ".join(f"{a}->{b} {n}" for (a, b), n in sorted(gx_to_pica.items())))
    log("tex_convert: worst PSNR per format " + ", ".join(f"{k} {v:.1f} dB" for k, v in sorted(worst.items())))
    return {"banks": len(parsed), "textures": len(converted), "in": total_in, "out": total_out,
            "formats": fmt_count, "worst_psnr": worst}


if __name__ == "__main__":
    # Standalone: python -I tex_convert.py <libtexconv.so> <cache dir> <romfs files dir>
    lib, cache, root = sys.argv[1:4]
    files = {}
    for dp, dn, fn in os.walk(root):
        for f in fn:
            path = os.path.join(dp, f)
            rel = "/" + os.path.relpath(path, root).replace(os.sep, "/")
            if is_texture_bank(rel):
                with open(path, "rb") as fh:
                    files[rel] = fh.read()
    convert_banks(files, TexConverter(lib, cache))
