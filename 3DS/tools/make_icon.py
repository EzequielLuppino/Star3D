"""Make the 48x48 Homebrew Launcher icon from the disc's own banner.

    python -I tools/make_icon.py <romfs_dir>/files/opening.bnr <icon.png>

GameCube banners (opening.bnr, "BNR1"/"BNR2") hold a 96x32 RGB5A3 image in 4x4
tiles at offset 0x20. It is downscaled 2x to 48x16 and centred on a
transparent 48x48 canvas. Uses only the standard library.
"""
import struct
import sys
import zlib

W, H = 96, 32


def rgb5a3(v):
    if v & 0x8000:
        r, g, b = (v >> 10) & 0x1F, (v >> 5) & 0x1F, v & 0x1F
        return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255)
    a, r, g, b = (v >> 12) & 0x7, (v >> 8) & 0xF, (v >> 4) & 0xF, v & 0xF
    return (r * 17, g * 17, b * 17, a * 255 // 7)


def decode_banner(data):
    if data[:3] != b"BNR":
        sys.exit("make_icon: not a GameCube banner file")
    pixels = [[(0, 0, 0, 0)] * W for _ in range(H)]
    pos = 0x20
    for ty in range(0, H, 4):
        for tx in range(0, W, 4):
            for y in range(4):
                for x in range(4):
                    v = struct.unpack_from(">H", data, pos)[0]
                    pos += 2
                    pixels[ty + y][tx + x] = rgb5a3(v)
    return pixels


def write_png(path, rows):
    raw = b"".join(b"\0" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", len(rows[0]), len(rows), 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: make_icon.py <opening.bnr> <icon.png>")
    with open(sys.argv[1], "rb") as f:
        banner = decode_banner(f.read())

    icon = [[(0, 0, 0, 0)] * 48 for _ in range(48)]
    top = (48 - H // 2) // 2
    for y in range(H // 2):
        for x in range(W // 2):
            block = [banner[2 * y + dy][2 * x + dx] for dy in (0, 1) for dx in (0, 1)]
            icon[top + y][x] = tuple(sum(px[c] for px in block) // 4 for c in range(4))
    write_png(sys.argv[2], icon)


if __name__ == "__main__":
    main()
