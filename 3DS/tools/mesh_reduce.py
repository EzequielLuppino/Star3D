"""Build-time polygon reduction for models and map blocks. Used by
extract_disc.py; the reduction itself is tools/native/meshsimp.c (ctypes).

Every display list of every model (MODELS.bin) and map block (mod*.zlb.bin)
is simplified to a fraction of its triangles (POLY_REDUCE percent removed,
default 50; 0 = off keeps the full polygon models) and written back over the original list, so nothing else in the
asset moves: only the list's size field changes. The lists of one asset are
reduced together as one mesh, so edges they share stay closed; the asset's
open edges (shared with the next map block) never move. Lists the reducer can't
handle safely (unclear vertex layout, lines/points, too small, no gain) stay
as they are. Assets are then recompressed and the bank's .tab offsets
rewritten. Results are cached per asset by content hash in
<build>/cache/mesh, like the textures.

Bank layouts (big-endian):
    MODELS.tab   u32 per model: 0x10000000 | offset, 0 = none. The word
                 before the 0xFFFFFFFF terminator is the .bin size (no flag).
    MODELS.bin   per model, 32-byte aligned: PackHeader 0xFACEFEED
                 (decompressed size, aux size 0xC, block size) + 0x14 bytes,
                 then a ZLB block (16-byte header + deflate data). The loader
                 inflates (block size - 0x10) bytes from the ZLB data.
                 Decompressed: a ModelFileHeader (model.h).
    modN.tab     u32 per block: 0x10000000 | offset, 0 = none, 0xFFFFFFFF
                 ends it.
    modN.zlb.bin per block, 32-byte aligned: a ZLB block (sizes come from its
                 header). Decompressed: MapBlockData (map_block.h).
"""
import ctypes
import hashlib
import multiprocessing
import os
import re
import struct
import zlib

MS_STATUS_NAMES = ("reduced", "small", "ambiguous", "no_layout", "no_gain", "unsupported", "error", "too_big")
MS_STATS = 10
ZLB_LEVEL = 9


class BankError(Exception):
    pass


def is_model_bank(path):
    name = os.path.basename(path)
    return name.upper() in ("MODELS.BIN", "MODELS.TAB")


_MAP_RE = re.compile(r"^mod(\d+)\.(zlb\.bin|tab)$", re.IGNORECASE)


def is_map_bank(path):
    return _MAP_RE.match(os.path.basename(path)) is not None


def round32(n):
    return (n + 31) & ~31


# --- ZLB blocks ----------------------------------------------------------------

def zlb_decode(data, off):
    """Returns (decompressed bytes, block length incl. header, version)."""
    if data[off:off + 4] != b"ZLB\0":
        raise BankError(f"no ZLB block at 0x{off:x}")
    ver, dsize, csize = struct.unpack_from(">III", data, off + 4)
    out = zlib.decompress(data[off + 16:off + 16 + csize])
    if len(out) != dsize:
        raise BankError(f"ZLB at 0x{off:x}: size {len(out)} != {dsize}")
    return out, 16 + csize, ver


def zlb_encode(raw, ver=1):
    comp = zlib.compress(raw, ZLB_LEVEL)
    return b"ZLB\0" + struct.pack(">III", ver, len(raw), len(comp)) + comp


# --- Display list reduction (worker side) ---------------------------------------

_lib = None
_params = None


def _worker_init(lib_path, keep, max_error, min_tris):
    global _lib, _params
    _lib = ctypes.CDLL(lib_path)
    _lib.ms_reduce_asset.restype = ctypes.c_int
    _lib.ms_reduce_asset.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int,
                                     ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                     ctypes.c_uint32, ctypes.c_uint32, ctypes.c_float, ctypes.c_float, ctypes.c_int,
                                     ctypes.c_int, ctypes.c_int, ctypes.c_float,
                                     ctypes.c_float, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    _params = (keep, max_error, min_tris)


# Largest change a collapse may make to a vertex colour (0..1 per channel) or
# texture coordinate (texture repeats) anywhere on the surface. Geometry error
# alone collapses flat floors freely, which stretched textures with
# hand-placed coordinates (OPEN_BUGS.md bug 31), and it removed every opaque
# vertex of the spirit door lock's fading disc, leaving only its alpha-0 rim
# (bug 36). Map blocks require the colour/uv layout (attr_strict); models use
# it where it fits (their vertices can carry normal indices too).
MAP_COLOR_TOL = 0.1
MAP_UV_TOL = 1.0 / 128


def _rewrite_lists(data, lists, pos_off, vcount, direct_min, direct_max, stats, color_count=0, tc_count=0,
                   color_off=0, tc_off=0, attr_strict=True):
    """lists: [(offset of the entry's u16 size field, list offset, size)].
    Reduces all lists of the asset together (shared edges stay closed) and
    rewrites them in place in data (bytearray)."""
    if vcount == 0 or pos_off + vcount * 6 > len(data):
        stats["bad_asset"] = stats.get("bad_asset", 0) + 1
        return
    uniq = []
    for _, off, size in lists:
        if size and off + size <= len(data) and off not in [u[0] for u in uniq]:
            uniq.append((off, size))
    if not uniq:
        return
    n = len(uniq)
    buf = (ctypes.c_uint8 * len(data)).from_buffer(data)
    offs = (ctypes.c_uint32 * n)(*[u[0] for u in uniq])
    sizes = (ctypes.c_uint32 * n)(*[u[1] for u in uniq])
    new_sizes = (ctypes.c_uint32 * n)()
    status = (ctypes.c_uint32 * n)()
    st = (ctypes.c_uint32 * MS_STATS)()
    _lib.ms_reduce_asset(buf, len(data), offs, sizes, n, pos_off, vcount, color_count, tc_count, color_off, tc_off,
                         MAP_COLOR_TOL, MAP_UV_TOL, 1 if attr_strict else 0, direct_min, direct_max, _params[0],
                         _params[1], _params[2], new_sizes, status, st)
    del buf
    new_size = {}
    for i, (off, size) in enumerate(uniq):
        name = MS_STATUS_NAMES[status[i]] if status[i] < len(MS_STATUS_NAMES) else "error"
        stats[name] = stats.get(name, 0) + 1
        stats["bytes_in"] = stats.get("bytes_in", 0) + size
        stats["bytes_out"] = stats.get("bytes_out", 0) + (new_sizes[i] or size)
        if new_sizes[i]:
            new_size[off] = new_sizes[i]
    stats["tris_in"] = stats.get("tris_in", 0) + st[1]
    stats["tris_out"] = stats.get("tris_out", 0) + st[2]
    for size_field, off, _ in lists:
        if off in new_size:
            struct.pack_into(">H", data, size_field, new_size[off])


def reduce_model(raw, stats):
    """raw: decompressed model file. Returns the modified bytes."""
    data = bytearray(raw)
    if len(data) < 0xFC:
        return raw
    verts_off = struct.unpack_from(">I", data, 0x28)[0]
    dl_off = struct.unpack_from(">I", data, 0xD0)[0]
    vcount = struct.unpack_from(">H", data, 0xE4)[0]
    joints = data[0xF3]
    count = data[0xF5] + data[0xF6]
    lists = []
    for i in range(count):
        e = dl_off + i * 0x1C
        if e + 0x1C > len(data):
            break
        off, size = struct.unpack_from(">IH", data, e)
        lists.append((e + 4, off, size))
    direct = (1, 10) if joints > 1 else (0, 0)
    colors_off, tc_off = struct.unpack_from(">II", data, 0x30)
    colors, texcoords = struct.unpack_from(">HH", data, 0xE8)
    if colors_off + colors * 2 > len(data):
        colors = colors_off = 0
    if tc_off + texcoords * 4 > len(data):
        texcoords = tc_off = 0
    _rewrite_lists(data, lists, verts_off, vcount, direct[0], direct[1], stats, colors, texcoords, colors_off, tc_off,
                   attr_strict=False)
    return bytes(data)


def reduce_map_block(raw, stats):
    """raw: decompressed map block. Returns the modified bytes."""
    data = bytearray(raw)
    if len(data) < 0xA4:
        return raw
    verts_off = struct.unpack_from(">I", data, 0x58)[0]
    dl_off = struct.unpack_from(">I", data, 0x68)[0]
    vcount = struct.unpack_from(">H", data, 0x90)[0]
    count = data[0xA1]
    lists = []
    for i in range(count):
        e = dl_off + i * 0x1C
        if e + 0x1C > len(data):
            break
        off, size = struct.unpack_from(">IH", data, e)
        lists.append((e + 4, off, size))
    colors, texcoords = struct.unpack_from(">HH", data, 0x94)
    colors_off, tc_off = struct.unpack_from(">II", data, 0x5C)
    _rewrite_lists(data, lists, verts_off, vcount, 0, 0, stats, colors, texcoords, colors_off, tc_off)
    return bytes(data)


def _reduce_job(job):
    kind, raw = job
    stats = {}
    try:
        out = reduce_model(raw, stats) if kind == "model" else reduce_map_block(raw, stats)
    except Exception as e:  # a malformed asset stays as it is
        stats = {"failed": 1, "error": str(e)}
        out = raw
    return out, stats


# --- Banks ------------------------------------------------------------------------

def parse_model_bank(bin_data, tab_data):
    """Returns (tab words, {offset: (prefix bytes, raw model, ZLB version)})."""
    words = list(struct.unpack(f">{len(tab_data) // 4}I", tab_data[:len(tab_data) // 4 * 4]))
    entries = {}
    for w in words:
        if w == 0xFFFFFFFF:
            break
        off = w & 0xFFFFFF
        if not (w & 0x10000000) or off in entries or off + 0x24 > len(bin_data):
            continue
        magic, dsize, aux, bsize = struct.unpack_from(">IIII", bin_data, off)
        if magic != 0xFACEFEED:
            continue
        zoff = off + 0x10 + aux + 8
        raw, blen, ver = zlb_decode(bin_data, zoff)
        if len(raw) != dsize:
            raise BankError(f"model at 0x{off:x}: size mismatch")
        entries[off] = (bin_data[off:zoff], raw, ver)
    return words, entries


def write_model_bank(words, entries, converted, bin_data):
    """Rebuilds MODELS.bin/.tab with the converted models."""
    out = bytearray()
    remap = {}
    for off in sorted(entries):
        prefix, raw, ver = entries[off]
        new_raw = converted.get(off, raw)
        block = zlb_encode(new_raw, ver)
        prefix = bytearray(prefix)
        struct.pack_into(">I", prefix, 4, len(new_raw))
        struct.pack_into(">I", prefix, 12, round32(len(block) - 16 + 0x34) - 0x20)  # as the retail files
        remap[off] = len(out)
        out += prefix + block
        out += bytes(round32(len(out)) - len(out))
    tab = bytearray()
    ended = False
    for i, w in enumerate(words):
        if not ended and w == 0xFFFFFFFF:
            ended = True
        elif not ended and w & 0x10000000 and (w & 0xFFFFFF) in remap:
            w = (w & 0xFF000000) | remap[w & 0xFFFFFF]
        elif not ended and w != 0 and i + 1 < len(words) and words[i + 1] == 0xFFFFFFFF:
            w = len(out)  # total size word
        elif not ended and w != 0:
            raise BankError(f"MODELS.tab word 0x{w:08x} points at no model")
        tab += struct.pack(">I", w)
    return bytes(out), bytes(tab)


def parse_map_bank(bin_data, tab_data):
    """Returns (tab words, {offset: (raw block, ZLB version)})."""
    words = list(struct.unpack(f">{len(tab_data) // 4}I", tab_data[:len(tab_data) // 4 * 4]))
    entries = {}
    for w in words:
        if w == 0xFFFFFFFF:
            break
        off = w & 0xFFFFFF
        if w == 0 or off in entries or off + 16 > len(bin_data) or bin_data[off:off + 4] != b"ZLB\0":
            continue
        raw, blen, ver = zlb_decode(bin_data, off)
        entries[off] = (raw, ver)
    return words, entries


def write_map_bank(words, entries, converted, bin_data):
    out = bytearray()
    remap = {}
    for off in sorted(entries):
        raw, ver = entries[off]
        block = zlb_encode(converted.get(off, raw), ver)
        remap[off] = len(out)
        out += block
        out += bytes(round32(len(out)) - len(out))
    tab = bytearray()
    ended = False
    for w in words:
        if not ended and w == 0xFFFFFFFF:
            ended = True
        elif not ended and w != 0:
            if (w & 0xFFFFFF) not in remap:
                raise BankError(f"map tab word 0x{w:08x} points at no block")
            w = (w & 0xFF000000) | remap[w & 0xFFFFFF]
        tab += struct.pack(">I", w)
    return bytes(out), bytes(tab)


class MeshReducer:
    def __init__(self, lib_path, cache_dir, reduce_percent=50, max_error=0.01, min_tris=16, jobs=None):
        self.lib_path = lib_path
        self.cache_dir = cache_dir
        self.keep = max(0.05, min(1.0, 1.0 - reduce_percent / 100.0))
        self.max_error = max_error
        self.min_tris = min_tris
        self.jobs = jobs or os.cpu_count() or 1
        # The reducer's own sources are part of the key: a changed algorithm
        # must not reuse old results
        code = hashlib.sha1()
        here = os.path.dirname(os.path.abspath(__file__))
        for name in ("mesh_reduce.py", os.path.join("native", "meshsimp.c")):
            with open(os.path.join(here, name), "rb") as f:
                code.update(f.read())
        self.tag = f"k{int(round(self.keep * 1000))}e{int(round(max_error * 10000))}t{min_tris}_{code.hexdigest()[:8]}"
        os.makedirs(cache_dir, exist_ok=True)
        if ctypes.CDLL(lib_path).ms_stats_count() != MS_STATS:
            raise RuntimeError("meshsimp library and mesh_reduce.py disagree on the stats layout")

    def _cache_path(self, kind, digest):
        return os.path.join(self.cache_dir, digest[:2], f"{digest}_{kind}_{self.tag}.bin")

    def reduce_assets(self, jobs):
        """jobs: [(kind, raw)]. Returns ([new raw], total stats)."""
        results = [None] * len(jobs)
        totals = {}
        todo = []
        for i, (kind, raw) in enumerate(jobs):
            path = self._cache_path(kind, hashlib.sha1(raw).hexdigest())
            if os.path.isfile(path):
                with open(path, "rb") as f:
                    blob = f.read()
                (slen,) = struct.unpack_from("<I", blob, 0)
                stats = eval_stats(blob[4:4 + slen])
                results[i] = blob[4 + slen:] or raw
                add_stats(totals, stats)
            else:
                todo.append((i, path))
        if todo:
            print(f"mesh_reduce: reducing {len(todo)} assets ({len(jobs) - len(todo)} cached) on {self.jobs} processes",
                  flush=True)
            with multiprocessing.Pool(self.jobs, _worker_init,
                                      (self.lib_path, self.keep, self.max_error, self.min_tris)) as pool:
                for (i, path), (out, stats) in zip(todo, pool.imap(_reduce_job, [jobs[i] for i, _ in todo], 4)):
                    raw = jobs[i][1]
                    sblob = encode_stats(stats)
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with open(path + ".tmp", "wb") as f:
                        f.write(struct.pack("<I", len(sblob)) + sblob + (out if out != raw else b""))
                    os.replace(path + ".tmp", path)
                    results[i] = out
                    add_stats(totals, stats)
        return results, totals


def encode_stats(stats):
    return ";".join(f"{k}={v}" for k, v in sorted(stats.items()) if isinstance(v, int)).encode()


def eval_stats(blob):
    out = {}
    for part in blob.decode().split(";"):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = int(v)
    return out


def add_stats(totals, stats):
    for k, v in stats.items():
        if isinstance(v, int):
            totals[k] = totals.get(k, 0) + v


def reduce_banks(files, reducer, log=print):
    """files: {path: bytes} with MODELS.bin/.tab and modN.zlb.bin/.tab pairs.
    Rewrites them in place. Returns a report."""
    pairs = []
    for path in sorted(files):
        name = os.path.basename(path)
        if name.upper() == "MODELS.BIN":
            tab = os.path.join(os.path.dirname(path), name[:-4] + (".tab" if name.endswith(".bin") else ".TAB"))
            tab = next((p for p in files if p.lower() == tab.lower()), None)
            if tab:
                pairs.append(("model", path, tab))
        else:
            m = _MAP_RE.match(name)
            if m and m.group(2).lower() == "zlb.bin":
                tab = os.path.join(os.path.dirname(path), f"mod{m.group(1)}.tab")
                tab = next((p for p in files if p.lower() == tab.lower()), None)
                if tab:
                    pairs.append(("map", path, tab))
    parsed = []
    jobs = []
    for kind, bpath, tpath in pairs:
        try:
            if kind == "model":
                words, entries = parse_model_bank(files[bpath], files[tpath])
                raws = {off: e[1] for off, e in entries.items()}
            else:
                words, entries = parse_map_bank(files[bpath], files[tpath])
                raws = {off: e[0] for off, e in entries.items()}
        except (BankError, zlib.error, struct.error) as e:
            log(f"mesh_reduce: {bpath}: skipped ({e})")
            continue
        start = len(jobs)
        offs = sorted(raws)
        jobs += [(kind, raws[o]) for o in offs]
        parsed.append((kind, bpath, tpath, words, entries, offs, start))
    results, totals = reducer.reduce_assets(jobs)
    for kind, bpath, tpath, words, entries, offs, start in parsed:
        converted = {o: results[start + i] for i, o in enumerate(offs)}
        writer = write_model_bank if kind == "model" else write_map_bank
        try:
            new_bin, new_tab = writer(words, entries, converted, files[bpath])
        except BankError as e:
            log(f"mesh_reduce: {bpath}: left unchanged ({e})")
            continue
        files[bpath], files[tpath] = new_bin, new_tab
    tin, tout = totals.get("tris_in", 0), totals.get("tris_out", 0)
    log(f"mesh_reduce: {len(pairs)} banks, {len(jobs)} assets, triangles {tin} -> {tout} "
        f"({100.0 * (tin - tout) / max(tin, 1):.1f}% removed); lists: " +
        ", ".join(f"{k} {totals.get(k, 0)}" for k in MS_STATUS_NAMES) +
        f"; failed assets {totals.get('failed', 0)}")
    return totals
