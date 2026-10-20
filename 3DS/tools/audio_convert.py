"""Build-time audio conversion for the 3DS DSP (used by extract_disc.py).

The 3DS DSP decodes GameCube DSP-ADPCM in hardware, so audio is stored in that
format and played without any CPU decoding:

* Streams (/streams/*.adp, GameCube DTK/"ADP" ADPCM, 48 kHz stereo) are
  transcoded to DSP-ADPCM. DTK's four fixed prediction filters are exactly
  representable as DSP-ADPCM coefficients, so every DTK block maps onto two
  DSP-ADPCM frames per channel without re-quantizing; only the decoder's
  history precision differs (16 instead of 22 bits). The file keeps its size:
  it is a sequence of chunks of STREAM_CHUNK_BYTES (the last one shorter),
  each holding the left channel's frames followed by the right channel's, so
  one read gives two buffers the DSP can play directly. port/audio_3ds.c reads
  this layout; the coefficients are STREAM_COEFS (fixed, so no header).

* MusyX sample banks (/audio/*.sdi + *.sam) are already DSP-ADPCM, but the DSP
  can only start a buffer at a 14-sample frame boundary and most loops start
  mid-frame. Such loops are moved forward to the next frame boundary (the loop
  body is periodic, so this does not change what is heard): the frames at the
  loop end are re-encoded to continue the waveform into the loop's first
  samples, and the loop start/length, sample length, loop predictor and loop
  history in the sample directory are updated. port/musyx_ndsp.c then plays a
  sample as one buffer up to the loop start plus one looping buffer.
"""
import struct

ADPCM_FRAME_SAMPLES = 14
ADPCM_FRAME_BYTES = 8

# --- DTK streams -------------------------------------------------------------

DTK_BLOCK_BYTES = 32  # 4 header bytes + 28 bytes of (left low, right high) nibbles
DTK_BLOCK_FRAMES = 28
STREAM_CHUNK_BYTES = 16384  # Must match port/audio_3ds.c
# DTK filters 0-3 (coefficients/64) as DSP-ADPCM coefficients (/2048);
# DTK treats predictor values 4-15 like filter 0
STREAM_COEFS = [0, 0, 1920, 0, 3680, -1664, 3136, -1760, 0, 0, 0, 0, 0, 0, 0, 0]

_LOW = bytes(b & 15 for b in range(256))
_HIGH = bytes(b >> 4 for b in range(256))
_LOW_TO_HIGH = bytes((b & 15) << 4 for b in range(256))
_HIGH_TO_HIGH = bytes(b & 0xF0 for b in range(256))
# DTK header (predictor << 4 | shift) to DSP-ADPCM header (predictor << 4 | scale).
# A DTK nibble is worth n << (12 - shift); shifts above 12 (fractional steps)
# do not occur in this game's data and are clamped.
_DTK_HEADER = bytes((min(b >> 4, 4) << 4) | max(0, 12 - (b & 15)) for b in range(256))


def _or_bytes(a, b):
    n = len(a)
    return (int.from_bytes(a, "little") | int.from_bytes(b, "little")).to_bytes(n, "little")


def _dtk_channel_frames(data, nibble_hi, nibble_lo, header_offset):
    """DSP-ADPCM frames (16 bytes per DTK block) for one channel of a DTK file."""
    count = len(data) // DTK_BLOCK_BYTES
    out = bytearray(count * 16)
    header = data[header_offset::DTK_BLOCK_BYTES].translate(_DTK_HEADER)
    out[0::16] = header
    out[8::16] = header
    for frame in range(2):
        for j in range(7):
            sample = frame * 14 + j * 2
            hi = data[4 + sample::DTK_BLOCK_BYTES].translate(nibble_hi)
            lo = data[5 + sample::DTK_BLOCK_BYTES].translate(nibble_lo)
            out[frame * 8 + 1 + j::16] = _or_bytes(hi, lo)
    return out


def convert_dtk_stream(data):
    if len(data) % DTK_BLOCK_BYTES:
        raise ValueError("DTK stream length is not a multiple of 32")
    left = _dtk_channel_frames(data, _LOW_TO_HIGH, _LOW, 0)
    right = _dtk_channel_frames(data, _HIGH_TO_HIGH, _HIGH, 1)
    half = STREAM_CHUNK_BYTES // 2
    out = bytearray()
    for start in range(0, len(left), half):
        out += left[start:start + half]
        out += right[start:start + half]
    assert len(out) == len(data)
    return bytes(out)


# --- DSP-ADPCM helpers ---------------------------------------------------------


def _clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def adpcm_decode(data, coefs, count, yn1=0, yn2=0):
    """Decode count samples of DSP-ADPCM (coefs: 16 values). Returns a list."""
    out = []
    for frame in range((count + 13) // 14):
        base = frame * 8
        header = data[base]
        c1 = coefs[(header >> 4 & 7) * 2]
        c2 = coefs[(header >> 4 & 7) * 2 + 1]
        scale = (1 << (header & 15)) * 2048
        for i in range(min(14, count - frame * 14)):
            byte = data[base + 1 + (i >> 1)]
            n = (byte & 15) if i & 1 else (byte >> 4)
            if n >= 8:
                n -= 16
            s = _clamp16((n * scale + c1 * yn1 + c2 * yn2 + 1024) >> 11)
            yn2, yn1 = yn1, s
            out.append(s)
    return out


def _greedy_error(target, c1, c2, step, yn1, yn2):
    h1, h2 = yn1, yn2
    err = 0
    for t in target:
        p = c1 * h1 + c2 * h2 + 1024
        n = (t * 2048 - p) / step
        n = int(n + 0.5) if n >= 0 else -int(-n + 0.5)
        n = -8 if n < -8 else 7 if n > 7 else n
        s = _clamp16((n * step + p) >> 11)
        err += (s - t) * (s - t)
        h2, h1 = h1, s
    return err


def _beam_encode(target, c1, c2, step, yn1, yn2, width=24):
    """Search nibble sequences (rounding each sample up or down) keeping the
    best partial sequences: rounding greedily lets errors compound through
    the predictor. Returns (err, nibbles, decoded)."""
    beam = [(0, yn1, yn2, ())]
    for t in target:
        grown = []
        for err, h1, h2, seq in beam:
            p = c1 * h1 + c2 * h2 + 1024
            ideal = (t * 2048 - p) / step
            low = int(ideal // 1)
            for n in {max(-8, min(7, low)), max(-8, min(7, low + 1))}:
                s = _clamp16((n * step + p) >> 11)
                grown.append((err + (s - t) * (s - t), s, h1, seq + ((n, s),)))
        grown.sort(key=lambda state: state[0])
        beam = grown[:width]
    err, _, _, seq = beam[0]
    return err, [n & 15 for n, _ in seq], [s for _, s in seq]


def _encode_frame(target, coefs, yn1, yn2):
    """Closed-loop DSP-ADPCM encode of up to 14 samples. Returns (bytes, yn1, yn2, decoded)."""
    candidates = []
    for pred in range(8):
        c1, c2 = coefs[pred * 2], coefs[pred * 2 + 1]
        for scale in range(13):
            step = (1 << scale) * 2048
            candidates.append((_greedy_error(target, c1, c2, step, yn1, yn2), pred, scale))
    candidates.sort()
    best = None
    for _, pred, scale in candidates[:8]:
        err, nibbles, decoded = _beam_encode(target, coefs[pred * 2], coefs[pred * 2 + 1],
                                             (1 << scale) * 2048, yn1, yn2)
        if not best or err < best[0]:
            best = (err, pred, scale, nibbles, decoded[-1], decoded[-2] if len(decoded) > 1 else yn1, decoded)
    err, pred, scale, nibbles, h1, h2, decoded = best
    nibbles += [0] * (14 - len(nibbles))
    frame = bytes([pred << 4 | scale] + [nibbles[i] << 4 | nibbles[i + 1] for i in range(0, 14, 2)])
    return frame, h1, h2, decoded


# --- MusyX sample banks -----------------------------------------------------------

SDIR_ENTRY = struct.Struct(">HHIIIIIII")  # id, ref, offset, addr, info, length, loop, loopLength, extra


def _adpcm_bytes(length):
    return (length + 13) // 14 * 8


def _align_loop(sample, coefs, length, loop, loop_len):
    """Re-encode the end of a sample so its loop can start at the next frame
    boundary. Returns (data, length, loop, loopPS, loopY0, loopY1)."""
    new_loop = (loop + 13) // 14 * 14
    loop_end = loop + loop_len
    new_end = new_loop + loop_len
    dec = adpcm_decode(sample, coefs, loop_end)

    # The looped signal is periodic: past the loop end it repeats the loop body
    frames_end = (new_end + 13) // 14 * 14
    target = dec[:loop_end]
    for i in range(loop_end, frames_end):
        target.append(target[i - loop_len])

    start = loop_end // 14 * 14
    if start < new_loop:
        start = new_loop - 14
    out = bytearray(sample[:start // 14 * 8])
    decoded = dec[:start]
    yn1 = dec[start - 1] if start >= 1 else 0
    yn2 = dec[start - 2] if start >= 2 else 0
    for frame_start in range(start, frames_end, 14):
        frame, yn1, yn2, frame_dec = _encode_frame(target[frame_start:frame_start + 14], coefs, yn1, yn2)
        out += frame
        decoded += frame_dec

    loop_ps = out[new_loop // 14 * 8]
    loop_y1, loop_y0 = decoded[new_loop - 1], decoded[new_loop - 2]

    # Check: the start up to the loop, then the loop twice (history reloaded
    # at each pass, as the DSP does for a looping buffer)
    head = adpcm_decode(out, coefs, new_loop)
    body = adpcm_decode(out[new_loop // 14 * 8:], coefs, loop_len, loop_y1, loop_y0)
    played = head + body + body
    worst = max(abs(played[i] - target[i] if i < len(target) else played[i] - target[loop + (i - loop) % loop_len])
                for i in range(len(played)))
    return bytes(out), new_end, new_loop, loop_ps, loop_y0, loop_y1, worst


def convert_sample_bank(sdi, sam):
    """Align MusyX sample loops to ADPCM frames. Returns (sdi, sam, stats)."""
    sdi = bytearray(sdi)
    entries = []
    while True:
        entry = SDIR_ENTRY.unpack_from(sdi, len(entries) * 32)
        if entry[0] == 0xFFFF:
            break
        entries.append(entry)

    new_data = {}
    worst = 0
    for index, (sid, _, offset, _, info, length_word, loop, loop_len, extra) in enumerate(entries):
        comp, length = length_word >> 24, length_word & 0xFFFFFF
        if not loop_len or loop % 14 == 0:
            continue
        if comp != 0 or not extra:
            raise ValueError(f"sample {sid:#x}: unaligned loop in a non-ADPCM sample (type {comp})")
        coefs = struct.unpack_from(">16h", sdi, extra + 8)
        sample = sam[offset:offset + _adpcm_bytes(length)]
        data, length, loop, loop_ps, loop_y0, loop_y1, error = _align_loop(sample, coefs, length, loop, loop_len)
        worst = max(worst, error)
        new_data[index] = data
        struct.pack_into(">II", sdi, index * 32 + 0x10, comp << 24 | length, loop)
        struct.pack_into(">Bxhh", sdi, extra + 3, loop_ps, loop_y0, loop_y1)

    # Lay the samples out again (32-byte aligned), shared offsets stay shared
    out = bytearray()
    placed = {}
    for index in sorted(range(len(entries)), key=lambda i: entries[i][2]):
        offset, length_word = entries[index][2], entries[index][5]
        if index in new_data:
            data = new_data[index]
        else:
            if offset in placed:
                struct.pack_into(">I", sdi, index * 32 + 4, placed[offset])
                continue
            data = sam[offset:offset + _adpcm_bytes(length_word & 0xFFFFFF)]
        new_offset = len(out)
        if index not in new_data:
            placed[offset] = new_offset
        out += data
        out += bytes(-len(out) % 32)
        struct.pack_into(">I", sdi, index * 32 + 4, new_offset)
    stats = f"{len(new_data)} of {len(entries)} sample loops aligned, max error {worst}"
    return bytes(sdi), bytes(out), stats
