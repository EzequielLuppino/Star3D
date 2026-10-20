"""Decode the textures a capture saved (tex_s<state>_u<unit>.bin, S3D_CAP_TEXDUMP).

    python -I tools/pica_tex_dump.py <capture folder or .bin files> [--scale N]

Each file is "S3DT", width, height, PICA format (u16 little-endian), then level
0 in PICA 8x8 tiles (Morton order inside a tile). Writes next to each file:
<name>.png (RGBA), <name>_rgb.png (colour, alpha ignored) and <name>_a.png
(alpha as grey). Rows are written as stored; the converter may store images
upside down, so compare orientation with care. ETC1 is not decoded.
"""
import glob
import os
import struct
import sys

from PIL import Image

FORMATS = ["RGBA8", "RGB8", "RGBA5551", "RGB565", "RGBA4", "LA8", "HILO8", "L8", "A8", "LA4", "L4", "A4",
           "ETC1", "ETC1A4"]
BITS = [32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8]


def morton(x, y):
    m = 0
    for b in range(3):
        m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1)
    return m


def texel(data, fmt, i):
    if fmt == 0:
        a, b, g, r = data[i * 4:i * 4 + 4]
        return r, g, b, a
    if fmt == 1:
        b, g, r = data[i * 3:i * 3 + 3]
        return r, g, b, 255
    if fmt in (2, 3, 4, 5, 6):
        v = data[i * 2] | data[i * 2 + 1] << 8
        if fmt == 2:
            return (v >> 11) << 3, ((v >> 6) & 31) << 3, ((v >> 1) & 31) << 3, 255 * (v & 1)
        if fmt == 3:
            return (v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3, 255
        if fmt == 4:
            return (v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17
        if fmt == 5:
            return v >> 8, v >> 8, v >> 8, v & 255
        return v & 255, v >> 8, 0, 255
    if fmt in (7, 8, 9):
        v = data[i]
        if fmt == 7:
            return v, v, v, 255
        if fmt == 8:
            return 0, 0, 0, v
        return (v >> 4) * 17, (v >> 4) * 17, (v >> 4) * 17, (v & 15) * 17
    if fmt in (10, 11):
        v = (data[i >> 1] >> (4 * (i & 1))) & 15
        return (v * 17, v * 17, v * 17, 255) if fmt == 10 else (0, 0, 0, v * 17)
    raise ValueError("ETC1 not decoded")


def decode(path, scale):
    raw = open(path, "rb").read()
    if raw[:4] != b"S3DT":
        print(path, "not a texture dump")
        return
    w, h, fmt = struct.unpack("<3H", raw[4:10])
    data = raw[10:]
    name = FORMATS[fmt] if fmt < len(FORMATS) else str(fmt)
    if fmt >= 12:
        print(f"{path}: {w}x{h} {name} (not decoded)")
        return
    img = Image.new("RGBA", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            i = ((y >> 3) * (w >> 3) + (x >> 3)) * 64 + morton(x, y)
            px[x, y] = texel(data, fmt, i)
    alphas = [px[x, y][3] for y in range(h) for x in range(w)]
    print(f"{path}: {w}x{h} {name}, alpha {min(alphas)}..{max(alphas)}")
    base = os.path.splitext(path)[0]
    big = (w * scale, h * scale)
    img.resize(big, Image.NEAREST).save(base + ".png")
    img.convert("RGB").resize(big, Image.NEAREST).save(base + "_rgb.png")
    img.getchannel("A").resize(big, Image.NEAREST).save(base + "_a.png")


def main():
    args = sys.argv[1:]
    scale = 4
    if "--scale" in args:
        k = args.index("--scale")
        scale = int(args[k + 1])
        del args[k:k + 2]
    files = []
    for a in args:
        files += sorted(glob.glob(os.path.join(a, "tex_s*_u*.bin"))) if os.path.isdir(a) else [a]
    if not files:
        print(__doc__)
        return
    for f in files:
        decode(f, scale)


if __name__ == "__main__":
    main()
