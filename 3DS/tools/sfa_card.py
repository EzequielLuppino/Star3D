import struct, sys

SLOT_OFF, SLOT_SIZE, BLOCKS = 0xA50, 0x6EC, (0x2000, 0x4000)

def checksum(block):
    x, a = 0, 1
    for i in range(0x3FF):
        v = struct.unpack_from(">Q", block, i * 8)[0]
        x ^= v
        a = (a + v) & 0xFFFFFFFFFFFFFFFF
    return x ^ ((a + 13) & 0xFFFFFFFFFFFFFFFF)

def load(path):
    data = bytearray(open(path, "rb").read())
    if data[:4] == b"GSAE" and len(data) == 0x6040:
        data = data[0x40:]
    return data

def slot(block, i):
    o = SLOT_OFF + i * SLOT_SIZE
    return block[o:o + SLOT_SIZE]

def describe(path):
    data = load(path)
    for b in BLOCKS:
        blk = data[b:b + 0x2000]
        ok = checksum(blk) == struct.unpack_from(">Q", blk, 0x1FF8)[0]
        print(f"{path} block {b:#x} checksum {'ok' if ok else 'BAD'}")
        for i in range(3):
            s = slot(blk, i)
            print(f"  slot {i}: name {bytes(s[0x1C:0x1F])!r} flag {s[0x21]:#04x} score {s[0x55D]}")

def copy(src_path, src_slot, dst_path, dst_slot, out_path):
    src, dst = load(src_path), load(dst_path)
    for b in BLOCKS:
        so = b + SLOT_OFF + src_slot * SLOT_SIZE
        do = b + SLOT_OFF + dst_slot * SLOT_SIZE
        dst[do:do + SLOT_SIZE] = src[so:so + SLOT_SIZE]
        struct.pack_into(">Q", dst, b + 0x1FF8, checksum(dst[b:b + 0x2000]))
    open(out_path, "wb").write(dst)

if __name__ == "__main__":
    if sys.argv[1] == "show":
        for p in sys.argv[2:]:
            describe(p)
    elif sys.argv[1] == "copy":
        copy(sys.argv[2], int(sys.argv[3]), sys.argv[4], int(sys.argv[5]), sys.argv[6])
