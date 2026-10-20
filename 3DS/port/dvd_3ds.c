// DVD layer for 3DS. The disc is extracted at build time into the RomFS by
// tools/extract_disc.py; this file reproduces the GameCube DVD API on top of it:
// the original FST (sys/fst.bin) gives entry numbers and case-insensitive path
// lookup, file data comes from romfs:/files/<path>.
//
// Async reads (DVDReadAsyncPrio) run on the worker thread (core 2, see
// worker.c), so loading overlaps with the game like on the GameCube. Their
// completion callbacks are delivered on the game thread, in submission order,
// when the game re-enables "interrupts" or polls the drive status. Without a
// worker (or with S3D_ASYNC_DVD=0) reads complete at once and only the
// callback is deferred, which is what Aurora (foxhollow's PC backend) does.

#include <dolphin/dvd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <3ds.h>
#include "config.h"
#include "log.h"
#include "port_3ds.h"
#include "profiler.h"
#include "worker.h"
#include "watchdog.h"

#define ROMFS_FILES "romfs:/files"
#define ROMFS_SYS   "romfs:/sys"

typedef struct {
    const char* name;
    u32 parent;       // Index of the parent directory
    u32 next_or_len;  // Directories: index past last child; files: length
    u32 disc_offset;  // Files: offset on the original disc
    bool is_dir;
} FstEntry;

typedef struct {
    FILE* file;
    s32 entrynum;
    LightLock lock;  // fseek+fread pairs from the game thread and the worker
    bool used;
} OpenFile;

// Handles live in a static pool outside the savestate image, so the pointers
// the game keeps in cb.userData stay valid across a snapshot restore
#define MAX_OPEN PORT_DVD_MAX_OPEN
static OpenFile s_open[MAX_OPEN];

// Slots do not move while a read is in flight (the worker holds a pointer to
// the job); s_order keeps them in submission order
typedef struct {
    DVDFileInfo* file_info;
    s32 result;
    bool used;
    WorkerJob job;
    void* addr;
    s32 length, offset;
} PendingCallback;

static FstEntry* s_fst;
static u32 s_fst_count;
static u8* s_fst_data;
static DVDDiskID s_disk_id;
static bool s_initialized;
static BOOL s_auto_invalidation = TRUE;

#define MAX_PENDING 64
static PendingCallback s_slots[MAX_PENDING];
static int s_order[MAX_PENDING];
static int s_pending_count;
static bool s_processing_callbacks;
static int s_async = -1;  // S3D_ASYNC_DVD, read on first use

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

static void* read_whole_file(const char* path, u32* size_out) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = size > 0 ? malloc(size) : NULL;
    if (data && fread(data, 1, size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (size_out) *size_out = data ? (u32)size : 0;
    return data;
}

static bool load_fst(void) {
    u32 size;
    s_fst_data = read_whole_file(ROMFS_SYS "/fst.bin", &size);
    if (!s_fst_data || size < 12) {
        LOG_ERROR("dvd", "cannot read " ROMFS_SYS "/fst.bin - was the disc extracted into the RomFS?");
        return false;
    }

    s_fst_count = be32(s_fst_data + 8);
    const char* strings = (const char*)s_fst_data + s_fst_count * 12;
    s_fst = calloc(s_fst_count, sizeof(FstEntry));
    if (!s_fst) return false;

    // Parent links: walk the tree keeping a stack of open directories
    u32 stack[32];
    int depth = 0;
    stack[0] = 0;
    s_fst[0].is_dir = true;
    s_fst[0].name = "";
    s_fst[0].next_or_len = s_fst_count;

    for (u32 i = 1; i < s_fst_count; i++) {
        const u8* raw = s_fst_data + i * 12;
        while (depth > 0 && i >= s_fst[stack[depth]].next_or_len) depth--;

        FstEntry* e = &s_fst[i];
        e->is_dir = raw[0] != 0;
        e->name = strings + (be32(raw) & 0xFFFFFF);
        e->parent = stack[depth];
        if (e->is_dir) {
            e->next_or_len = be32(raw + 8);
            if (depth < 31) stack[++depth] = i;
        } else {
            e->disc_offset = be32(raw + 4);
            e->next_or_len = be32(raw + 8);
        }
    }
    return true;
}

static bool is_valid_entry(s32 entrynum) {
    return s_initialized && entrynum >= 0 && (u32)entrynum < s_fst_count;
}

static s32 find_in_dir(u32 dir, const char* name, size_t len) {
    u32 end = s_fst[dir].next_or_len;
    for (u32 i = dir + 1; i < end && i < s_fst_count;) {
        if (strncasecmp(s_fst[i].name, name, len) == 0 && s_fst[i].name[len] == '\0') {
            return (s32)i;
        }
        i = s_fst[i].is_dir ? s_fst[i].next_or_len : i + 1;
    }
    return -1;
}

static void build_path(u32 entry, char* out, size_t out_size) {
    // Collect the chain of names from the root to the entry
    u32 chain[32];
    int n = 0;
    for (u32 e = entry; e != 0 && n < 32; e = s_fst[e].parent) chain[n++] = e;

    size_t len = snprintf(out, out_size, "%s", ROMFS_FILES);
    while (n-- > 0 && len < out_size) {
        len += snprintf(out + len, out_size - len, "/%s", s_fst[chain[n]].name);
    }
}

// --- Callback queue ----------------------------------------------------------

static s32 read_file(DVDFileInfo* fi, void* addr, s32 length, s32 offset, bool profile);

static void read_job(void* arg) {
    PendingCallback* p = arg;
    p->result = read_file(p->file_info, p->addr, p->length, p->offset, false);
}

static PendingCallback* alloc_slot(void) {
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!s_slots[i].used) {
            memset(&s_slots[i], 0, sizeof(s_slots[i]));
            s_slots[i].used = true;
            return &s_slots[i];
        }
    }
    return NULL;
}

static void complete_now(DVDFileInfo* fi, s32 result) {
    fi->cb.state = DVD_STATE_END;
    if (fi->callback) fi->callback(result, fi);
}

static void queue_callback(DVDFileInfo* fi, s32 result) {
    PendingCallback* p = alloc_slot();
    if (!p) {
        LOG_ERROR("dvd", "callback queue full, completing immediately");
        complete_now(fi, result);
        return;
    }
    p->file_info = fi;
    p->result = result;
    s_order[s_pending_count++] = p - s_slots;
}

// Read on the worker; false if the queue is full (caller reads synchronously)
static bool queue_async_read(DVDFileInfo* fi, void* addr, s32 length, s32 offset) {
    PendingCallback* p = alloc_slot();
    if (!p) return false;
    p->file_info = fi;
    p->addr = addr;
    p->length = length;
    p->offset = offset;
    s_order[s_pending_count++] = p - s_slots;
    worker_submit(&p->job, read_job, p);
    PROF_COUNT("dvd.async_reads", 1);
    return true;
}

void aurora_dvd_process_callbacks(void) {
    if (s_processing_callbacks) return;
    s_processing_callbacks = true;

    // Callbacks may queue new reads, so take entries off the front one by
    // one, stopping at the first read still in flight (keeps the order)
    while (s_pending_count > 0) {
        PendingCallback* p = &s_slots[s_order[0]];
        if (!worker_done(&p->job)) break;
        DVDFileInfo* fi = p->file_info;
        s32 result = p->result;
        p->used = false;
        memmove(&s_order[0], &s_order[1], (s_pending_count - 1) * sizeof(s_order[0]));
        s_pending_count--;
        complete_now(fi, result);
    }

    s_processing_callbacks = false;
}

static bool remove_pending(DVDFileInfo* fi) {
    for (int i = 0; i < s_pending_count; i++) {
        PendingCallback* p = &s_slots[s_order[i]];
        if (p->file_info == fi) {
            worker_wait(&p->job);  // The buffer must not be written after this returns
            p->used = false;
            memmove(&s_order[i], &s_order[i + 1], (s_pending_count - i - 1) * sizeof(s_order[0]));
            s_pending_count--;
            return true;
        }
    }
    return false;
}

// --- Init / disc info ----------------------------------------------------------

void DVDInit(void) {
    if (s_initialized) return;

    u32 size;
    void* boot = read_whole_file(ROMFS_SYS "/boot.bin", &size);
    if (boot && size >= sizeof(DVDDiskID)) {
        memcpy(&s_disk_id, boot, sizeof(DVDDiskID));
    }
    free(boot);

    if (!load_fst()) return;
    s_initialized = true;
    LOG_INFO("dvd", "disc %.4s%.2s rev %u: %lu FST entries", s_disk_id.gameName, s_disk_id.company,
             s_disk_id.gameVersion, (unsigned long)s_fst_count);
}

DVDDiskID* DVDGetCurrentDiskID(void) { return &s_disk_id; }
BOOL DVDCheckDisk(void) { return s_initialized; }

BOOL DVDSetAutoInvalidation(BOOL autoInval) {
    BOOL prev = s_auto_invalidation;
    s_auto_invalidation = autoInval;
    return prev;
}

s32 DVDGetDriveStatus(void) {
    aurora_dvd_process_callbacks();
    return s_initialized ? DVD_STATE_END : DVD_STATE_NO_DISK;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock* block) {
    aurora_dvd_process_callbacks();
    return block ? block->state : DVD_STATE_FATAL_ERROR;
}

s32 DVDGetFileInfoStatus(const DVDFileInfo* fileInfo) {
    return DVDGetCommandBlockStatus(fileInfo ? &fileInfo->cb : NULL);
}

void DVDPause(void) {}
void DVDResume(void) {}

// --- Paths and files ---------------------------------------------------------

s32 DVDConvertPathToEntrynum(const char* path) {
    if (!s_initialized || !path) return -1;

    u32 current = 0;
    const char* p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        if (!s_fst[current].is_dir) return -1;

        const char* end = p;
        while (*end && *end != '/') end++;
        size_t len = end - p;

        if (len == 1 && p[0] == '.') {
            // Same directory
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            current = s_fst[current].parent;
        } else {
            s32 found = find_in_dir(current, p, len);
            if (found < 0) return -1;
            current = (u32)found;
        }
        p = end;
    }
    return (s32)current;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fileInfo) {
    if (!fileInfo || !is_valid_entry(entrynum) || s_fst[entrynum].is_dir) return FALSE;

    char path[256];
    build_path((u32)entrynum, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) {
        LOG_ERROR("dvd", "cannot open %s", path);
        return FALSE;
    }

    OpenFile* handle = NULL;
    for (int i = 0; i < MAX_OPEN && !handle; i++) {
        if (!s_open[i].used) handle = &s_open[i];
    }
    if (!handle) {
        LOG_ERROR("dvd", "too many open files");
        fclose(f);
        return FALSE;
    }
    handle->used = true;
    handle->file = f;
    handle->entrynum = entrynum;
    LightLock_Init(&handle->lock);

    // The size comes from the RomFS: build-time conversion may change it
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    memset(fileInfo, 0, sizeof(*fileInfo));
    fileInfo->startAddr = s_fst[entrynum].disc_offset;
    fileInfo->length = size >= 0 ? (u32)size : s_fst[entrynum].next_or_len;
    fileInfo->cb.userData = handle;
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

bool port_dvd_get_path(DVDFileInfo* fileInfo, char* out, size_t out_size) {
    OpenFile* handle = fileInfo ? fileInfo->cb.userData : NULL;
    if (!handle) return false;
    build_path((u32)handle->entrynum, out, out_size);
    return true;
}

void gx_gpu_trace_note_file(const char* name);  // gx/gx_init.c (S3D_GPU_TRACE_PAUSE)

BOOL DVDOpen(const char* fileName, DVDFileInfo* fileInfo) {
    s32 entrynum = DVDConvertPathToEntrynum(fileName);
    if (entrynum < 0) {
        LOG_WARN("dvd", "DVDOpen(\"%s\"): no such file (caller %p)", fileName ? fileName : "(null)",
                 __builtin_return_address(0));
        return FALSE;
    }
    LOG_DEBUG("dvd", "DVDOpen(\"%s\") -> entry %ld", fileName, (long)entrynum);
    gx_gpu_trace_note_file(fileName);
    return DVDFastOpen(entrynum, fileInfo);
}

BOOL DVDClose(DVDFileInfo* fileInfo) {
    if (!fileInfo) return FALSE;
    while (remove_pending(fileInfo)) {}
    OpenFile* handle = fileInfo->cb.userData;
    if (handle) {
        fclose(handle->file);
        handle->file = NULL;
        handle->used = false;
        fileInfo->cb.userData = NULL;
    }
    fileInfo->cb.state = DVD_STATE_END;
    return TRUE;
}

// Slow storage simulation for Azahar, which reads at host speed: each read
// takes at least S3D_IO_SIM_LATENCY_US plus its size at S3D_IO_SIM_KBPS. The
// first real New 3DS bench (2026-10-11) fits about 9000 us + 11000 KB/s, with
// a cold 4 KB read near 67000 us. 0 = off (default)
static void io_sim_wait(u64 t0, s32 length) {
    static int latency_us = -1, kbps = -1;
    CFG_LIVE(latency_us, cfg_int("S3D_IO_SIM_LATENCY_US", 0));
    CFG_LIVE(kbps, cfg_int("S3D_IO_SIM_KBPS", 0));
    if (latency_us <= 0 && kbps <= 0) return;
    u64 want_us = (latency_us > 0 ? (u64)latency_us : 0) + (kbps > 0 ? (u64)length * 1000000 / ((u64)kbps * 1024) : 0);
    u64 spent_us = (svcGetSystemTick() - t0) / CPU_TICKS_PER_USEC;
    if (want_us > spent_us) svcSleepThread((want_us - spent_us) * 1000);
}

// Read into addr; the GameCube rounds reads up to 32 bytes, so reading past the
// end of the file is allowed and the remainder is zero-filled
// The profiler is not thread-safe, so the worker reads with profile = false
static s32 read_file(DVDFileInfo* fi, void* addr, s32 length, s32 offset, bool profile) {
    OpenFile* handle = fi->cb.userData;
    if (!handle || length < 0 || offset < 0) return DVD_RESULT_FATAL_ERROR;
    u64 t0 = svcGetSystemTick();

    size_t got = 0;
    volatile const char* phase = g_main_phase;
    if (profile) WD_PHASE("dvd: sync read");
    LightLock_Lock(&handle->lock);
    if (fseek(handle->file, offset, SEEK_SET) == 0) {
        got = fread(addr, 1, length, handle->file);
    }
    LightLock_Unlock(&handle->lock);
    io_sim_wait(t0, length);
    if (profile) g_main_phase = phase;
    if (got < (size_t)length) {
        memset((u8*)addr + got, 0, length - got);
    }
    u64 us = (svcGetSystemTick() - t0) / CPU_TICKS_PER_USEC;
    if (us >= 250000) {
        char path[128];
        build_path((u32)handle->entrynum, path, sizeof(path));
        LOG_WARN("dvd", "slow %s read: %ld KB at offset %ld of %s took %lu ms", profile ? "sync" : "async",
                 (long)(length / 1024), (long)offset, path + sizeof(ROMFS_FILES), (unsigned long)(us / 1000));
    }
    if (profile) {
        PROF_COUNT("dvd.bytes", length);
        PROF_COUNT("dvd.sync_read_us", us);
    }
    fi->cb.transferredSize = length;
    return length;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset,
                      DVDCallback callback, s32 prio) {
    (void)prio;
    if (!fileInfo || !fileInfo->cb.userData) return FALSE;

    fileInfo->callback = callback;
    fileInfo->cb.state = DVD_STATE_BUSY;
    fileInfo->cb.addr = addr;
    fileInfo->cb.length = length;
    fileInfo->cb.offset = offset;
    if (s_async < 0) s_async = cfg_bool("S3D_ASYNC_DVD", true) && worker_core() >= 0;
    if (s_async && queue_async_read(fileInfo, addr, length, offset)) {
        PROF_COUNT("dvd.bytes", length);
        return TRUE;
    }
    queue_callback(fileInfo, read_file(fileInfo, addr, length, offset, true));
    return TRUE;
}

s32 DVDReadPrio(DVDFileInfo* fileInfo, void* addr, s32 length, s32 offset, s32 prio) {
    (void)prio;
    if (!fileInfo) return DVD_RESULT_FATAL_ERROR;
    s32 result = read_file(fileInfo, addr, length, offset, true);
    fileInfo->cb.state = DVD_STATE_END;
    return result;
}

s32 DVDCancel(volatile DVDCommandBlock* block) {
    if (!block) return DVD_RESULT_FATAL_ERROR;
    // DVDCommandBlock is the first member of DVDFileInfo
    remove_pending((DVDFileInfo*)block);
    block->state = DVD_STATE_CANCELED;
    return DVD_RESULT_GOOD;
}

int DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    DVDCancel(block);
    if (callback) callback(DVD_RESULT_CANCELED, block);
    return TRUE;
}

// --- Streaming (DTK audio): handled by the audio layer, see fhAIPrepareStream --

BOOL DVDPrepareStreamAsync(DVDFileInfo* fileInfo, u32 length, u32 offset, DVDCallback callback) {
    (void)length;
    (void)offset;
    if (!fileInfo || !fileInfo->cb.userData) return FALSE;
    fileInfo->cb.state = DVD_STATE_END;
    if (callback) callback(DVD_RESULT_IGNORED, fileInfo);
    return TRUE;
}

int DVDCancelStreamAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    if (block) block->state = DVD_STATE_CANCELED;
    if (callback) callback(DVD_RESULT_CANCELED, block);
    return TRUE;
}

int DVDStopStreamAtEndAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    if (block) block->state = DVD_STATE_END;
    if (callback) callback(DVD_RESULT_GOOD, block);
    return TRUE;
}

// The game polls this every frame: a result of 0 means the stream has ended
int DVDGetStreamPlayAddrAsync(DVDCommandBlock* block, DVDCBCallback callback) {
    if (block) block->state = DVD_STATE_END;
    if (callback) callback((s32)fhAIGetStreamPlayAddr(), block);
    return TRUE;
}

// --- Savestates ------------------------------------------------------------------

bool port_dvd_idle(void) { return s_pending_count == 0; }

// Entry number of each open handle (-1 = free)
void port_dvd_save_handles(s32 out[], int n) {
    for (int i = 0; i < n; i++) {
        out[i] = i < MAX_OPEN && s_open[i].used ? s_open[i].entrynum : -1;
    }
}

// Make the open handles match a saved table: the restored game memory points
// at these slots by address
bool port_dvd_restore_handles(const s32 in[], int n) {
    bool ok = true;
    for (int i = 0; i < MAX_OPEN; i++) {
        s32 want = i < n ? in[i] : -1;
        OpenFile* h = &s_open[i];
        if (h->used && h->entrynum == want) {
            continue;
        }
        if (h->used) {
            fclose(h->file);
            h->file = NULL;
            h->used = false;
        }
        if (want < 0) continue;
        if (!is_valid_entry(want) || s_fst[want].is_dir) {
            ok = false;
            continue;
        }
        char path[256];
        build_path((u32)want, path, sizeof(path));
        h->file = fopen(path, "rb");
        if (!h->file) {
            LOG_ERROR("dvd", "savestate: cannot reopen %s", path);
            ok = false;
            continue;
        }
        h->entrynum = want;
        LightLock_Init(&h->lock);
        h->used = true;
    }
    return ok;
}
