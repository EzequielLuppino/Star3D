// Save states as exact memory snapshots, like an emulator's: the whole game
// state is copied to a file and copied back on load, so the game continues
// from the very frame it was saved on (enemies, timers, music, cutscenes).
//
// What is game state: the data and bss of the game objects (grouped between
// __game_data_start/end and __game_bss_start/end by ld/star3d.ld, including
// the MusyX pool of snapmem_3ds.c), the emulated MEM1 arena and the used part
// of ARAM. Platform state outside those blocks is rebuilt after a restore: the
// ndsp channels from the restored MusyX voices, the disc stream (the game
// restarts it), the GX caches, the open DVD handles (reopened by slot) and the
// movie frame slots. A snapshot only fits the build that wrote it, at the same
// memory addresses; the header records both, and the MusyX sample set (kept in
// linear memory, not in the snapshot). When they differ, the state falls back
// to its text block (savestate_3ds.c), which reloads the map from the saved
// savegame and puts the player back at the saved position.
//
// Snapshots are taken and restored at the top of gameLoop
// (port_savestate_point, patches/main/gameloop_main.c.sed), between frames,
// with no DVD read in flight and the MusyX lock held.
//
// Files: sdmc:/3ds/star3d/savestates/star3d.YYYYMMDD_HHMMSS.cst, gzip
// compressed (Azahar names its states <title>.<date>.cst too). Every save
// writes a new file and none is deleted automatically; the touch menu offers
// the 3 newest as slots 1-3 (1 = newest) and "Clean old states" deletes the
// rest. Contents: Header, text block, then the regions in Header order.

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>
#include <3ds.h>
#include "gx_rec.h"
#include "log.h"
#include "movie_3ds.h"
#include "port_3ds.h"
#include "savestate_3ds.h"
#include "snapmem_3ds.h"

#define STATE_DIR      "sdmc:/3ds/star3d/savestates"
#define STATE_PREFIX   "star3d."
#define STATE_EXT      ".cst"
#define STATE_MAGIC    "S3DSTATE"
#define STATE_VERSION  1
#define TEXT_CAP       (64 * 1024)
#define CHUNK          (1024 * 1024)
#define MAX_LIST       256
#define FLAG_SNAPSHOT  1

extern char __start__[], __exidx_start[];
extern char __game_data_start[], __game_data_end[];
extern char __game_bss_start[], __game_bss_end[];
bool salPCExternalEnter(void);
bool salPCExternalLeave(void);

enum { REG_DATA, REG_BSS, REG_MEM1, REG_ARAM, REG_COUNT };

typedef struct {
    char magic[8];
    u32 version;
    u32 header_size;
    u32 build_id;
    u32 flags;
    u32 text_len;
    u32 musyx_fp;
    u32 base[REG_COUNT];
    u32 size[REG_COUNT];
    s32 dvd[PORT_DVD_MAX_OPEN];
} Header;

static const char* const kRegionName[REG_COUNT] = { "data", "bss", "mem1", "aram" };

enum { REQ_NONE, REQ_SAVE, REQ_LOAD };
static int s_request;
static int s_request_slot;
static int s_wait_frames;
static char s_message[64];
static bool s_message_new;

// Newest first
static char s_names[MAX_LIST][40];
static int s_count;
static bool s_list_valid;

static void message(const char* msg) {
    snprintf(s_message, sizeof(s_message), "%s", msg);
    s_message_new = true;
}

const char* savestate_snap_message(void) {
    if (!s_message_new) return NULL;
    s_message_new = false;
    return s_message;
}

// Hash of the code and read-only data: a snapshot holds code and data pointers
// FNV-1a over the 32-bit words in [lo, hi)
u32 port_fnv1a(const void* lo, const void* hi) {
    u32 h = 2166136261u;
    for (const u32* p = (const u32*)lo; p < (const u32*)hi; p++) h = (h ^ *p) * 16777619u;
    return h;
}

static u32 build_id(void) {
    static u32 id;
    if (id) return id;
    // Skips the 3dsx crt0 parameter block (__service_ptr .. __system_runflags,
    // 0x100008-0x10001F), which the Homebrew Launcher fills in (bug 13)
    u32 h = port_fnv1a(__start__ + 0x20, __exidx_start);
    id = h ? h : 1;
    return id;
}

// Logged at startup, so a hardware log names the exact build
u32 port_build_id(void) { return build_id(); }

static void regions(u32 base[REG_COUNT], u32 size[REG_COUNT]) {
    base[REG_DATA] = (u32)__game_data_start;
    size[REG_DATA] = (u32)(__game_data_end - __game_data_start);
    base[REG_BSS] = (u32)__game_bss_start;
    size[REG_BSS] = (u32)(__game_bss_end - __game_bss_start);
    base[REG_MEM1] = (u32)port_mem1_base();
    size[REG_MEM1] = PORT_MEM1_SIZE;
    base[REG_ARAM] = (u32)ARGetStorageAddress();
    size[REG_ARAM] = port_aram_used();
}

static void make_dirs(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/star3d", 0777);
    mkdir(STATE_DIR, 0777);
}

// --- File list ------------------------------------------------------------------

static bool is_state_name(const char* n) {
    size_t len = strlen(n), pl = strlen(STATE_PREFIX), el = strlen(STATE_EXT);
    return len > pl + el && len < sizeof(s_names[0]) && !strncmp(n, STATE_PREFIX, pl) &&
           !strcmp(n + len - el, STATE_EXT);
}

static int cmp_desc(const void* a, const void* b) { return strcmp((const char*)b, (const char*)a); }

// The names carry the creation time, so name order is age order
static void refresh_list(void) {
    if (s_list_valid) return;
    s_list_valid = true;
    s_count = 0;
    DIR* d = opendir(STATE_DIR);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && s_count < MAX_LIST) {
        if (is_state_name(e->d_name)) snprintf(s_names[s_count++], sizeof(s_names[0]), "%s", e->d_name);
    }
    closedir(d);
    qsort(s_names, s_count, sizeof(s_names[0]), cmp_desc);
}

// "star3d.20261011_142530.cst" -> "10/11 14:25"
static void slot_label(const char* name, char* out, size_t cap) {
    const char* t = name + strlen(STATE_PREFIX);
    if (strlen(t) >= 15 && t[8] == '_')
        snprintf(out, cap, "%.2s/%.2s %.2s:%.2s", t + 4, t + 6, t + 9, t + 11);
    else
        snprintf(out, cap, "%s", t);
}

int savestate_snap_slots(char labels[SAVESTATE_SLOTS][24]) {
    refresh_list();
    for (int i = 0; i < SAVESTATE_SLOTS; i++) {
        if (i < s_count) slot_label(s_names[i], labels[i], 24);
        else labels[i][0] = 0;
    }
    return s_count < SAVESTATE_SLOTS ? s_count : SAVESTATE_SLOTS;
}

int savestate_snap_old_count(void) {
    refresh_list();
    return s_count > SAVESTATE_SLOTS ? s_count - SAVESTATE_SLOTS : 0;
}

// Deletes all but the newest SAVESTATE_SLOTS states, and leftover temp files
int savestate_snap_clean(void) {
    s_list_valid = false;
    refresh_list();
    int removed = 0;
    char path[96];
    for (int i = SAVESTATE_SLOTS; i < s_count; i++) {
        snprintf(path, sizeof(path), STATE_DIR "/%s", s_names[i]);
        if (remove(path) == 0) removed++;
        else LOG_WARN("savestate", "cannot delete %s", path);
    }
    DIR* d = opendir(STATE_DIR);
    if (d) {
        struct dirent* e;
        while ((e = readdir(d))) {
            size_t len = strlen(e->d_name);
            if (len > 4 && !strcmp(e->d_name + len - 4, ".tmp")) {
                snprintf(path, sizeof(path), STATE_DIR "/%s", e->d_name);
                remove(path);
            }
        }
        closedir(d);
    }
    s_list_valid = false;
    LOG_INFO("savestate", "cleaned %d old states", removed);
    return removed;
}

// --- Save ---------------------------------------------------------------------------

static bool write_all(gzFile gz, const void* p, u32 n) {
    const u8* b = (const u8*)p;
    while (n) {
        u32 c = n < CHUNK ? n : CHUNK;
        if (gzwrite(gz, b, c) != (int)c) return false;
        b += c;
        n -= c;
    }
    return true;
}

static void do_save(void) {
    char* text = malloc(TEXT_CAP);
    if (!text) {
        message("Save failed: out of memory");
        return;
    }
    int text_len = savestate_format(text, TEXT_CAP);
    if (text_len <= 0) {
        free(text);
        message("Save failed: no game state");
        return;
    }

    static Header h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, STATE_MAGIC, 8);
    h.version = STATE_VERSION;
    h.header_size = sizeof(h);
    h.build_id = build_id();
    h.text_len = (u32)text_len;
    regions(h.base, h.size);
    bool snapshot = port_musyx_pool_snapshot_ok() && h.base[REG_MEM1] && h.base[REG_ARAM];
    if (snapshot) h.flags |= FLAG_SNAPSHOT;
    else LOG_WARN("savestate", "MusyX memory outside its pool: writing the reload block only");

    make_dirs();
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char stamp[24], path[96], tmp[100];
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", tm);
    snprintf(path, sizeof(path), STATE_DIR "/" STATE_PREFIX "%s" STATE_EXT, stamp);
    for (int n = 2; access(path, F_OK) == 0 && n < 100; n++)
        snprintf(path, sizeof(path), STATE_DIR "/" STATE_PREFIX "%s_%d" STATE_EXT, stamp, n);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    u64 t0 = osGetTime();
    gzFile gz = gzopen(tmp, "wb1");
    bool ok = gz != NULL;
    if (ok) {
        gzbuffer(gz, 256 * 1024);
        salPCExternalEnter();
        h.musyx_fp = port_musyx_sample_fingerprint();
        port_dvd_save_handles(h.dvd, PORT_DVD_MAX_OPEN);
        ok = write_all(gz, &h, sizeof(h)) && write_all(gz, text, h.text_len);
        for (int r = 0; ok && snapshot && r < REG_COUNT; r++) ok = write_all(gz, (void*)h.base[r], h.size[r]);
        salPCExternalLeave();
        if (gzclose(gz) != Z_OK) ok = false;
    }
    free(text);
    if (ok) {
        remove(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        remove(tmp);
        LOG_ERROR("savestate", "cannot write %s", path);
        message("Save failed: cannot write the SD card");
        return;
    }
    s_list_valid = false;
    LOG_WARN("savestate", "saved %s (%s, %u ms)", path, snapshot ? "snapshot" : "reload only",
             (unsigned)(osGetTime() - t0));
    message(snapshot ? "State saved to slot 1" : "State saved (reload only)");
}

// --- Load ---------------------------------------------------------------------------

static bool read_all(gzFile gz, void* p, u32 n) {
    u8* b = (u8*)p;
    while (n) {
        u32 c = n < CHUNK ? n : CHUNK;
        if (gzread(gz, b, c) != (int)c) return false;
        b += c;
        n -= c;
    }
    return true;
}

// Uncompressed size from the gzip trailer (mod 2^32)
static bool gzip_isize(const char* path, u32* out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    u8 b[4];
    bool ok = fseek(f, -4, SEEK_END) == 0 && fread(b, 1, 4, f) == 4;
    fclose(f);
    *out = b[0] | (b[1] << 8) | (b[2] << 16) | ((u32)b[3] << 24);
    return ok;
}

// Why the snapshot does not fit this run, NULL when it does
static const char* snapshot_mismatch(const Header* h) {
    if (!(h->flags & FLAG_SNAPSHOT)) return "no snapshot in the file";
    if (h->build_id != build_id()) return "different build";
    u32 base[REG_COUNT], size[REG_COUNT];
    regions(base, size);
    for (int r = 0; r < REG_COUNT; r++) {
        // ARAM grows as the game uploads to it; the snapshot carries its own extent
        bool size_ok = r == REG_ARAM ? h->size[r] <= PORT_ARAM_SIZE : h->size[r] == size[r];
        if (h->base[r] != base[r] || !size_ok) return kRegionName[r];
    }
    if (h->musyx_fp != port_musyx_sample_fingerprint()) return "different sample set";
    return NULL;
}

static void after_restore(const Header* h) {
    port_audio_stream_reset();
    port_gx_request_reset();
    movie_reset_slots();
    if (!port_dvd_restore_handles(h->dvd, PORT_DVD_MAX_OPEN)) LOG_ERROR("savestate", "some DVD files did not reopen");
    savestate_cancel_pending();
}

static void do_load(int slot) {
    s_list_valid = false;
    refresh_list();
    if (slot < 0 || slot >= s_count || slot >= SAVESTATE_SLOTS) {
        message("Slot is empty");
        return;
    }
    char path[96];
    snprintf(path, sizeof(path), STATE_DIR "/%s", s_names[slot]);
    gzFile gz = gzopen(path, "rb");
    if (!gz) {
        message("Load failed: cannot open the file");
        return;
    }
    gzbuffer(gz, 256 * 1024);
    static Header h;
    char* text = NULL;
    const char* fail = NULL;
    if (gzread(gz, &h, sizeof(h)) != (int)sizeof(h) || memcmp(h.magic, STATE_MAGIC, 8) ||
        h.header_size != sizeof(h) || h.text_len >= TEXT_CAP)
        fail = "not a Star 3D state";
    else if (h.version != STATE_VERSION)
        fail = "unsupported version";
    if (!fail) {
        text = malloc(h.text_len + 1);
        if (!text || gzread(gz, text, h.text_len) != (int)h.text_len) fail = "damaged file";
        else text[h.text_len] = 0;
    }
    if (fail) {
        gzclose(gz);
        free(text);
        LOG_ERROR("savestate", "%s: %s", path, fail);
        message("Load failed: damaged state file");
        return;
    }

    const char* why = snapshot_mismatch(&h);
    if (!why) {
        u32 total = sizeof(h) + h.text_len, isize = 0;
        for (int r = 0; r < REG_COUNT; r++) total += h.size[r];
        if (!gzip_isize(path, &isize) || isize != total) why = "truncated file";
    }
    if (why) {
        gzclose(gz);
        LOG_WARN("savestate", "%s: snapshot not usable (%s), reloading from its savegame", path, why);
        bool ok = savestate_apply_text(text);
        free(text);
        message(ok ? "Loading (map reload)" : "Load failed: no usable state");
        return;
    }

    u64 t0 = osGetTime();
    bool ok = true;
    salPCExternalEnter();
    for (int r = 0; ok && r < REG_COUNT; r++) ok = read_all(gz, (void*)h.base[r], h.size[r]);
    port_musyx_after_restore();
    salPCExternalLeave();
    gzclose(gz);
    after_restore(&h);
    if (!ok) {
        // Game memory is partly overwritten; the reload block is the best left
        LOG_ERROR("savestate", "%s: read failed during restore, reloading from its savegame", path);
        savestate_apply_text(text);
        free(text);
        message("Load failed: read error, reloading map");
        return;
    }
    free(text);
    LOG_WARN("savestate", "loaded %s (%u ms)", path, (unsigned)(osGetTime() - t0));
    char msg[40];
    snprintf(msg, sizeof(msg), "Loaded slot %d", slot + 1);
    message(msg);
}

// --- Requests -----------------------------------------------------------------------

void savestate_snap_request_save(void) {
    if (!savestate_in_game()) {
        message("Save states work in game only");
        return;
    }
    s_request = REQ_SAVE;
    s_wait_frames = 0;
    message("Saving...");
}

void savestate_snap_request_load(int slot) {
    refresh_list();
    if (slot < 0 || slot >= s_count || slot >= SAVESTATE_SLOTS) {
        message("Slot is empty");
        return;
    }
    s_request = REQ_LOAD;
    s_request_slot = slot;
    s_wait_frames = 0;
    message("Loading...");
}

void port_savestate_point(void) {
    if (s_request == REQ_NONE) return;
    // A read in flight would land in memory that is being saved or replaced
    if (!port_dvd_idle()) {
        if (++s_wait_frames > 600) {
            s_request = REQ_NONE;
            message("Busy loading, try again");
        }
        return;
    }
    int req = s_request;
    s_request = REQ_NONE;
    // Between frames the render thread is idle anyway (gx/gx_rec.c drains at
    // the frame end); this keeps it so if the save point ever moves
    gxrec_direct_begin();
    if (req == REQ_SAVE) {
        if (savestate_in_game()) do_save();
        else message("Save states work in game only");
    } else {
        do_load(s_request_slot);
    }
    gxrec_direct_end();
}
