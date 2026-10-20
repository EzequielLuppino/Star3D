"""Build-time conversion of the attract movie's audio for the 3DS
(GRAPHICS_PLAN.md section 7.2, used by extract_disc.py).

The THP audio component is DSP-ADPCM, but every frame carries its own
coefficients (starfox.thp uses 39 sets per channel), and an ndsp channel holds
one set. The track is decoded here and re-encoded with one fixed set per
channel, chosen from the movie's own sets by how well they predict the track.
At runtime (port/movie_audio_3ds.c) the DSP plays it on two channels, in step
with the game's movie clock; THPAudioDecode only returns silence of the right
length, so the game's mixer and timing are unchanged.

.a3d layout (little-endian):
  0x00 "A3D1"  0x04 header size  0x08 sample rate  0x0C frame count
  0x10 samples per channel  0x14 channels (2)  0x18 frame table offset
  0x1C seek table offset  0x20 blocks per seek entry  0x24 seek entries
  0x28 data offset  0x2C bytes per channel  0x30 coefficients, 16 s16 per channel
  frame table: per THP frame, u32 hash (FNV-1a of the first 144 bytes of the
  frame's audio component, see frame_hash) and u32 first sample
  seek table: per entry, s16 yn1, yn2 for each channel (decoder history before
  block entry * blocks per seek entry)
  data: the left channel's ADPCM blocks, then the right channel's
"""
import math
import os
import struct
import sys

A3D_MAGIC = b"A3D1"
A3D_HEADER_SIZE = 0x30 + 2 * 32
SEEK_BLOCKS = 16
HASH_BYTES = 144
RATE = 32000

_here = os.path.dirname(os.path.abspath(__file__))
if "video_convert" in sys.modules:
    video_convert = sys.modules["video_convert"]
else:
    import importlib.util
    _spec = importlib.util.spec_from_file_location("video_convert", os.path.join(_here, "video_convert.py"))
    video_convert = importlib.util.module_from_spec(_spec)
    sys.modules["video_convert"] = video_convert
    _spec.loader.exec_module(video_convert)


def _clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def frame_hash(audio):
    """Must match frame_hash() in port/movie_audio_3ds.c."""
    h = 2166136261
    for b in audio[:HASH_BYTES]:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def audio_frames(data, hdr):
    """Yield the audio component of every frame (None if a frame has none)."""
    off, size = hdr["movie_off"], hdr["first_size"]
    comps = hdr["components"]
    for _ in range(hdr["frames"]):
        next_size = struct.unpack_from(">I", data, off)[0]
        sizes = struct.unpack_from(">%dI" % len(comps), data, off + 8)
        p = off + 8 + 4 * len(comps)
        audio = None
        for kind, n in zip(comps, sizes):
            if kind == 1 and audio is None:
                audio = data[p:p + n]
            p += n
        yield audio
        off += size
        size = next_size


def _decode_channel(data, start, coefs, count, yn1, yn2, out):
    for block in range((count + 13) // 14):
        base = start + block * 8
        header = data[base]
        c1 = coefs[(header >> 4 & 7) * 2]
        c2 = coefs[(header >> 4 & 7) * 2 + 1]
        scale = (1 << (header & 15)) * 2048
        for i in range(min(14, count - block * 14)):
            byte = data[base + 1 + (i >> 1)]
            n = (byte & 15) if i & 1 else (byte >> 4)
            if n >= 8:
                n -= 16
            s = _clamp16((n * scale + c1 * yn1 + c2 * yn2 + 1024) >> 11)
            yn2, yn1 = yn1, s
            out.append(s)


def decode_track(data, hdr):
    """Returns (left, right, coef sets {tuple: count} per channel, frame list (hash, first sample))."""
    pcm = ([], [])
    sets = ({}, {})
    frames = []
    for audio in audio_frames(data, hdr):
        if audio is None or len(audio) < 80:
            frames.append((0, len(pcm[0])))
            continue
        frames.append((frame_hash(audio), len(pcm[0])))
        next_off, count = struct.unpack_from(">II", audio, 0)
        for ch in range(2):
            coefs = struct.unpack_from(">16h", audio, 8 + 32 * ch)
            yn1, yn2 = struct.unpack_from(">hh", audio, 72 + 4 * ch)
            sets[ch][coefs] = sets[ch].get(coefs, 0) + count
            start = 80 + (next_off if ch else 0)
            _decode_channel(audio, start, coefs, count, yn1, yn2, pcm[ch])
    return pcm[0], pcm[1], sets, frames


def _block_cost(target, h1, h2, coefs):
    """Open-loop choice: (predictor, scale, cost) with the smallest peak residual."""
    best = None
    for p in range(8):
        c1, c2 = coefs[2 * p], coefs[2 * p + 1]
        a, b = h1, h2
        peak = 0
        for t in target:
            r = t * 2048 - (c1 * a + c2 * b)
            if r < 0:
                r = -r
            if r > peak:
                peak = r
            b, a = a, t
        if best is None or peak < best[0]:
            best = (peak, p)
    peak, p = best
    scale = 0
    while scale < 12 and peak > 7 * 2048 * (1 << scale):
        scale += 1
    return p, scale, math.log2(1 + peak / 2048)


def score_coefs(pcm, coefs, stride=37):
    """Summed log2 peak residual over every stride-th block (lower is better)."""
    total = 0.0
    for block in range(1, len(pcm) // 14, stride):
        s = block * 14
        total += _block_cost(pcm[s:s + 14], pcm[s - 1], pcm[s - 2], coefs)[2]
    return total


def _encode_block(target, c1, c2, step, yn1, yn2):
    out = []
    err = 0
    h1, h2 = yn1, yn2
    for t in target:
        p = c1 * h1 + c2 * h2 + 1024
        n = (t * 2048 - p) / step
        n = int(n + 0.5) if n >= 0 else -int(-n + 0.5)
        n = -8 if n < -8 else 7 if n > 7 else n
        s = _clamp16((n * step + p) >> 11)
        err += (s - t) * (s - t)
        out.append(n)
        h2, h1 = h1, s
    return err, out, h1, h2


def encode_channel(args):
    """(pcm, coefs) -> (ADPCM bytes, seek history [(yn1, yn2)], SNR in dB)."""
    pcm, coefs = args
    blocks = (len(pcm) + 13) // 14
    pcm = pcm + [0] * (blocks * 14 - len(pcm))
    out = bytearray(blocks * 8)
    seek = []
    yn1 = yn2 = 0
    sig = noise = 0
    for block in range(blocks):
        if block % SEEK_BLOCKS == 0:
            seek.append((yn1, yn2))
        target = pcm[block * 14:block * 14 + 14]
        p, scale, _ = _block_cost(target, yn1, yn2, coefs)
        c1, c2 = coefs[2 * p], coefs[2 * p + 1]
        best = None
        for s in (scale, scale + 1) if scale < 12 else (scale,):
            r = _encode_block(target, c1, c2, (1 << s) * 2048, yn1, yn2)
            if best is None or r[0] < best[0][0]:
                best = (r, s)
        (err, nibbles, yn1, yn2), s = best
        base = block * 8
        out[base] = (p << 4) | s
        for i in range(0, 14, 2):
            out[base + 1 + i // 2] = ((nibbles[i] & 15) << 4) | (nibbles[i + 1] & 15)
        noise += err
        sig += sum(t * t for t in target)
    snr = 10 * math.log10(sig / noise) if noise else 99.0
    return bytes(out), seek, snr


def _pick_coefs(pcm, sets):
    candidates = sorted(sets, key=lambda c: -sets[c])
    return min(candidates, key=lambda c: score_coefs(pcm, c))


def convert_thp_audio(path, data, log=None):
    """Return {path of the .a3d: bytes} for one .thp file ({} if it has no audio)."""
    log = log or (lambda m: print("movie_audio_convert: " + m, flush=True))
    try:
        hdr = video_convert.parse_thp(data)
    except (ValueError, struct.error) as e:
        log(f"{path}: {e}, movie audio left unconverted")
        return {}
    if 1 not in hdr["components"]:
        return {}
    try:
        left, right, sets, frames = decode_track(data, hdr)
    except (struct.error, IndexError) as e:
        log(f"{path}: cannot decode the audio track ({e}), movie audio left unconverted")
        return {}
    if not left:
        return {}
    coefs = [_pick_coefs(left, sets[0]), _pick_coefs(right, sets[1])]
    encoded = [encode_channel((left, coefs[0])), encode_channel((right, coefs[1]))]
    channel_bytes = len(encoded[0][0])
    seek_count = len(encoded[0][1])

    frame_off = A3D_HEADER_SIZE
    seek_off = frame_off + 8 * len(frames)
    data_off = (seek_off + 8 * seek_count + 127) & ~127
    header = struct.pack("<4sIIIIIIIIIII", A3D_MAGIC, A3D_HEADER_SIZE, RATE, len(frames), len(left), 2,
                         frame_off, seek_off, SEEK_BLOCKS, seek_count, data_off, channel_bytes)
    header += struct.pack("<16h", *coefs[0]) + struct.pack("<16h", *coefs[1])
    header += b"".join(struct.pack("<II", h, s) for h, s in frames)
    header += b"".join(struct.pack("<hhhh", *l, *r) for l, r in zip(encoded[0][1], encoded[1][1]))
    header += bytes(data_off - len(header))
    out_path = os.path.splitext(path)[0] + ".a3d"
    log(f"{path}: {len(left) / RATE:.1f} s stereo, {len(sets[0])}/{len(sets[1])} coefficient sets -> {out_path} "
        f"({2 * channel_bytes // 1024} KB DSP-ADPCM, SNR {encoded[0][2]:.1f}/{encoded[1][2]:.1f} dB)")
    return {out_path: header + encoded[0][0] + encoded[1][0]}


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: movie_audio_convert.py movie.thp out.a3d")
    with open(sys.argv[1], "rb") as f:
        result = convert_thp_audio(sys.argv[1], f.read())
    if not result:
        sys.exit(1)
    with open(sys.argv[2], "wb") as f:
        f.write(next(iter(result.values())))
