// uncompress() for zlbDecompress (patches/main/zlb.c.sed) without the C heap.
// zlib mallocs its ~7 KB inflate state (plus a 32 KB window when the output
// does not fit in one call). With the heap exhausted that failed with
// Z_MEM_ERROR on a map change, the map block stayed garbage and
// mapBlockUnpackFile crashed reading it (bug 30). The state comes from a
// static arena instead, so only bad data can make it fail.
#include <3ds.h>
#include <string.h>
#include <zlib.h>

#define ARENA_BYTES (48 * 1024)

static u8 s_arena[ARENA_BYTES] __attribute__((aligned(8)));
static u32 s_used;
static LightLock s_lock = 1;  // 1 = unlocked (LightLock_Init)

static voidpf arena_alloc(voidpf opaque, uInt items, uInt size) {
    u32 n = ((u32)items * size + 7) & ~7u;
    if (s_used + n > ARENA_BYTES) return Z_NULL;
    void* p = s_arena + s_used;
    s_used += n;
    return p;
}

static void arena_free(voidpf opaque, voidpf p) {}

// Same contract and results as zlib's uncompress()
int port_uncompress(unsigned char* dest, unsigned long* dest_len, const unsigned char* src, unsigned long src_len) {
    z_stream s;
    memset(&s, 0, sizeof(s));
    s.next_in = (Bytef*)src;
    s.avail_in = (uInt)src_len;
    s.next_out = dest;
    s.avail_out = (uInt)*dest_len;
    s.zalloc = arena_alloc;
    s.zfree = arena_free;

    LightLock_Lock(&s_lock);
    s_used = 0;
    int r = inflateInit(&s);
    if (r == Z_OK) {
        // Z_FINISH: a stream that fits the output never allocates the window
        r = inflate(&s, Z_FINISH);
        *dest_len = s.total_out;
        inflateEnd(&s);
        if (r == Z_STREAM_END) r = Z_OK;
        else if (r == Z_NEED_DICT) r = Z_DATA_ERROR;
        else if (r == Z_BUF_ERROR && s.avail_out) r = Z_DATA_ERROR;  // Input ended early
    }
    LightLock_Unlock(&s_lock);
    return r;
}
