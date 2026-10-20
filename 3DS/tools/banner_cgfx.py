"""Write the HOME Menu banner model: a CGFX file (the NintendoWare for CTR
scene format the HOME Menu draws banners from) with any number of textured
meshes.

    import banner_cgfx
    data = banner_cgfx.build([Mesh(...), ...], [Texture(...), ...])

The layout copies, field for field, the flat banner that bannertool writes
for a PNG (its built-in 0x1580-byte CGFX header, which came from an official
banner): one skeletal model "COMMON" with a single billboard bone "banner"
(the camera always looks straight at it, so the bone's frame is the screen:
x right, y up, z towards the viewer), the same animation-group table, and
for every mesh a copy of its "mt_banner" material (DefaultShader, primary
colour x texture 0, alpha blending). With the bannertool plane as the only
mesh, build() reproduces bannertool's file byte for byte (check_template()
compares against the CGFX inside a flat .bnr), so every structure the HOME
Menu reads is one it already accepts from bannertool banners. Extra meshes add a vertex colour attribute (the DefaultShader
then outputs the vertex colour as the primary colour, which is how baked
lighting gets in) and can switch on depth writes and turn off face culling;
every material state that differs from the plane's also gets a different
state hash, since the runtime may skip state it believes unchanged.

Pointers are self-relative 32-bit offsets. Strings are collected after the
DATA objects in order of first use; vertex, index and texture data go to the
IMAG block (buffers 8-byte aligned, textures 0x80-aligned, as in the
template). Texture data is RGBA4 in PICA 8x8 Morton tiles with the image's
top row first (the PICA samples memory row 0 at v = 1), exactly as bannertool
converts the banner PNG.
"""
import hashlib
import struct

# GL enums of the texture and vertex formats used here
GL_UNSIGNED_BYTE = 0x1401
GL_UNSIGNED_SHORT = 0x1403
GL_FLOAT = 0x1406
GL_RGBA_NATIVE_DMP = 0x6752
GL_UNSIGNED_SHORT_4_4_4_4 = 0x8033
PICA_RGBA4 = 4
# PICA vertex attribute names
ATTR_POSITION, ATTR_COLOR, ATTR_TEXCOORD0 = 0, 3, 4

# State hashes of the bannertool material (mt_banner), in file order:
# material flags, shader parameters, texture coordinators, samplers,
# mappers, material colours, rasterisation, fragment lighting, lighting
# table, lighting table parameters, texture combiners, alpha test, fragment
# operation, then the unique id
TEMPLATE_HASHES = (0xDA5DE5CD, 0x3B75655E, 0xF43DF7F4, 0x9AE75B22, 0, 0x66F7700C, 0xC06AC8F4,
                   0xE0099C31, 0, 0x3B75655E, 0x3582016C, 0x640102F8, 0xCDF201A2, 0)
HASH_RASTER, HASH_FRAGOP = 6, 12


class Texture:
    """name, width, height (powers of two), rgba: bytes, top row first."""
    def __init__(self, name, width, height, rgba):
        self.name, self.width, self.height, self.rgba = name, width, height, rgba


class Mesh:
    """positions [(x, y, z)], uvs [(u, v)], colors [(r, g, b, a)] 0-255 or
    None (no colour attribute: the plane), triangles [(i, j, k)]."""
    def __init__(self, material, texture, positions, uvs, triangles, colors=None,
                 depth_write=False, cull_back=True):
        self.material, self.texture = material, texture
        self.positions, self.uvs, self.colors, self.triangles = positions, uvs, colors, triangles
        self.depth_write, self.cull_back = depth_write, cull_back


# --- Assembler -----------------------------------------------------------------

class Ptr:
    def __init__(self, label):
        self.label = label


class Str:
    def __init__(self, text):
        self.text = text


class F32(float):
    pass


class Label:
    def __init__(self, name):
        self.name = name


class Align:
    def __init__(self, n):
        self.n = n


def _size(item):
    return len(item) if isinstance(item, bytes) else 4


class Assembler:
    """Two item lists (DATA objects, IMAG data). Items: int (u32), F32,
    bytes, Ptr(label), Str(text), Label(name), Align(n)."""

    def __init__(self):
        self.data = []
        self.imag = []
        self.counter = 0

    def new_label(self, hint):
        self.counter += 1
        return f"{hint}#{self.counter}"

    def emit(self, *items):
        self.data.extend(items)

    def emit_imag(self, align, blob, label):
        self.imag += [Align(align), Label(label), blob]

    def link(self):
        labels = {}

        def place(items, pos):
            out = []
            for it in items:
                if isinstance(it, Label):
                    labels[it.name] = pos
                elif isinstance(it, Align):
                    pad = -pos % it.n
                    if pad:
                        out.append((pos, bytes(pad)))
                    pos += pad
                else:
                    out.append((pos, it))
                    pos += _size(it)
            return out, pos

        data, end = place(self.data, 0x14)
        strings = {}
        table = bytearray()
        for _, it in data:
            if isinstance(it, Str) and it.text not in strings:
                strings[it.text] = end + len(table)
                table += it.text.encode("ascii") + b"\0"
        imag_hdr = ((end + len(table) + 8 + 0x7F) & ~0x7F) - 8
        imag, file_end = place(self.imag, imag_hdr + 8)

        out = bytearray(file_end)
        for pos, it in data + imag:
            if isinstance(it, bytes):
                out[pos:pos + len(it)] = it
            elif isinstance(it, F32):
                struct.pack_into("<f", out, pos, it)
            elif isinstance(it, Ptr):
                struct.pack_into("<I", out, pos, (labels[it.label] - pos) & 0xFFFFFFFF if it.label else 0)
            elif isinstance(it, Str):
                struct.pack_into("<I", out, pos, strings[it.text] - pos)
            else:
                struct.pack_into("<I", out, pos, it & 0xFFFFFFFF)
        out[end:end + len(table)] = table
        struct.pack_into("<4sHHIII", out, 0, b"CGFX", 0xFEFF, 0x14, 0x05000000, file_end, 2)
        struct.pack_into("<4sI", out, 0x14, b"DATA", imag_hdr - 0x14)
        struct.pack_into("<4sI", out, imag_hdr, b"IMAG", file_end - imag_hdr)
        return bytes(out)


# --- Dictionaries (patricia trees, as SPICA builds them) ----------------------------

def _bit(name, bit):
    pos = bit >> 3
    return pos < len(name) and (ord(name[pos]) >> (bit & 7)) & 1 == 1


def patricia(names):
    """Nodes [ref bit, left, right, name]; node 0 is the root."""
    nodes = [[0xFFFFFFFF, 0, 0, None]]
    if not names:
        return [[0, 0, 0, None]]
    maxlen = max(len(n) for n in names)

    def traverse(name, bit=0):
        root = 0
        out = nodes[0][1]
        while nodes[root][0] > nodes[out][0] > bit:
            root = out
            out = nodes[out][2] if _bit(name, nodes[out][0]) else nodes[out][1]
        return out, root

    for name in names:
        bit = maxlen * 8 - 1
        idx, _ = traverse(name)
        other = nodes[idx][3] or ""
        while _bit(other, bit) == _bit(name, bit):
            bit -= 1
            if bit < 0:
                raise ValueError("duplicate dictionary key " + name)
        node = [bit, 0, 0, name]
        target, root = traverse(name, bit)
        if _bit(name, bit):
            node[1], node[2] = target, len(nodes)
        else:
            node[1], node[2] = len(nodes), target
        if _bit(name, nodes[root][0]):
            nodes[root][2] = len(nodes)
        else:
            nodes[root][1] = len(nodes)
        nodes.append(node)
    return nodes


def emit_dict(asm, label, entries):
    """entries: [(name, target label)]."""
    nodes = patricia([n for n, _ in entries])
    asm.emit(Label(label), b"DICT", len(nodes) * 0x10 + 0xC, len(entries))
    targets = dict(entries)
    for ref, left, right, name in nodes:
        asm.emit(ref, left | right << 16, Str(name) if name is not None else 0,
                 Ptr(targets[name]) if name is not None else 0)


# --- State hashes -------------------------------------------------------------------

def state_hash(data):
    """SPICA's material hash: MD5 folded into 32 bits. It reproduces the
    template's rasterisation hash; for the fragment operation it gives a
    value that differs from the template's, which is all the runtime needs
    to resend that state."""
    digest = hashlib.md5(data).digest()
    h = 0
    for i, b in enumerate(digest):
        h ^= b << (i & 3) * 8
    return h or 1


def raster_hash(cull_back):
    cull = 2 if cull_back else 0
    return state_hash(struct.pack("<IIIf", 0, cull, 0x00010040, 0.0))


def fragop_hash(depth_write):
    # SPICA GfxFragOp.GetBytes: depth flags, test function (Less = 1), 0,
    # blend mode, blend colour, logic op (copy), six GL blend factors,
    # stencil commands
    flags = 3 if depth_write else 1
    return state_hash(struct.pack("<IIII4fI6I4I", flags, 1, 0, 1, 0, 0, 0, 1, 3,
                                  0x302, 0x303, 0x8006, 0x302, 0x303, 0x8006,
                                  0, 0x000D0105, 0, 0x000F0106))


# --- Objects ------------------------------------------------------------------------

IDENTITY_3X4 = (1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0)


def f32s(*values):
    return [F32(v) for v in values]


def material_color_paths(mat):
    names = ("Emission", "Ambient", "Diffuse", "Specular0", "Specular1",
             "Constant0", "Constant1", "Constant2", "Constant3", "Constant4", "Constant5")
    return [f'Materials["{mat}"].MaterialColor.{n}' for n in names]


def emit_anim_groups(asm, labels, mat):
    """The template's three animation groups, verbatim: they describe what
    animations may drive (the bone, the visibility of the model and mesh 0,
    the plane material's colours and texture state). The banner has no
    animations, so they stay as bannertool has them."""
    sk, vis, mt = labels
    # Skeletal: the bone
    blend, dic, el = asm.new_label("blend"), asm.new_label("dict"), asm.new_label("el")
    asm.emit(Label(sk), 0x80000000, 1, Str("SkeletalAnimation"), 1, 1, Ptr(dic), 1, Ptr(blend), 1)
    asm.emit(Label(blend), 8)
    emit_dict(asm, dic, [("banner", el)])
    asm.emit(Label(el), 0x40000000, Str("banner"), Str("banner"), 0, 0, 0, 0, 0, 0, 0, Str("banner"), 0)
    # Visibility: the model and mesh 0
    blend, dic, e1, e2 = (asm.new_label(h) for h in ("blend", "dict", "el", "el"))
    asm.emit(Label(vis), 0x80000000, 0, Str("VisibilityAnimation"), 3, 2, Ptr(dic), 1, Ptr(blend), 0)
    asm.emit(Label(blend), 0)
    emit_dict(asm, dic, [("IsVisible", e1), ("Meshes[0].IsVisible", e2)])
    asm.emit(Label(e1), 0x10000000, Str("IsVisible"), 0, 0, 0xD4, 1, 0, 6, 1, 0, 6)
    asm.emit(Label(e2), 0x01000000, Str("Meshes[0].IsVisible"), Str("0"), 0, 0x24, 1, 0, 7, 0, 0, 0, 7)
    # Material: colours, sampler, texture, blend colour, texture coordinates
    tc = f'Materials["{mat}"].TextureCoordinators[0].'
    elements = [(p, [0x08000000, None, Str(mat), Str("MaterialColor"), 0x10 * i, 0x10, 0, 1, i, 0, Str(mat), 1])
                for i, p in enumerate(material_color_paths(mat))]
    elements += [
        (f'Materials["{mat}"].TextureMappers[0].Sampler.BorderColor',
         [0x02000000, None, Str(mat), Str("TextureMappers[0].Sampler"), 0x0C, 0x10, 0, 2, 0, 0, Str(mat), 0, 2]),
        (f'Materials["{mat}"].TextureMappers[0].Texture',
         [0x20000000, None, Str(mat), Str("TextureMappers[0]"), 8, 4, 1, 3, 0, 0, Str(mat), 0, 3]),
        (f'Materials["{mat}"].FragmentOperation.BlendOperation.BlendColor',
         [0x04000000, None, Str(mat), Str("FragmentOperation.BlendOperation"), 4, 0x10, 0, 4, 0, 0, Str(mat), 4]),
        (tc + "Scale", [0x80000000, None, Str(mat), Str("TextureCoordinators[0]"), 0x10, 8, 2, 5, 0, 0, Str(mat), 0, 5]),
        (tc + "Rotate", [0x80000000, None, Str(mat), Str("TextureCoordinators[0]"), 0x18, 4, 3, 5, 1, 0, Str(mat), 0, 5]),
        (tc + "Translate", [0x80000000, None, Str(mat), Str("TextureCoordinators[0]"), 0x1C, 8, 2, 5, 2, 0, Str(mat), 0, 5]),
    ]
    blend, dic = asm.new_label("blend"), asm.new_label("dict")
    els = [asm.new_label("el") for _ in elements]
    asm.emit(Label(mt), 0x80000000, 0, Str("MaterialAnimation"), 2, len(elements), Ptr(dic), 4, Ptr(blend), 1)
    asm.emit(Label(blend), 3, 7, 5, 2)
    emit_dict(asm, dic, [(p, l) for (p, _), l in zip(elements, els)])
    for (path, words), l in zip(elements, els):
        words[1] = Str(path)
        asm.emit(Label(l), *words)


def emit_mesh(asm, label, shape_index, material_index, model):
    asm.emit(Label(label), 0x01000000, b"SOBJ", 0, Str(""), 0, 0, shape_index, material_index, Ptr(model),
             0xFFFF0001, 0, 0, *([0] * 12), 0, 0, 0, 0, Str(""), 0, 0, 0)


def emit_material(asm, label, mesh, hashes):
    mapper, texref, sampler, shader, frag, luts = (asm.new_label(h) for h in
                                                   ("mapper", "texref", "sampler", "shader", "frag", "luts"))
    asm.emit(Label(label), 0x08000000, b"MTOB", 0x06000000, Str(mesh.material), 0, 0,
             0, 0, 1)                                       # flags, texcoord config, translucency layer 1
    # Material colours: float RGBA, then the same as bytes, then a command cache
    asm.emit(*f32s(0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0))
    asm.emit(*f32s(*([0, 0, 0, 1] * 6)))
    asm.emit(0, 0x00FFFFFF, 0xFFFFFFFF, 0x00FFFFFF, 0, *([0xFF000000] * 6), 0)
    # Rasterisation: no polygon offset, culling, FACECULLING_CONFIG command
    cull = 1 if mesh.cull_back else 0
    asm.emit(0, cull, F32(0), 2 if mesh.cull_back else 0, 0x00010040)
    # Depth: test (Less) and, for the ship, writes (DEPTH_COLOR_MASK bit 12)
    asm.emit(3 if mesh.depth_write else 1, 0x1041 if mesh.depth_write else 0x41, 0x00010107, 0x03000000, 0x00080126)
    # Blend: separate-free alpha blending, blend colour black
    asm.emit(1, *f32s(0, 0, 0, 1), 0x00E40100, 0x803F0100, 0x76760000, 0, 0xFF000000, 0)
    # Stencil off
    asm.emit(0, 0x000D0105, 0, 0x000F0106)
    # Texture coordinators: one in use (UV set 0, identity transform)
    asm.emit(1)
    asm.emit(0, 0, 0, 0, *f32s(1, 1, 0, 0, 0), 0, *f32s(1, -0.0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0))
    for _ in range(2):
        asm.emit(0, 0, 0, 0, *f32s(0, 0, 0, 0, 0), 0, *f32s(*IDENTITY_3X4))
    asm.emit(Ptr(mapper), 0, 0, 0, Ptr(shader), Ptr(frag), 0, 0, 0, 0, 0)
    asm.emit(*hashes)
    # Texture mapper: TEXUNIT0 commands (type, border colour, parameters;
    # the runtime fills in size and address)
    asm.emit(Label(mapper), 0x80000000, 0, Ptr(texref), Ptr(sampler),
             0, 0x0001008E, 0xFF000000, 0x809F0081, 0, 0x00002206, *([0] * 8), 0x38)
    asm.emit(Label(texref), 0x20000004, b"TXOB", 0x05000000, Str(""), 0, 0, Str(mesh.texture), 0)
    asm.emit(Label(sampler), 0x80000000, Ptr(mapper), 1, *f32s(0, 0, 0, 1), F32(0))
    asm.emit(Label(shader), 0x80000001, b"SHDR", 0x05000000, Str(""), 0, 0, Str("DefaultShader"), 0)
    # Fragment shader: no lighting; stage 0 = primary colour x texture 0
    # (both colour and alpha), stages 1-5 pass the previous result through
    asm.emit(Label(frag), *f32s(0, 0, 0, 1), 0, 0, 0, 0, 0, 0, Ptr(luts))
    asm.emit(0, 0x0E300E30, 0x804F00C0, 0, 0x00010001, 0xFF000000, 0)
    for reg in (0xC8, 0xD0, 0xD8, 0xF0, 0xF8):
        asm.emit(0, 0x0E1F0E1F, 0x804F0000 | reg, 0, 0, 0xFF000000, 0)
    asm.emit(0x10, 0x000F0104)                             # alpha test off
    asm.emit(0xFF000000, 0x000F00FD, 0, 0x000200E0, 0x400, 0x000201C3)
    asm.emit(Label(luts), 0, 0, 0, 0, 0, 0)


def emit_shape(asm, label, mesh):
    """One sub-mesh bound to bone 0 (not skinned: the bone's matrix moves
    it), one face set, one interleaved vertex buffer."""
    pos = mesh.positions
    lo = [min(p[k] for p in pos) for k in range(3)]
    hi = [max(p[k] for p in pos) for k in range(3)]
    center = [(a + b) / 2 for a, b in zip(lo, hi)]
    size = [b - a for a, b in zip(lo, hi)]
    attrs = [(ATTR_POSITION, GL_FLOAT, 3, 1.0, 0)]
    stride = 12
    if mesh.colors is not None:
        attrs.append((ATTR_COLOR, GL_UNSIGNED_BYTE, 4, 1 / 255, stride))
        stride += 4
    attrs.append((ATTR_TEXCOORD0, GL_FLOAT, 2, 1.0, stride))
    stride += 8
    vb = bytearray()
    for i, p in enumerate(pos):
        vb += struct.pack("<3f", *p)
        if mesh.colors is not None:
            vb += bytes(mesh.colors[i])
        vb += struct.pack("<2f", *mesh.uvs[i])
    flat = [i for t in mesh.triangles for i in t]
    if len(pos) <= 256:
        ib, ifmt = bytes(flat), GL_UNSIGNED_BYTE
    else:
        ib, ifmt = struct.pack(f"<{len(flat)}H", *flat), GL_UNSIGNED_SHORT
    sub_tab, vb_tab, bbox, sub, bones, face_tab, face, fd_tab, bufobjs, fd, vbuf, attr_tab, ib_data, vb_data = (
        asm.new_label(h) for h in ("subtab", "vbtab", "bbox", "sub", "bones", "facetab", "face", "fdtab",
                                   "bufobjs", "fd", "vbuf", "attrtab", "ib", "vb"))
    attr_labels = [asm.new_label("attr") for _ in attrs]
    asm.emit(Label(label), 0x10000001, b"SOBJ", 0, Str(""), 0, 0, 0, Ptr(bbox), *f32s(0, 0, 0),
             1, Ptr(sub_tab), 0, 1, Ptr(vb_tab), 0)
    asm.emit(Label(sub_tab), Ptr(sub), Label(vb_tab), Ptr(vbuf))
    asm.emit(Label(bbox), 0x80000000, *f32s(*center), *f32s(1, 0, 0, 0, 1, 0, 0, 0, 1), *f32s(*size))
    asm.emit(Label(sub), 1, Ptr(bones), 0, 1, Ptr(face_tab), Label(bones), 0, Label(face_tab), Ptr(face))
    asm.emit(Label(face), 1, Ptr(fd_tab), 1, Ptr(bufobjs), Label(fd_tab), Ptr(fd), Label(bufobjs), 0)
    asm.emit(Label(fd), ifmt, 0x100, len(ib), Ptr(ib_data), 0, 0, 0, 0, 0, 0, 0)
    asm.emit(Label(vbuf), 0x40000002, 0x15, 2, 0, 0, len(vb), Ptr(vb_data), 0, 0, stride, len(attrs), Ptr(attr_tab))
    asm.emit(Label(attr_tab), *[Ptr(l) for l in attr_labels])
    for (name, fmt, n, scale, off), l in zip(attrs, attr_labels):
        asm.emit(Label(l), 0x40000001, name, 0, 0, 0, 0, 0, 0, 0, fmt, n, F32(scale), off)
    asm.emit_imag(8, ib, ib_data)
    asm.emit_imag(8, bytes(vb), vb_data)


def emit_skeleton(asm, label):
    dic, bone = asm.new_label("dict"), asm.new_label("bone")
    asm.emit(Label(label), 0x02000000, b"SOBJ", 0, Str(""), 0, 0, 1, Ptr(dic), Ptr(bone), 0, 2)
    emit_dict(asm, dic, [("banner", bone)])
    # The bone sits 1 unit up and is a screen-viewpoint billboard (mode 5)
    asm.emit(Label(bone), Str("banner"), 0x1DC, 0, -1, 0, 0, 0, 0,
             *f32s(1, 1, 1, 0, 0, 0, 0, 1, 0),
             *f32s(1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0),
             *f32s(*IDENTITY_3X4),
             *f32s(1, -0.0, 0, 0, -0.0, 1, -0.0, -1, 0, -0.0, 1, 0),
             5, 0, 0)


def tile_rgba4(tex):
    """bannertool's image_data_to_tiles (RGBA4 path)."""
    w, h, src = tex.width, tex.height, tex.rgba
    out = bytearray(w * h * 2)
    for y in range(h):
        for x in range(w):
            i = (((y >> 3) * (w >> 3) + (x >> 3)) << 6) + ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                                                          ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))
            r, g, b, a = src[(y * w + x) * 4:(y * w + x) * 4 + 4]
            struct.pack_into("<H", out, i * 2, (r & 0xF0) << 8 | (g & 0xF0) << 4 | (b & 0xF0) | a >> 4)
    return bytes(out)


def emit_texture(asm, label, tex):
    img, raw = asm.new_label("image"), asm.new_label("pixels")
    data = tile_rgba4(tex)
    asm.emit(Label(label), 0x20000011, b"TXOB", 0x05000000, Str(tex.name), 0, 0, tex.height, tex.width,
             GL_RGBA_NATIVE_DMP, GL_UNSIGNED_SHORT_4_4_4_4, 1, 0, 0, PICA_RGBA4, Ptr(img))
    asm.emit(Label(img), tex.height, tex.width, len(data), Ptr(raw), 0, 16, 0, 0)
    asm.emit_imag(0x80, data, raw)


def build(meshes, textures):
    """meshes[0] is the plane with the bannertool material "mt_banner"; the
    animation groups refer to it."""
    asm = Assembler()
    models_dict, tex_dict, model = "models", "textures", "model"
    mesh_tab, shape_tab, anim_dict, mat_dict, skeleton = "meshtab", "shapetab", "animdict", "matdict", "skeleton"
    mesh_l = [asm.new_label("mesh") for _ in meshes]
    mat_l = [asm.new_label("material") for _ in meshes]
    shape_l = [asm.new_label("shape") for _ in meshes]
    tex_l = [asm.new_label("texture") for _ in textures]
    anim_l = ["anim_skeletal", "anim_visibility", "anim_material"]

    # DATA header: 15 dictionaries, only models and textures used
    asm.emit(b"DATA", 0)                                  # size filled in by link()
    asm.emit(1, Ptr(models_dict), len(textures), Ptr(tex_dict), *([0] * 26))
    emit_dict(asm, models_dict, [("COMMON", model)])
    emit_dict(asm, tex_dict, [(t.name, l) for t, l in zip(textures, tex_l)])
    asm.emit(Label(model), 0x40000092, b"CMDL", 0x07000000, Str("COMMON"), 0, 0, 1, 1, 0, 0, 3, Ptr(anim_dict),
             *f32s(1, 1, 1, 0, 0, 0, 0, 0, 0), *f32s(*IDENTITY_3X4), *f32s(*IDENTITY_3X4),
             len(meshes), Ptr(mesh_tab), len(meshes), Ptr(mat_dict), len(meshes), Ptr(shape_tab),
             0, 0, 1, 0, 0, Ptr(skeleton))
    asm.emit(Label(mesh_tab), *[Ptr(l) for l in mesh_l])
    asm.emit(Label(shape_tab), *[Ptr(l) for l in shape_l])
    emit_dict(asm, anim_dict, list(zip(("SkeletalAnimation", "VisibilityAnimation", "MaterialAnimation"), anim_l)))
    emit_dict(asm, mat_dict, [(m.material, l) for m, l in zip(meshes, mat_l)])
    emit_anim_groups(asm, anim_l, meshes[0].material)
    for i, l in enumerate(mesh_l):
        emit_mesh(asm, l, i, i, model)
    for m, l in zip(meshes, mat_l):
        hashes = list(TEMPLATE_HASHES)
        if not m.cull_back:
            hashes[HASH_RASTER] = raster_hash(False)
        if m.depth_write:
            hashes[HASH_FRAGOP] = fragop_hash(True)
        emit_material(asm, l, m, hashes)
    for m, l in zip(meshes, shape_l):
        emit_shape(asm, l, m)
    emit_skeleton(asm, skeleton)
    for t, l in zip(textures, tex_l):
        emit_texture(asm, l, t)
    return asm.link()


# The bannertool plane: 26 x 13 units, the banner image on it
PLANE_POSITIONS = [(-13.0, -7.5, 0.0), (13.0, -7.5, 0.0), (-13.0, 5.5, 0.0), (13.0, 5.5, 0.0)]
PLANE_UVS = [(0.0, 0.0), (1.0, 0.0), (0.0, 1.0), (1.0, 1.0)]
PLANE_TRIANGLES = [(0, 1, 2), (1, 3, 2)]


def plane_mesh():
    return Mesh("mt_banner", "COMMON1", PLANE_POSITIONS, PLANE_UVS, PLANE_TRIANGLES)


def check_template(template):
    """template: bannertool's BANNER_CGFX_HEADER plus its RGBA4 pixels (the
    CGFX inside a flat .bnr). Returns True if build() reproduces it."""
    w, h = 256, 128
    data = template[0x1580:0x1580 + w * h * 2]
    # Untile back to RGBA (exact: the RGBA4 values come back unchanged)
    rgba = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            i = (((y >> 3) * (w >> 3) + (x >> 3)) << 6) + ((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
                                                          ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3))
            v = struct.unpack_from("<H", data, i * 2)[0]
            rgba[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(((v >> 12) * 17, (v >> 8 & 15) * 17,
                                                               (v >> 4 & 15) * 17, (v & 15) * 17))
    return build([plane_mesh()], [Texture("COMMON1", w, h, bytes(rgba))]) == template
