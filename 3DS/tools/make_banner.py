"""Make the 256x128 HOME Menu banner image: the game's Arwing, rendered from
the disc.

    python -I tools/make_banner.py <game.iso> <banner.png> <banner.wav> [font.ttf]
                                   [--cgfx banner.cgfx]

With --cgfx it also writes a 3D banner scene (see "3D banner" below and
banner_cgfx.py) and a software preview of it, <banner>_preview.png.

The model (ARWArwing, object 0x523, model 1152 in the root MODELS bank) and
its textures (root TEX1 bank) are read from the disc image itself, not from
the RomFS: the RomFS copies are polygon-reduced and converted to PICA
formats. The model is drawn in its bind pose: skinned vertices are stored
relative to their joint, which then only needs its bind position. A small
software rasteriser (perspective, z-buffer, textured, Gouraud lit, 3x
supersampled) draws it over a generated starfield; Pillow only does the
title text, the final downscale and the PNG. The title needs a TrueType
font (the Makefile passes the one in devkitPro's examples; without it the
banner has no text). The banner sound is synthesised. Everything is
deterministic, so the same disc always gives the same banner.

Model file layout (big-endian, model.h ModelFileHeader, as stored on disc):
    0x20 texture id table, 0x28 positions (3 x s16), 0x2C normals (3 s8, or
    9 with flags24 & 8), 0x30 colours (RGBA4), 0x34 texcoords (2 x s16),
    0x38 render ops (0x44 bytes each: layer texture index at 0x24,
    vtxAttrFlags 0x40, layerCount 0x41), 0xD0 display list table (0x1C
    each: offset, u16 size), 0xD4 render instruction bitstream.
The bitstream (objprint_dolphin.c modelDoRenderInstrs) is read LSB first in
4-bit opcodes: 1 = render op (6 bits), 2 = call display list (8 bits),
3 = vertex descriptor (1 bit per indexed attribute: 16-bit index or not),
4 = load joint matrices (4-bit count, 8 bits each), 5 = end.
"""
import importlib.util
import math
import os
import random
import struct
import sys
import wave

from PIL import Image, ImageDraw, ImageFilter

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
MODEL_ID = 1152
W, H = 256, 128
SS = 3  # supersampling factor


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(TOOLS_DIR, name + ".py"))
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


extract_disc = load_module("extract_disc")
mesh_reduce = load_module("mesh_reduce")
tex_convert = load_module("tex_convert")


# --- Disc ------------------------------------------------------------------------

def read_disc_files(iso_path, names):
    out = {}
    with open(iso_path, "rb") as iso:
        boot = extract_disc.read_at(iso, 0, 0x440)
        fst_offset, fst_size = struct.unpack_from(">II", boot, 0x424)
        fst = extract_disc.read_at(iso, fst_offset, fst_size)
        for path, offset, length in extract_disc.parse_fst(fst):
            if path in names:
                out[path] = extract_disc.read_at(iso, offset, length)
    missing = [n for n in names if n not in out]
    if missing:
        sys.exit("make_banner: not on the disc: " + ", ".join(missing))
    return out


# --- GameCube textures -------------------------------------------------------------

def _rgb565(v):
    return ((v >> 11) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31, 255)


def _rgb5a3(v):
    if v & 0x8000:
        return ((v >> 10 & 0x1F) * 255 // 31, (v >> 5 & 0x1F) * 255 // 31, (v & 0x1F) * 255 // 31, 255)
    return ((v >> 8 & 0xF) * 17, (v >> 4 & 0xF) * 17, (v & 0xF) * 17, (v >> 12 & 7) * 255 // 7)


def decode_gx_texture(tex):
    """tex: one game texture (0x60-byte header + GX data). Returns (w, h,
    [RGBA tuples], row-major). Level 0 only."""
    w, h = struct.unpack_from(">HH", tex, 0x0A)
    fmt = tex[0x16]
    data = tex[0x60:]
    px = [(255, 0, 255, 255)] * (w * h)
    # Block sizes in texels per GX format
    bw, bh = {0: (8, 8), 1: (8, 4), 2: (8, 4), 3: (4, 4), 4: (4, 4), 5: (4, 4), 6: (4, 4), 14: (8, 8)}.get(fmt, (0, 0))
    if not bw:
        return w, h, px
    pos = 0

    def put(x, y, c):
        if x < w and y < h:
            px[y * w + x] = c

    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            if fmt == 0:  # I4
                for y in range(8):
                    for x in range(0, 8, 2):
                        b = data[pos]
                        pos += 1
                        for k, v in enumerate((b >> 4, b & 0xF)):
                            put(bx + x + k, by + y, (v * 17,) * 3 + (255,))
            elif fmt == 1:  # I8
                for y in range(4):
                    for x in range(8):
                        v = data[pos]
                        pos += 1
                        put(bx + x, by + y, (v, v, v, 255))
            elif fmt == 2:  # IA4
                for y in range(4):
                    for x in range(8):
                        b = data[pos]
                        pos += 1
                        put(bx + x, by + y, ((b & 0xF) * 17,) * 3 + ((b >> 4) * 17,))
            elif fmt == 3:  # IA8
                for y in range(4):
                    for x in range(4):
                        a, v = data[pos], data[pos + 1]
                        pos += 2
                        put(bx + x, by + y, (v, v, v, a))
            elif fmt in (4, 5):  # RGB565, RGB5A3
                conv = _rgb565 if fmt == 4 else _rgb5a3
                for y in range(4):
                    for x in range(4):
                        put(bx + x, by + y, conv(data[pos] << 8 | data[pos + 1]))
                        pos += 2
            elif fmt == 6:  # RGBA8: AR plane then GB plane
                for i in range(16):
                    a, r = data[pos + 2 * i], data[pos + 2 * i + 1]
                    g, b = data[pos + 32 + 2 * i], data[pos + 33 + 2 * i]
                    put(bx + i % 4, by + i // 4, (r, g, b, a))
                pos += 64
            elif fmt == 14:  # CMPR: 2x2 DXT1 sub-blocks
                for sub in range(4):
                    c0, c1 = struct.unpack_from(">HH", data, pos)
                    bits = struct.unpack_from(">I", data, pos + 4)[0]
                    pos += 8
                    p0, p1 = _rgb565(c0), _rgb565(c1)
                    if c0 > c1:
                        pal = (p0, p1, tuple((2 * a + b) // 3 for a, b in zip(p0, p1)),
                               tuple((a + 2 * b) // 3 for a, b in zip(p0, p1)))
                    else:
                        pal = (p0, p1, tuple((a + b) // 2 for a, b in zip(p0, p1)), (0, 0, 0, 0))
                    sx, sy = bx + (sub & 1) * 4, by + (sub >> 1) * 4
                    for i in range(16):
                        put(sx + i % 4, sy + i // 4, pal[(bits >> (30 - 2 * i)) & 3])
    return w, h, px


def load_texture(bin_data, tab_data, tex_id):
    """Model texture ids index the TEX1 bank (textureLoad(-(id | 0x8000)))."""
    word = struct.unpack_from(">I", tab_data, (tex_id & 0x7FFF) * 4)[0]
    off = (word & 0xFFFFFF) * 2
    if (word >> 24) & 0x3F > 1:
        off += struct.unpack_from(">I", bin_data, off)[0]
    tex, _ = tex_convert.parse_block(bin_data, off)
    return decode_gx_texture(tex)


# --- Model ---------------------------------------------------------------------------

POS_FRAC = {0: 0, 1: 2, 2: 0, 3: 8, 4: 0, 5: 3, 6: 8, 7: 0}  # pi_videoinit.c GXSetVtxAttrFmt
TEX_FRAC = {0: 7, 1: 0, 2: 0, 3: 10, 4: 7, 5: 8, 6: 10, 7: 10}


class Bits:
    def __init__(self, data):
        self.data = data + b"\0\0\0"
        self.pos = 0

    def get(self, n):
        off = self.pos >> 3
        w = self.data[off] | self.data[off + 1] << 8 | self.data[off + 2] << 16
        v = (w >> (self.pos & 7)) & ((1 << n) - 1)
        self.pos += n
        return v


def parse_dlist(dl, layout, counts):
    """layout: (direct bytes, pos16, nrm16 or None, clr16 or None, [tex16]).
    Returns [(pos, nrm, tex0, vtxfmt) per triangle corner] or None when the
    list does not parse with this layout."""
    direct, p16, n16, c16, t16 = layout
    out = []
    i = 0
    n = len(dl)
    while i < n:
        op = dl[i]
        i += 1
        if op == 0:
            continue
        if op < 0x80 or op >= 0xC0:
            return None
        prim, fmt = op & 0xF8, op & 7
        if i + 2 > n:
            return None
        count = dl[i] << 8 | dl[i + 1]
        i += 2
        verts = []
        for _ in range(count):
            mtx = dl[i] if direct and i < n else None
            i += direct

            def idx(wide):
                nonlocal i
                if wide:
                    v = dl[i] << 8 | dl[i + 1]
                    i += 2
                else:
                    v = dl[i]
                    i += 1
                return v

            if i > n:
                return None
            pos = idx(p16)
            nrm = idx(n16) if n16 is not None else None
            if c16 is not None:
                idx(c16)
            tex = [idx(w) for w in t16]
            if i > n or pos >= counts[0] or (nrm is not None and nrm >= counts[1]) or \
                    any(t >= counts[2] for t in tex):
                return None
            verts.append((pos, nrm, tex[0] if tex else None, fmt, mtx))
        if prim == 0x90:
            tris = [verts[k:k + 3] for k in range(0, count - 2, 3)]
        elif prim == 0x98:
            tris = [(verts[k], verts[k + 1], verts[k + 2]) if k % 2 == 0 else (verts[k + 1], verts[k], verts[k + 2])
                    for k in range(count - 2)]
        elif prim == 0xA0:
            tris = [(verts[0], verts[k], verts[k + 1]) for k in range(1, count - 1)]
        elif prim == 0x80:
            tris = [t for k in range(0, count - 3, 4)
                    for t in ((verts[k], verts[k + 1], verts[k + 2]), (verts[k], verts[k + 2], verts[k + 3]))]
        else:
            tris = []  # lines and points
        out += tris
    return out


def parse_model(raw):
    """Returns (triangles, render ops). Triangles: (op index, 3 corners of
    (position, normal, uv, joint))."""
    u32 = lambda o: struct.unpack_from(">I", raw, o)[0]
    vcount, ncount, _, tcount = struct.unpack_from(">HHHH", raw, 0xE4)
    joints = raw[0xF3]
    texmtx = raw[0xFA]
    nbt = raw[0x24] & 8
    verts_off, nrm_off, tc_off, ops_off, dl_off = u32(0x28), u32(0x2C), u32(0x34), u32(0x38), u32(0xD0)
    ops = []
    for i in range(raw[0xF8]):
        r = ops_off + i * 0x44
        ops.append({
            "tex": struct.unpack_from(">i", raw, r + 0x24)[0],
            "aux": u32(r + 0x34) != 0xFFFFFFFF,
            "flags": u32(r + 0x3C),
            "vaf": raw[r + 0x40],
            "layers": raw[r + 0x41],
        })
    bits = Bits(raw[u32(0xD4):u32(0xD4) + struct.unpack_from(">H", raw, 0xD8)[0]])
    end = struct.unpack_from(">H", raw, 0xD8)[0] * 8
    op = None
    layout = None
    mtx_joints = [0]
    # Skinned vertices are stored relative to their joint; in the bind pose a
    # joint's matrix is just its bind position (ModelBone tail, the sum of
    # the parent chain's head offsets)
    joint_pos = [struct.unpack_from(">3f", raw, u32(0x3C) + j * 0x1C + 0x10) for j in range(joints)] or [(0, 0, 0)]
    tris = []
    while bits.pos < end:
        code = bits.get(4)
        if code == 1:
            op = bits.get(6)
        elif code == 3:
            o = ops[op]
            p16 = bits.get(1)
            n16 = bits.get(1) if o["vaf"] & 1 else None
            c16 = bits.get(1) if o["vaf"] & 2 else None
            t16 = bits.get(1)
            layout = [p16, n16, c16, [t16] * o["layers"]]
        elif code == 4:
            mtx_joints = [bits.get(8) for _ in range(bits.get(4))]
        elif code == 2:
            idx = bits.get(8)
            e = dl_off + idx * 0x1C
            off, size = struct.unpack_from(">IH", raw, e)
            dl = raw[off:off + size]
            # Direct matrix-index bytes before the indexed attributes: the
            # position/normal matrix when the model has a skeleton, plus
            # texture matrices whose count depends on runtime state
            # (modelRenderFn_setVtxDescr); the expected count is tried first
            o = ops[op]
            guess = 0
            if joints > 1:
                guess = 1 + texmtx + ((3 if o["aux"] else 1) if o["tex"] >= 0 else 0)
            parsed = None
            for direct in [guess] + [d for d in range(0, 12) if d != guess]:
                parsed = parse_dlist(dl, [direct] + layout, (vcount, ncount, tcount))
                if parsed is not None:
                    break
            if parsed is None:
                print(f"make_banner: display list {idx} does not parse, skipped")
                continue
            for corners in parsed:
                tri = []
                for pos, nrm, tex, fmt, mtx in corners:
                    # PNMTXIDX is the GX matrix slot * 3
                    joint = mtx_joints[mtx // 3] if mtx is not None and mtx // 3 < len(mtx_joints) else 0
                    ps = 1.0 / (1 << POS_FRAC[fmt])
                    x, y, z = struct.unpack_from(">hhh", raw, verts_off + pos * 6)
                    if nrm is not None:
                        nx, ny, nz = struct.unpack_from(">bbb", raw, nrm_off + nrm * (9 if nbt else 3))
                        n = (nx, ny, nz)
                    else:
                        n = None
                    if tex is not None:
                        ts = 1.0 / (1 << TEX_FRAC[fmt])
                        s, t = struct.unpack_from(">hh", raw, tc_off + tex * 4)
                        uv = (s * ts, t * ts)
                    else:
                        uv = (0.0, 0.0)
                    jx, jy, jz = joint_pos[joint] if joint < len(joint_pos) else (0, 0, 0)
                    tri.append(((x * ps + jx, y * ps + jy, z * ps + jz), n, uv, joint))
                tris.append((op, tri))
        elif code == 5:
            break
    return tris, ops


# --- Rasteriser --------------------------------------------------------------------------

def normalize(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) or 1.0
    return (v[0] / l, v[1] / l, v[2] / l)


def rotation(yaw, pitch, roll):
    """Model -> view rotation (degrees): roll about the nose (z), then yaw
    about y, then pitch about x."""
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    cr, sr = math.cos(math.radians(roll)), math.sin(math.radians(roll))
    rz = ((cr, -sr, 0), (sr, cr, 0), (0, 0, 1))
    ry = ((cy, 0, sy), (0, 1, 0), (-sy, 0, cy))
    rx = ((1, 0, 0), (0, cp, -sp), (0, sp, cp))

    def mul(a, b):
        return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))

    return mul(rx, mul(ry, rz))


def apply(m, v):
    return (m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2],
            m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2])


def render_model(tris, ops, textures, size, box, rot, light):
    """Draws the triangles into an RGBA image of size (w, h), fitted into box
    (x0, y0, x1, y1). Returns the image."""
    w, h = size
    rgba = bytearray(w * h * 4)
    zbuf = [1e30] * (w * h)
    # View space: x right, y up, z away from the camera
    # GX model space is right-handed and this view space is left-handed:
    # mirror x so the model is not drawn mirror-imaged
    view = [(op, [(apply(rot, (-c[0][0], c[0][1], c[0][2])), apply(rot, (-c[1][0], c[1][1], c[1][2])) if c[1] else None,
                   c[2]) for c in tri]) for op, tri in tris]
    pts = [p for _, tri in view for p, _, _ in tri]
    cx = (min(p[0] for p in pts) + max(p[0] for p in pts)) / 2
    cy = (min(p[1] for p in pts) + max(p[1] for p in pts)) / 2
    cz = (min(p[2] for p in pts) + max(p[2] for p in pts)) / 2
    radius = max(math.dist((cx, cy, cz), p) for p in pts)
    dist = radius * 3.2  # mild perspective
    proj = []
    for p in pts:
        z = p[2] - cz + dist
        proj.append(((p[0] - cx) / z, (p[1] - cy) / z))
    px0, px1 = min(q[0] for q in proj), max(q[0] for q in proj)
    py0, py1 = min(q[1] for q in proj), max(q[1] for q in proj)
    bx0, by0, bx1, by1 = box
    scale = min((bx1 - bx0) / (px1 - px0), (by1 - by0) / (py1 - py0))
    ox = (bx0 + bx1) / 2 - (px0 + px1) / 2 * scale
    oy = (by0 + by1) / 2 + (py0 + py1) / 2 * scale
    light = normalize(light)
    half = normalize((light[0], light[1], light[2] - 1.0))

    for op, tri in view:
        tex_w, tex_h, texels = textures.get(ops[op]["tex"], (1, 1, [(200, 200, 200, 255)]))
        glass = ops[op]["flags"] & 0x40000000
        sv = []
        for p, n, uv in tri:
            z = p[2] - cz + dist
            sx = ox + (p[0] - cx) / z * scale
            sy = oy - (p[1] - cy) / z * scale
            if n is None:
                n = (0.0, 0.0, -1.0)
            n = normalize(n)
            # Two-sided: light the side that faces the camera
            if n[2] > 0:
                n = (-n[0], -n[1], -n[2])
            diff = max(0.0, -(n[0] * light[0] + n[1] * light[1] + n[2] * light[2]))
            spec = max(0.0, -(n[0] * half[0] + n[1] * half[1] + n[2] * half[2])) ** 28
            rim = (1.0 - abs(n[2])) ** 3
            sv.append((sx, sy, 1.0 / z, uv[0] / z, uv[1] / z, 0.32 + 0.85 * diff, spec * 0.55 + rim * 0.25))
        (x0, y0, iz0, u0, v0, l0, s0), (x1, y1, iz1, u1, v1, l1, s1), (x2, y2, iz2, u2, v2, l2, s2) = sv
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        minx, maxx = max(int(min(x0, x1, x2)), 0), min(int(max(x0, x1, x2)) + 1, w - 1)
        miny, maxy = max(int(min(y0, y1, y2)), 0), min(int(max(y0, y1, y2)) + 1, h - 1)
        inv = 1.0 / area
        for y in range(miny, maxy + 1):
            fy = y + 0.5
            for x in range(minx, maxx + 1):
                fx = x + 0.5
                a = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) * inv
                b = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) * inv
                c = 1.0 - a - b
                if a < 0 or b < 0 or c < 0:
                    continue
                iz = a * iz0 + b * iz1 + c * iz2
                z = 1.0 / iz
                i = y * w + x
                if z >= zbuf[i]:
                    continue
                zbuf[i] = z
                u = (a * u0 + b * u1 + c * u2) * z
                v = (a * v0 + b * v1 + c * v2) * z
                lit = a * l0 + b * l1 + c * l2
                spec = (a * s0 + b * s1 + c * s2) * 255
                t = texels[(int(v * tex_h) % tex_h) * tex_w + int(u * tex_w) % tex_w]
                if glass:
                    t = (40 + t[0] // 3, 90 + t[1] // 3, 150 + t[2] // 3, 255)
                o = i * 4
                rgba[o] = min(255, int(t[0] * lit + spec))
                rgba[o + 1] = min(255, int(t[1] * lit + spec))
                rgba[o + 2] = min(255, int(t[2] * lit + spec))
                rgba[o + 3] = 255
    return Image.frombytes("RGBA", (w, h), bytes(rgba))


# --- Banner ------------------------------------------------------------------------------

# The bind pose has the landing gear down (joints 1-8: nose and main gear
# struts); a ship in flight looks better without it
HIDE_JOINTS = set(range(1, 9))
# Nose towards the lower left, seen a little from below, banking
VIEW = (-150.0, -15.0, 20.0)
LIGHT = (-0.5, -0.7, 0.6)


def space_background(w, h):
    """Deep-space gradient, a soft nebula, a planet rim and stars. Seeded, so
    the result never changes."""
    rnd = random.Random(0x5F3D)
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = (int(4 + 10 * t), int(6 + 12 * t), int(22 + 34 * t), 255)
        for x in range(w):
            px[x, y] = c
    glow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(glow)
    for _ in range(7):
        cx, cy = rnd.uniform(0.15, 0.95) * w, rnd.uniform(0.1, 0.9) * h
        r = rnd.uniform(0.12, 0.3) * w
        col = rnd.choice(((70, 40, 140), (30, 70, 150), (110, 40, 120)))
        d.ellipse((cx - r, cy - r * 0.6, cx + r, cy + r * 0.6), fill=col + (60,))
    glow = glow.filter(ImageFilter.GaussianBlur(w / 14))
    img.alpha_composite(glow)
    stars = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(stars)
    for _ in range(w * h // 2400):
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        r = rnd.choice((0.35, 0.35, 0.45, 0.45, 0.6, 0.9)) * w / 256
        a = rnd.randint(70, 255)
        tint = rnd.choice(((255, 255, 255), (200, 220, 255), (255, 240, 210)))
        d.ellipse((x - r, y - r, x + r, y + r), fill=tint + (a,))
    img.alpha_composite(stars)
    # Planet in the lower right corner, lit from the upper left
    planet = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    pcx, pcy, pr = w * 0.98, h * 1.62, h * 0.9
    d = ImageDraw.Draw(planet)
    d.ellipse((pcx - pr * 1.06, pcy - pr * 1.06, pcx + pr * 1.06, pcy + pr * 1.06), fill=(90, 170, 255, 70))
    planet = planet.filter(ImageFilter.GaussianBlur(w / 80))
    body = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    bp = body.load()
    for y in range(max(0, int(pcy - pr)), h):
        for x in range(max(0, int(pcx - pr)), w):
            dx, dy = (x - pcx) / pr, (y - pcy) / pr
            r2 = dx * dx + dy * dy
            if r2 > 1:
                continue
            nz = math.sqrt(1 - r2)
            lit = max(0.0, -0.6 * dx - 0.55 * dy + 0.25 * nz)
            band = 0.5 + 0.5 * math.sin(dy * 9 + math.sin(dx * 5) * 1.5)
            base = (30 + 40 * band, 90 + 50 * band, 70 + 40 * band)
            edge = min(1.0, (1 - math.sqrt(r2)) * 40)
            bp[x, y] = tuple(min(255, int(c * (0.12 + lit * 1.1))) for c in base) + (int(255 * edge),)
    planet.alpha_composite(body)
    img.alpha_composite(planet)
    return img


def title_text(size, font_path, scale):
    """'Star 3D' in the corner: white-to-blue gradient letters with a dark
    outline. None without a font file."""
    from PIL import ImageFont
    if not font_path or not os.path.isfile(font_path):
        return None
    w, h = size
    font = ImageFont.truetype(font_path, int(30 * scale))
    text = "Star 3D"
    mask = Image.new("L", size, 0)
    md = ImageDraw.Draw(mask)
    x0, y0, x1, y1 = md.textbbox((0, 0), text, font=font, stroke_width=int(1 * scale))
    tx = w - (x1 - x0) - int(8 * scale) - x0
    ty = int(6 * scale) - y0
    md.text((tx, ty), text, font=font, fill=255, stroke_width=int(1 * scale), stroke_fill=255)
    outline = Image.new("L", size, 0)
    ImageDraw.Draw(outline).text((tx, ty), text, font=font, fill=255, stroke_width=int(3 * scale), stroke_fill=255)
    out = Image.new("RGBA", size, (0, 0, 0, 0))
    shadow = Image.new("RGBA", size, (5, 10, 40, 0))
    shadow.putalpha(outline.filter(ImageFilter.GaussianBlur(scale)))
    out.alpha_composite(shadow)
    grad = Image.new("RGBA", size)
    gp = grad.load()
    top, bot = ty + y0, ty + y1
    for y in range(h):
        t = min(1.0, max(0.0, (y - top) / max(1, bot - top)))
        c = (int(255 - 120 * t), int(255 - 80 * t), 255, 255)
        for x in range(w):
            gp[x, y] = c
    grad.putalpha(mask)
    out.alpha_composite(grad)
    return out


# --- 3D banner -----------------------------------------------------------------------
#
# The HOME Menu draws the banner from a CGFX scene in stereoscopic 3D. The 3D
# banner (--cgfx) is the bannertool plane, carrying the starfield and the
# title, plus the Arwing itself as a real mesh in front of it. Coordinates
# are those of the plane's billboard bone: x right, y up, z towards the
# viewer, the plane 26 x 13 units at z = 0 (x -13..13, y -7.5..5.5; one unit
# is 256 / 26 banner pixels). The camera distance is the HOME Menu's and is
# not known; ship depths are kept to a few units so the pop-out stays
# moderate whatever it is.

PLANE_X0, PLANE_Y1, PLANE_UNITS = -13.0, 5.5, 26.0
# Ship footprint in banner pixels (margins leave room for the nose, which is
# nearest the camera and grows most in perspective) and its depth range
SHIP_BOX = (36, 28, 180, 118)
SHIP_Z = (0.5, 6.5)
# Nose to the left, banking: a 3/4 view with much less depth than the
# 2D banner's (whose nose points almost at the camera)
VIEW_3D = (-110.0, -25.0, 35.0)
SHIP_TEX = 256           # hull atlas size in the banner (the disc's is 512)
# Glass canopy: no texture of its own; it samples the brightest flat spot of
# the hull atlas, tinted by its vertex colour
GLASS_TINT = (110, 170, 235)
# Assumed camera distance for the preview only (vertical field of view of
# about 30 degrees over the 13-unit plane)
PREVIEW_DISTANCE = 24.0


def bayer_rgba4(img):
    """Ordered dither before the RGBA4 truncation, so gradients and the hull
    shading do not band."""
    m = (0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5)
    w, h = img.size
    src = img.tobytes()
    out = bytearray(len(src))
    for y in range(h):
        for x in range(w):
            d = m[(y & 3) * 4 + (x & 3)]
            o = (y * w + x) * 4
            for c in range(4):
                out[o + c] = min(255, src[o + c] + d)
    return bytes(out)


def bright_spot(img, cell=8):
    """UV centre of the brightest, flattest cell x cell block of the atlas."""
    w, h = img.size
    px = img.convert("L").load()
    best = None
    for by in range(0, h, cell):
        for bx in range(0, w, cell):
            vals = [px[x, y] for y in range(by, by + cell) for x in range(bx, bx + cell)]
            mean = sum(vals) / len(vals)
            score = mean - 4 * (max(vals) - min(vals))
            if best is None or score > best[0]:
                best = (score, bx, by)
    _, bx, by = best
    # Image row y from the top is v = 1 - y / h (memory row 0 sits at v = 1)
    return ((bx + cell / 2) / w, 1.0 - (by + cell / 2) / h)


def arwing_mesh(tris, ops, atlas):
    """The ship in the bone's frame: positions, UVs, baked colours (the 2D
    banner's light), triangles sorted back to front."""
    rot = rotation(*VIEW_3D)
    light = normalize(LIGHT)
    half = normalize((light[0], light[1], light[2] - 1.0))
    glass_uv = bright_spot(atlas)
    corners = []
    for op, tri in tris:
        glass = ops[op]["flags"] & 0x40000000
        out = []
        for p, n, uv, _ in tri:
            # Model -> the 2D renderer's left-handed view space (mirror x),
            # then -> the bone frame (z towards the viewer): two mirrors, so
            # the ship is not mirror-imaged
            v = apply(rot, (-p[0], p[1], p[2]))
            nv = normalize(apply(rot, (-n[0], n[1], n[2])) if n else (0.0, 0.0, -1.0))
            if nv[2] > 0:
                nv = (-nv[0], -nv[1], -nv[2])  # two-sided: light the side facing the camera
            diff = max(0.0, -(nv[0] * light[0] + nv[1] * light[1] + nv[2] * light[2]))
            spec = max(0.0, -(nv[0] * half[0] + nv[1] * half[1] + nv[2] * half[2])) ** 28
            rim = (1.0 - abs(nv[2])) ** 3
            lit = min(1.0, 0.30 + 0.80 * diff + 0.45 * spec + 0.2 * rim)
            if glass:
                col = tuple(int(c * min(1.0, 0.45 + 0.6 * diff + 0.8 * spec)) for c in GLASS_TINT) + (255,)
                tuv = glass_uv
            else:
                col = (int(255 * lit),) * 3 + (255,)
                # GX t runs down the image, PICA v up it
                tuv = (uv[0], 1.0 - uv[1])
            out.append(((v[0], v[1], -v[2]), tuv, col))
        corners.append(out)

    # Fit: x/y into SHIP_BOX (banner pixels -> plane units), z into SHIP_Z
    pts = [c[0] for tri in corners for c in tri]
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    bx0, by0, bx1, by1 = SHIP_BOX
    unit = PLANE_UNITS / W
    scale = min((bx1 - bx0) * unit / (hi[0] - lo[0]), (by1 - by0) * unit / (hi[1] - lo[1]))
    cx = PLANE_X0 + (bx0 + bx1) / 2 * unit
    cy = PLANE_Y1 - (by0 + by1) / 2 * unit
    zscale = min(scale, (SHIP_Z[1] - SHIP_Z[0]) / (hi[2] - lo[2]))

    def place(p):
        return (cx + (p[0] - (lo[0] + hi[0]) / 2) * scale,
                cy + (p[1] - (lo[1] + hi[1]) / 2) * scale,
                SHIP_Z[0] + (p[2] - lo[2]) * zscale)

    # Back to front, for a correct picture even if the runtime drew the
    # ship without depth writes
    corners.sort(key=lambda tri: sum(c[0][2] for c in tri))
    index = {}
    positions, uvs, colors, triangles = [], [], [], []
    for tri in corners:
        ids = []
        for p, uv, col in tri:
            q = place(p)
            key = (round(q[0], 4), round(q[1], 4), round(q[2], 4), round(uv[0], 5), round(uv[1], 5), col)
            if key not in index:
                index[key] = len(positions)
                positions.append(q)
                uvs.append(uv)
                colors.append(col)
            ids.append(index[key])
        triangles.append(tuple(ids))
    return positions, uvs, colors, triangles, zscale / scale


def render_scene(meshes, textures, size, distance):
    """Software preview of the CGFX scene: a pinhole camera on the bone's z
    axis at the given distance, aimed at the plane's centre, vertical field
    of view just covering the plane. meshes: (positions, uvs, colors or
    None, triangles, texture name); textures: name -> (w, h, rgba top row
    first). Drawn in order with a z-buffer, texture x vertex colour, like
    the material's stage 0."""
    w, h = size
    eye_y = PLANE_Y1 - 6.5
    f = h / 2 / (6.5 / distance)
    rgba = bytearray(w * h * 4)
    zbuf = [-1e30] * (w * h)
    for positions, uvs, colors, triangles, tex in meshes:
        tw, th, texels = textures[tex]
        sv = []
        for (x, y, z), (u, v) in zip(positions, uvs):
            d = distance - z
            sv.append((w / 2 + x * f / d, h / 2 - (y - eye_y) * f / d, 1.0 / d, u / d, v / d))
        for tri in triangles:
            (x0, y0, iz0, u0, v0), (x1, y1, iz1, u1, v1), (x2, y2, iz2, u2, v2) = (sv[i] for i in tri)
            c0, c1, c2 = ((colors[i] for i in tri) if colors else ((255, 255, 255, 255),) * 3)
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(area) < 1e-9:
                continue
            inv = 1.0 / area
            for y in range(max(int(min(y0, y1, y2)), 0), min(int(max(y0, y1, y2)) + 1, h - 1) + 1):
                fy = y + 0.5
                for x in range(max(int(min(x0, x1, x2)), 0), min(int(max(x0, x1, x2)) + 1, w - 1) + 1):
                    fx = x + 0.5
                    a = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) * inv
                    b = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) * inv
                    c = 1.0 - a - b
                    if a < 0 or b < 0 or c < 0:
                        continue
                    iz = a * iz0 + b * iz1 + c * iz2
                    i = y * w + x
                    if iz <= zbuf[i]:
                        continue
                    zbuf[i] = iz
                    z = 1.0 / iz
                    u = (a * u0 + b * u1 + c * u2) * z
                    v = (a * v0 + b * v1 + c * v2) * z
                    # v = 1 is the image's top row
                    t = texels[(int((1.0 - v) * th) % th) * tw + int(u * tw) % tw]
                    o = i * 4
                    for k in range(3):
                        rgba[o + k] = int(t[k] * (a * c0[k] + b * c1[k] + c * c2[k]) / 255)
                    rgba[o + 3] = 255
    return Image.frombytes("RGBA", (w, h), bytes(rgba))


def write_3d_banner(path, preview_path, tris, ops, textures, font_path):
    banner_cgfx = load_module("banner_cgfx")
    # Background: the 2D banner without the ship
    size = (W * SS, H * SS)
    bg = space_background(*size)
    text = title_text(size, font_path, SS)
    if text is not None:
        bg.alpha_composite(text)
    bg = bg.resize((W, H), Image.LANCZOS)
    tw, th, texels = textures[0]
    atlas = Image.new("RGBA", (tw, th))
    atlas.putdata(texels)
    atlas = atlas.resize((SHIP_TEX, SHIP_TEX), Image.LANCZOS)
    positions, uvs, colors, triangles, zsquash = arwing_mesh(tris, ops, atlas)
    plane = banner_cgfx.plane_mesh()
    ship = banner_cgfx.Mesh("mt_arwing", "ARWING", positions, uvs, triangles, colors,
                            depth_write=True, cull_back=False)
    bg_px, atlas_px = bayer_rgba4(bg), bayer_rgba4(atlas)
    data = banner_cgfx.build([plane, ship], [banner_cgfx.Texture("COMMON1", W, H, bg_px),
                                             banner_cgfx.Texture("ARWING", SHIP_TEX, SHIP_TEX, atlas_px)])
    if len(data) > 0x80000:
        sys.exit(f"make_banner: banner CGFX is {len(data)} bytes, the HOME Menu takes 0x80000 at most")
    with open(path, "wb") as f:
        f.write(data)

    def texel_list(px, w, h):
        return (w, h, [tuple(c >> 4 << 4 | c >> 4 for c in px[i:i + 4]) for i in range(0, w * h * 4, 4)])

    preview = render_scene([(plane.positions, plane.uvs, None, plane.triangles, "COMMON1"),
                            (positions, uvs, colors, triangles, "ARWING")],
                           {"COMMON1": texel_list(bg_px, W, H), "ARWING": texel_list(atlas_px, SHIP_TEX, SHIP_TEX)},
                           (W * 2, H * 2), PREVIEW_DISTANCE)
    preview.save(preview_path)
    print(f"make_banner: 3D banner, {len(triangles)} triangles, {len(positions)} vertices, "
          f"depth squash {zsquash:.2f}, CGFX {len(data)} bytes -> {path}")


def write_chime(path, rate=32000):
    """The banner's sound (the HOME Menu plays it when the icon is selected,
    3 seconds at most): a short rising bell arpeggio, synthesised so nothing
    from the disc's audio banks is needed. 16-bit mono PCM."""
    notes = ((0.00, 659.26), (0.11, 987.77), (0.22, 1318.51), (0.33, 1975.53))  # E5 B5 E6 B6
    length = int(rate * 1.6)
    samples = [0.0] * length
    for start, freq in notes:
        first = int(start * rate)
        for i in range(length - first):
            t = i / rate
            env = math.exp(-t * 3.2) * min(1.0, t * 400)
            samples[first + i] += env * (math.sin(2 * math.pi * freq * t) +
                                         0.3 * math.sin(2 * math.pi * freq * 2.01 * t) * math.exp(-t * 6))
    peak = max(abs(v) for v in samples) or 1.0
    with wave.open(path, "wb") as f:
        f.setnchannels(1)
        f.setsampwidth(2)
        f.setframerate(rate)
        f.writeframes(b"".join(struct.pack("<h", int(v / peak * 0.45 * 32767)) for v in samples))


def main():
    args = sys.argv[1:]
    cgfx_path = None
    if "--cgfx" in args:
        i = args.index("--cgfx")
        cgfx_path = args[i + 1]
        del args[i:i + 2]
    if len(args) not in (3, 4):
        sys.exit("usage: make_banner.py <game.iso> <banner.png> <banner.wav> [font.ttf] [--cgfx banner.cgfx]")
    iso, png_path, wav_path = args[:3]
    font_path = args[3] if len(args) == 4 else None
    files = read_disc_files(iso, ["/MODELS.bin", "/MODELS.tab", "/TEX1.bin", "/TEX1.tab"])
    words, entries = mesh_reduce.parse_model_bank(files["/MODELS.bin"], files["/MODELS.tab"])
    word = words[MODEL_ID] if MODEL_ID < len(words) else 0
    if not word & 0x10000000 or (word & 0xFFFFFF) not in entries:
        sys.exit(f"make_banner: model {MODEL_ID} not in MODELS.bin")
    raw = entries[word & 0xFFFFFF][1]
    tris, ops = parse_model(raw)
    tris = [(op, tri) for op, tri in tris if not any(c[3] in HIDE_JOINTS for c in tri)]
    tex_ids = struct.unpack_from(f">{raw[0xF2]}I", raw, struct.unpack_from(">I", raw, 0x20)[0])
    textures = {}
    for op in ops:
        if 0 <= op["tex"] < len(tex_ids) and op["tex"] not in textures:
            textures[op["tex"]] = load_texture(files["/TEX1.bin"], files["/TEX1.tab"], tex_ids[op["tex"]])

    size = (W * SS, H * SS)
    img = space_background(*size)
    ship = render_model(tris, ops, textures, size, (4 * SS, 14 * SS, 196 * SS, 124 * SS), rotation(*VIEW), LIGHT)
    # Faint blue halo so the hull reads against the dark sky
    halo = Image.new("RGBA", size, (120, 170, 255, 0))
    halo.putalpha(ship.getchannel("A").filter(ImageFilter.GaussianBlur(3 * SS)).point(lambda a: a * 45 // 100))
    img.alpha_composite(halo)
    img.alpha_composite(ship)
    text = title_text(size, font_path, SS)
    if text is not None:
        img.alpha_composite(text)
    img = img.resize((W, H), Image.LANCZOS)
    img.save(png_path)
    write_chime(wav_path)
    print(f"make_banner: model {MODEL_ID}, {len(tris)} triangles -> {png_path}"
          + ("" if text is not None else " (no font, no title text)"))
    if cgfx_path:
        write_3d_banner(cgfx_path, os.path.splitext(cgfx_path)[0] + "_preview.png", tris, ops, textures, font_path)


if __name__ == "__main__":
    main()
