"""Build-time conversion of the attract movie for the 3DS (GRAPHICS_PLAN.md
section 7, used by extract_disc.py).

The game's movie (starfox.thp) is MJPEG video plus THP-ADPCM audio. Decoding
a 512x384 JPEG per frame costs more than the 3DS CPU can spare, so every frame
is decoded here (Pillow) and stored as a PICA ETC1 texture the GPU samples
directly: <name>.v3d next to the .thp. At runtime (port/movie_3ds.c) a frame
is one 64 KB read into linear memory, with no decoding at all.

The .thp stays in the RomFS unchanged: the game still opens it, reads its
frames and decodes the (cheap) audio component from it; only the video
component is replaced.

THP frames are baseline JPEGs whose entropy-coded data is not byte-stuffed
(the GameCube decoder reads raw bits), so 0x00 is inserted after every 0xFF
in the scan before handing a frame to Pillow.

.v3d layout (little-endian):
  0x00 "V3D1"   0x04 header size (64)   0x08 frame count   0x0C fps (float)
  0x10 width    0x14 height  (stored texture size)          0x18 GPU_TEXCOLOR
  0x1C frame size in bytes   0x20 source width   0x24 source height
  0x28 frames that failed to decode (repeat the previous frame)
  0x2C offset of the frame hash table   0x30 offset of the first frame
  hash table: u32 per frame, FNV-1a of the first 64 bytes of the frame's JPEG
  scan data (frame_hash; the port identifies the frame THPVideoDecode is
  given with it, so the game's player needs no changes)
  frames: frame i at first frame offset + i * frame size (128-byte aligned)
"""
import ctypes
import io
import multiprocessing
import os
import struct
import sys

V3D_MAGIC = b"V3D1"
V3D_HEADER_SIZE = 64
FRAME_W, FRAME_H = 512, 256  # 4:3 movie stretched into one texture; the quad maps it back
GPU_ETC1 = 0xC
FRAME_BYTES = FRAME_W * FRAME_H // 2

try:
    from PIL import Image
except ImportError:  # Local builds without Pillow keep the black-frame fallback
    Image = None


def _lib_path():
    lib = os.environ.get("TEXCONV_LIB", "")
    return lib if lib and os.path.isfile(lib) else None


def available():
    """Video conversion needs Pillow (JPEG decode) and the texconv library (ETC1)."""
    return Image is not None and _lib_path() is not None


def parse_thp(data):
    if data[:4] != b"THP\0":
        raise ValueError("not a THP file")
    (version, max_buffer, max_audio, fps, num_frames, first_size, movie_size, comp_off, offsets_off,
     movie_off, final_off) = struct.unpack_from(">IIIfIIIIIII", data, 4)
    num_comp = struct.unpack_from(">I", data, comp_off)[0]
    comps = list(data[comp_off + 4:comp_off + 4 + num_comp])
    info = comp_off + 4 + 16
    width = height = 0
    for c in comps:
        if c == 0:
            width, height = struct.unpack_from(">II", data, info)
            info += 12 if version >= 0x11000 else 8
        elif c == 1:
            info += 16 if version >= 0x11000 else 12
    return {"fps": fps, "frames": num_frames, "first_size": first_size, "movie_off": movie_off,
            "components": comps, "width": width, "height": height}


def video_frames(data, hdr):
    """Yield the JPEG data of every frame's video component."""
    off, size = hdr["movie_off"], hdr["first_size"]
    comps = hdr["components"]
    for _ in range(hdr["frames"]):
        next_size = struct.unpack_from(">I", data, off)[0]
        sizes = struct.unpack_from(">%dI" % len(comps), data, off + 8)
        p = off + 8 + 4 * len(comps)
        jpeg = None
        for kind, n in zip(comps, sizes):
            if kind == 0:
                jpeg = data[p:p + n]
            p += n
        yield jpeg
        off += size
        size = next_size


def scan_offset(jpeg):
    """Offset of the entropy-coded data (after the SOS segment)."""
    pos = 2
    while pos + 4 <= len(jpeg):
        if jpeg[pos] != 0xFF:
            raise ValueError("bad JPEG marker")
        marker = jpeg[pos + 1]
        seg_len = struct.unpack_from(">H", jpeg, pos + 2)[0]
        if marker == 0xDA:
            return pos + 2 + seg_len
        pos += 2 + seg_len
    raise ValueError("no scan in JPEG")


def frame_hash(jpeg):
    """Must match frame_hash() in port/movie_3ds.c."""
    h = 2166136261
    scan = scan_offset(jpeg)
    for b in jpeg[scan:scan + 64]:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def stuff_jpeg(jpeg):
    """Return a standard JPEG: byte-stuff the scan data and make sure it ends with EOI."""
    scan = scan_offset(jpeg)
    end = jpeg.rfind(b"\xff\xd9")
    if end < scan:
        end = len(jpeg)
        while end > scan and jpeg[end - 1] == 0:  # Padding after the scan
            end -= 1
    body = jpeg[scan:end].replace(b"\xff", b"\xff\x00")
    return jpeg[:scan] + body + b"\xff\xd9"


_lib = None


def _worker_init(lib_path):
    global _lib
    _lib = ctypes.CDLL(lib_path)
    _lib.tc_encode_frame_etc1.restype = ctypes.c_double
    _lib.tc_encode_frame_etc1.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
                                          ctypes.c_uint32, ctypes.c_char_p]


def _encode_frame(jpeg):
    """JPEG bytes -> (ETC1 frame bytes or None, PSNR, hash)."""
    if jpeg is None:
        return None, 0.0, 0
    try:
        h = frame_hash(jpeg)
    except (ValueError, struct.error):
        h = 0
    try:
        img = Image.open(io.BytesIO(stuff_jpeg(jpeg)))
        img = img.convert("RGBA")
    except Exception:
        return None, 0.0, h
    out = ctypes.create_string_buffer(FRAME_BYTES)
    psnr = _lib.tc_encode_frame_etc1(img.tobytes(), img.width, img.height, FRAME_W, FRAME_H, out)
    if psnr < 0:
        return None, 0.0, h
    return out.raw, psnr, h


def convert_thp(path, data, log=None):
    """Return {path of the .v3d: bytes} for one .thp file ({} if it cannot be converted)."""
    log = log or (lambda m: print("video_convert: " + m, flush=True))
    if not available():
        return {}
    try:
        hdr = parse_thp(data)
    except (ValueError, struct.error) as e:
        log(f"{path}: {e}, movie left unconverted")
        return {}
    if 0 not in hdr["components"]:
        log(f"{path}: no video component")
        return {}

    jobs = max(1, (os.cpu_count() or 1))
    with multiprocessing.Pool(jobs, _worker_init, (_lib_path(),)) as pool:
        results = pool.map(_encode_frame, video_frames(data, hdr), chunksize=8)

    black = bytes(FRAME_BYTES)  # All-zero ETC1 blocks decode to black
    frames, hashes, failed, worst, prev = [], [], 0, 99.0, black
    for frame, psnr, h in results:
        hashes.append(h)
        if frame is None:
            failed += 1
            frame = prev
        else:
            worst = min(worst, psnr)
        frames.append(frame)
        prev = frame

    table_off = V3D_HEADER_SIZE
    frames_off = (table_off + 4 * len(frames) + 127) & ~127
    header = struct.pack("<4sIIfIIIIIIIII", V3D_MAGIC, V3D_HEADER_SIZE, len(frames), hdr["fps"], FRAME_W, FRAME_H,
                         GPU_ETC1, FRAME_BYTES, hdr["width"], hdr["height"], failed, table_off, frames_off)
    header += bytes(V3D_HEADER_SIZE - len(header))
    header += struct.pack("<%dI" % len(hashes), *hashes)
    header += bytes(frames_off - len(header))
    out_path = os.path.splitext(path)[0] + ".v3d"
    log(f"{path}: {len(frames)} frames {hdr['width']}x{hdr['height']} @ {hdr['fps']:.2f} fps -> {out_path} "
        f"({FRAME_W}x{FRAME_H} ETC1, {len(frames) * FRAME_BYTES // (1024 * 1024)} MB, worst PSNR {worst:.1f} dB, "
        f"{failed} frames failed to decode)")
    if failed == len(frames):
        log(f"{path}: no frame decoded, movie left unconverted")
        return {}
    return {out_path: header + b"".join(frames)}


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: video_convert.py movie.thp out.v3d   (needs TEXCONV_LIB and Pillow)")
    with open(sys.argv[1], "rb") as f:
        result = convert_thp(sys.argv[1], f.read())
    if not result:
        sys.exit(1)
    with open(sys.argv[2], "wb") as f:
        f.write(next(iter(result.values())))
