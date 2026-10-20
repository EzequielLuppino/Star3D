// Attract movie playback from build-time ETC1 frames (see movie_3ds.h and the
// .v3d layout in tools/video_convert.py).
//
// The game's player is unchanged: it reads THP frames, decodes their audio and
// calls THPVideoDecode with the frame's JPEG and its Y/U/V buffers. The JPEG is
// identified by a hash of its first scan bytes (normally just "previous frame
// + 1"), and the pre-encoded frame is read into a linear-memory ETC1 texture
// owned by the Y buffer (one per texture set the game cycles through). The
// next frame is prefetched on the worker core, so the call usually costs one
// 64 KB memcpy.
//
// Without a .v3d (builds without Pillow/texconv) aurora's software decoder
// runs instead; in the 3DS build that outputs black frames (thp_compat.hpp).
// S3D_MOVIE=0 forces that path.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include "config.h"
#include "gx_init.h"
#include "gx_rec.h"
#include "log.h"
#include "movie_3ds.h"
#include "profiler.h"
#include "worker.h"

#define V3D_PATH "romfs:/files/starfox.v3d"  // named after the disc's .thp
#define V3D_MAGIC 0x31443356u  // "V3D1"
#define MAX_SLOTS 4

typedef struct {
    u32 magic, header_size, frames;
    float fps;
    u32 width, height, format, frame_size;
    u32 src_width, src_height, failed;
    u32 table_offset, frames_offset;
} V3dHeader;

typedef struct {
    const void* y;
    const void* u;
    const void* v;
    C3D_Tex tex;
    u32 last_use;
} Slot;

typedef struct {
    u8* buffer;  // Linear memory, frame_size bytes
    int frame;   // Frame in the buffer, -1 = none
    int want;
    WorkerJob job;
} Prefetch;

s32 THPVideoDecode_sw(const void* file, void* tileY, void* tileU, void* tileV, void* work);

static int s_state;  // 0 not tried, 1 open, -1 unavailable
static FILE* s_file;
static V3dHeader s_hdr;
static u32* s_hashes;
static Slot s_slots[MAX_SLOTS];
static u32 s_use_counter;
static int s_last_frame = -1;
static Prefetch s_prefetch;
static LightLock s_file_lock;

bool movie_active(void) { return s_state == 1; }

// After a save state restore the game's Y/U/V buffers may sit elsewhere
void movie_reset_slots(void) {
    if (s_state != 1) return;
    if (s_prefetch.job.state) worker_wait(&s_prefetch.job);
    s_prefetch.frame = -1;
    for (int i = 0; i < MAX_SLOTS; i++) s_slots[i].y = s_slots[i].u = s_slots[i].v = NULL;
    s_last_frame = -1;
}

static bool read_frame(int frame, void* dst) {
    LightLock_Lock(&s_file_lock);
    bool ok = fseek(s_file, (long)(s_hdr.frames_offset + (u32)frame * s_hdr.frame_size), SEEK_SET) == 0 &&
              fread(dst, 1, s_hdr.frame_size, s_file) == s_hdr.frame_size;
    LightLock_Unlock(&s_file_lock);
    return ok;
}

static void prefetch_job(void* arg) {
    Prefetch* p = arg;
    p->frame = read_frame(p->want, p->buffer) ? p->want : -1;
}

static bool movie_open(void) {
    if (s_state) return s_state > 0;
    s_state = -1;
    if (!cfg_bool("S3D_MOVIE", true)) return false;
    s_file = fopen(V3D_PATH, "rb");
    if (!s_file) {
        LOG_WARN("movie", "%s not found: attract movie frames are black (build without Pillow/texconv?)", V3D_PATH);
        return false;
    }
    setvbuf(s_file, NULL, _IONBF, 0);
    if (fread(&s_hdr, sizeof(s_hdr), 1, s_file) != 1 || s_hdr.magic != V3D_MAGIC || s_hdr.frames == 0 ||
        s_hdr.format != GPU_ETC1 || s_hdr.frame_size != s_hdr.width * s_hdr.height / 2) {
        LOG_ERROR("movie", "%s: bad header", V3D_PATH);
        fclose(s_file);
        return false;
    }
    s_hashes = malloc(s_hdr.frames * 4);
    if (!s_hashes || fseek(s_file, (long)s_hdr.table_offset, SEEK_SET) != 0 ||
        fread(s_hashes, 4, s_hdr.frames, s_file) != s_hdr.frames) {
        LOG_ERROR("movie", "%s: cannot read the frame table", V3D_PATH);
        fclose(s_file);
        free(s_hashes);
        return false;
    }
    LightLock_Init(&s_file_lock);
    s_prefetch.buffer = linearAlloc(s_hdr.frame_size);
    s_prefetch.frame = -1;
    LOG_INFO("movie", "%s: %lu frames %lux%lu ETC1 @ %.2f fps (source %lux%lu, %lu frames repeated)", V3D_PATH,
             s_hdr.frames, s_hdr.width, s_hdr.height, s_hdr.fps, s_hdr.src_width, s_hdr.src_height, s_hdr.failed);
    s_state = 1;
    return true;
}

// Must match frame_hash() in tools/video_convert.py
static bool frame_hash(const u8* jpeg, u32* out) {
    u32 pos = 2;
    for (int guard = 0; guard < 64; guard++) {
        if (jpeg[pos] != 0xFF) return false;
        u8 marker = jpeg[pos + 1];
        u32 len = (jpeg[pos + 2] << 8) | jpeg[pos + 3];
        pos += 2 + len;
        if (marker == 0xDA) {
            u32 h = 2166136261u;
            for (int i = 0; i < 64; i++) h = (h ^ jpeg[pos + i]) * 16777619u;
            *out = h;
            return true;
        }
        if (pos > 0x10000) return false;
    }
    return false;
}

static int find_frame(u32 hash) {
    int n = (int)s_hdr.frames;
    int guess[2] = { (s_last_frame + 1) % n, s_last_frame < 0 ? 0 : s_last_frame };
    for (int i = 0; i < 2; i++)
        if (s_hashes[guess[i]] == hash) return guess[i];
    for (int i = 0; i < n; i++)
        if (s_hashes[i] == hash) return i;
    return -1;
}

static Slot* slot_for(const void* y, const void* u, const void* v) {
    Slot* lru = &s_slots[0];
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot* s = &s_slots[i];
        if (s->y == y) {
            s->u = u;
            s->v = v;
            return s;
        }
        if (s->last_use < lru->last_use) lru = s;
    }
    if (!lru->tex.data && !C3D_TexInit(&lru->tex, (u16)s_hdr.width, (u16)s_hdr.height, GPU_ETC1)) {
        LOG_ONCE(LOG_LEVEL_ERROR, "movie", "out of linear memory for movie frames");
        return NULL;
    }
    lru->y = y;
    lru->u = u;
    lru->v = v;
    return lru;
}

static s32 decode_frame(const void* file, void* tileY, void* tileU, void* tileV);

// The frame upload rewrites a texture the GX layer may be drawing with and
// the slots movie_plane reads (render side): with the render thread
// (gx/gx_rec.c) it runs after a drain
s32 THPVideoDecode(const void* file, void* tileY, void* tileU, void* tileV, void* work) {
    if (!movie_open()) return THPVideoDecode_sw(file, tileY, tileU, tileV, work);
    gxrec_direct_begin();
    s32 r = decode_frame(file, tileY, tileU, tileV);
    gxrec_direct_end();
    return r;
}

static s32 decode_frame(const void* file, void* tileY, void* tileU, void* tileV) {
    PROF_ZONE("movie.frame");

    u32 hash;
    int frame = frame_hash(file, &hash) ? find_frame(hash) : -1;
    if (frame < 0) {
        LOG_ONCE(LOG_LEVEL_WARN, "movie", "movie frame not found in %s (keeping the previous frame)", V3D_PATH);
        PROF_COUNT("movie.frames_unknown", 1);
        frame = s_last_frame >= 0 ? s_last_frame : 0;
    }

    Slot* slot = slot_for(tileY, tileU, tileV);
    if (!slot) return 0;
    slot->last_use = ++s_use_counter;

    if (s_prefetch.job.state) worker_wait(&s_prefetch.job);
    if (s_prefetch.frame == frame && s_prefetch.buffer) {
        memcpy(slot->tex.data, s_prefetch.buffer, s_hdr.frame_size);
        PROF_COUNT("movie.frames_prefetched", 1);
    } else if (read_frame(frame, slot->tex.data)) {
        PROF_COUNT("movie.frames_read", 1);
    } else {
        LOG_ONCE(LOG_LEVEL_ERROR, "movie", "read error in %s", V3D_PATH);
    }
    GSPGPU_FlushDataCache(slot->tex.data, s_hdr.frame_size);
    gx_c3d_tex_invalidate();
    s_last_frame = frame;

    if (s_prefetch.buffer) {
        s_prefetch.want = (frame + 1) % (int)s_hdr.frames;
        worker_submit(&s_prefetch.job, prefetch_job, &s_prefetch);
    }
    return 0;
}

int movie_plane(const void* data, C3D_Tex** tex) {
    if (s_state != 1 || !data) return MOVIE_PLANE_NONE;
    for (int i = 0; i < MAX_SLOTS; i++) {
        Slot* s = &s_slots[i];
        if (!s->y) continue;
        if (data == s->y) {
            if (tex) *tex = &s->tex;
            return MOVIE_PLANE_Y;
        }
        if (data == s->u || data == s->v) return MOVIE_PLANE_UV;
    }
    return MOVIE_PLANE_NONE;
}
