#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "bench.h"
#include "config.h"
#include "crash.h"
#include "log.h"
#include "netsrv_3ds.h"
#include "profiler.h"
#include "gx_init.h"
#include "gx_rec.h"
#include "helper.h"
#include "port_3ds.h"
#include "sysinfo.h"
#include "touchui_3ds.h"
#include "stereo_auto_3ds.h"
#include "watchdog.h"
#include "worker.h"

void probe_init(void);  // port/probe_3ds.c

static PrintConsole g_debug_console;

// Linear memory (readable by the GPU and DSP) holds GPU buffers and textures,
// all MusyX samples (~15 MB, played by the DSP straight from memory) and the
// game's MEM1 arena (28 MB), so the GPU reads build-time converted textures
// in place. libctru's default is 32 MB; the rest of the application memory
// is the heap.
u32 __ctru_linear_heap_size = 48 * 1024 * 1024 + PORT_MEM1_SIZE;

// Log how much memory the 3DS gives us, since the game's budget depends on it
static void log_memory_info(void) {
    extern u32 __ctru_heap_size, __ctru_linear_heap_size;
    struct mallinfo mi = mallinfo();
    LOG_INFO("init", "memory: heap %lu KB (claimed %lu KB, free in claimed %lu KB), linear %lu KB (free %lu KB)",
             (unsigned long)(__ctru_heap_size / 1024), (unsigned long)(mi.arena / 1024),
             (unsigned long)(mi.fordblks / 1024), (unsigned long)(__ctru_linear_heap_size / 1024),
             (unsigned long)(linearSpaceFree() / 1024));
}

// Optional settings: KEY=VALUE lines in sdmc:/3ds/star3d/env.txt become
// environment variables (foxhollow reads its FOXHOLLOW_* options with getenv)
#define DATA_DIR     "sdmc:/3ds/star3d"
#define OLD_DATA_DIR "sdmc:/3ds/sfa_port"
#define ENV_FILE     DATA_DIR "/env.txt"
static void load_env_file(void) {
    FILE* f = fopen(ENV_FILE, "r");
    if (!f) {
        LOG_INFO("init", "no " ENV_FILE " (defaults and touch menu settings only)");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (line[0] == '#' || !eq || eq == line) continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        LOG_INFO("init", "env %s=%s", line, eq + 1);
    }
    fclose(f);
}

// The 400x240 top screen is 5:3. By default the game runs in its 16:9 mode
// (FOXHOLLOW_SCREEN_STYLE, port/src/foxhollow_config.c), which the renderer
// stretches over the full screen width (gx_init.c update_screen_mapping);
// S3D_WIDESCREEN=0 keeps the pillarboxed 4:3 image. The 16:9 projection
// aspect (widescreenAspect, also patched in newshadows.c) becomes 5:3 so the
// picture is not distorted.
extern u32 widescreenAspect;

static void setup_screen_style(void) {
    if (!getenv("FOXHOLLOW_SCREEN_STYLE"))
        setenv("FOXHOLLOW_SCREEN_STYLE", cfg_bool("S3D_WIDESCREEN", true) ? "wide" : "narrow", 1);
    float aspect = 400.0f / 240.0f;
    memcpy(&widescreenAspect, &aspect, sizeof(aspect));
}

// One-time migration: the SD card data folder used to be sdmc:/3ds/sfa_port.
// rename() can fail (e.g. a host handle open on the folder in an emulator), so
// fall back to copying; the old folder is then left in place. Runs before
// log_init(), so the outcome is logged later from s_migrate_note.
static const char* s_migrate_note;

static bool path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return false;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return false; }
    static char buf[16 * 1024];
    bool ok = true;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) remove(dst);
    return ok;
}

// Copies src into dst recursively; existing files in dst are kept
static bool copy_tree(const char* src, const char* dst) {
    DIR* d = opendir(src);
    if (!d) return false;
    mkdir(dst, 0777);
    bool ok = path_exists(dst);
    struct dirent* e;
    char from[512], to[512];
    while (ok && (e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(from, sizeof(from), "%s/%s", src, e->d_name);
        snprintf(to, sizeof(to), "%s/%s", dst, e->d_name);
        struct stat st;
        if (stat(from, &st) != 0) { ok = false; break; }
        if (S_ISDIR(st.st_mode)) ok = copy_tree(from, to);
        else if (!path_exists(to)) ok = copy_file(from, to);
    }
    closedir(d);
    return ok;
}

static bool dir_has_files(const char* path) {
    DIR* d = opendir(path);
    if (!d) return false;
    struct dirent* e;
    bool found = false;
    while (!found && (e = readdir(d)) != NULL)
        found = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    closedir(d);
    return found;
}

static void migrate_data_dir(void) {
    if (!path_exists(OLD_DATA_DIR)) return;
    if (!path_exists(DATA_DIR)) {
        if (rename(OLD_DATA_DIR, DATA_DIR) == 0) { s_migrate_note = "moved " OLD_DATA_DIR; return; }
        mkdir("sdmc:/3ds", 0777);
        s_migrate_note = copy_tree(OLD_DATA_DIR, DATA_DIR)
            ? "rename failed, copied " OLD_DATA_DIR " (old folder kept)"
            : "rename failed, copy of " OLD_DATA_DIR " incomplete (old folder kept)";
        return;
    }
    // An earlier failed migration may have left a new folder without saves
    if (!dir_has_files(DATA_DIR "/card") && dir_has_files(OLD_DATA_DIR "/card"))
        s_migrate_note = copy_tree(OLD_DATA_DIR "/card", DATA_DIR "/card")
            ? "copied saves from " OLD_DATA_DIR "/card"
            : "copy of saves from " OLD_DATA_DIR "/card incomplete";
}

int main(void) {
    // Enable New 3DS clock speed if available
    osSetSpeedupEnable(true);

    // Top screen: citro3d; bottom screen: the minimap (S3D_BOTTOM_HUD,
    // gx_init.c) or the debug console
    gfxInitDefault();

    migrate_data_dir();
    log_init();
    if (s_migrate_note) LOG_INFO("init", "data folder: %s", s_migrate_note);
    crash_install_handler();
    log_capture_stderr();
    prof_init();
    watchdog_start();

    load_env_file();
    touchui_load_settings();  // After env.txt, which wins
    stereo_auto_init();  // Snapshot the boot quality profile (S3D_STEREO_AUTO)
    netsrv_init();
    setup_screen_style();
    if (!cfg_bool("S3D_BOTTOM_HUD", true)) {
        consoleInit(GFX_BOTTOM, &g_debug_console);
        consoleSelect(&g_debug_console);
    }

    bool is_new_3ds = false;
    APT_CheckNew3DS(&is_new_3ds);
    LOG_INFO("init", "Star 3D, Stellaris engine (%s), %s", is_new_3ds ? "New 3DS" : "Old 3DS",
             envIsHomebrew() ? "started as .3dsx" : "installed title (CIA)");
    {
        // Hash of the code (savestate_snap.c); the header's date only changes
        // when log.c recompiles. tools/build_id.py prints it for an ELF
        extern u32 port_build_id(void);
        extern u32 port_fnv1a(const void* lo, const void* hi);
        LOG_INFO("init", "build id %08lX", (unsigned long)port_build_id());
        // The parts separately (tools/build_id.py prints the same three for an
        // ELF). Like the id, text skips the crt0 block the launcher writes
        // (bug 13); the gap is .text's page padding
        extern char __start__[], __text_end[], __rodata_start[], __exidx_start[];
        LOG_INFO("init", "image hash: text %08lX (%u B), gap %08lX (%u B), rodata %08lX (%u B)",
                 (unsigned long)port_fnv1a(__start__ + 0x20, __text_end), (unsigned)(__text_end - __start__ - 0x20),
                 (unsigned long)port_fnv1a(__text_end, __rodata_start), (unsigned)(__rodata_start - __text_end),
                 (unsigned long)port_fnv1a(__rodata_start, __exidx_start), (unsigned)(__exidx_start - __rodata_start));
    }
    sysinfo_log();
    log_memory_info();
    worker_init();  // Core 1 (MULTICORE_PLAN.md)
    helper_init();  // Core 1 fork-join helper (skinning, port/skin_3ds.c)
    probe_init();   // S3D_PROBE baseline, before the GPU and render thread start (port/probe_3ds.c)

    port_os_init();

    // Game data: the disc is extracted into the RomFS at build time. A data
    // file on the SD card (sdmc:/3ds/star3d/romfs.bin, from out/star3d-romfs.bin)
    // is preferred: then a code-only .3dsx (the "code" target) runs the full
    // game, so console tests only transfer the small code image over 3dslink
    // instead of the several-hundred-MB data blob every time.
    Result rc = 0xFFFFFFFF;
    {
        static const u16 sd_romfs_path[] = u"/3ds/star3d/romfs.bin";
        FS_Path archPath = { PATH_EMPTY, 1, "" };
        FS_Path filePath = { PATH_UTF16, sizeof(sd_romfs_path), sd_romfs_path };
        Handle fd = 0;
        Result orc = FSUSER_OpenFileDirectly(&fd, ARCHIVE_SDMC, archPath, filePath, FS_OPEN_READ, 0);
        if (R_SUCCEEDED(orc)) {
            rc = romfsMountFromFile(fd, 0, "romfs");
            if (R_FAILED(rc)) {
                LOG_WARN("init", "SD romfs mount failed (0x%08lX): using the embedded data", (unsigned)rc);
                FSFILE_Close(fd);
            } else {
                LOG_INFO("init", "game data: sdmc:/3ds/star3d/romfs.bin");
            }
        } else {
            LOG_INFO("init", "SD romfs open failed (0x%08lX)", (unsigned)orc);
        }
    }
    if (R_FAILED(rc)) {
        rc = romfsInit();
        if (R_FAILED(rc)) {
            LOG_ERROR("init", "romfsInit failed (0x%08lX): this build has no game data", rc);
        }
    }
    if (R_SUCCEEDED(rc)) {
        // Build-time conversion options (tools/extract_disc.py): the build id
        // only covers the code, this tells data variants apart
        FILE* f = fopen("romfs:/data_info.txt", "r");
        char line[160];
        if (f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            LOG_INFO("init", "game data: %s", line);
        }
        if (f) fclose(f);
    }

    if (!gx_init()) {
        LOG_ERROR("init", "graphics init failed");
        port_shutdown_and_exit(1);
    }
    gxrec_init();  // Render thread (core 2), started with the first frame
    log_memory_info();

    bench_run();
    gx_restore_state();
    gx_hwtest();
    gx_vbench();
    cfg_log_summary();

#ifdef GX_SELFTEST
    gx_selftest();
#endif
    LOG_INFO("init", "entering gameMain()");
    log_flush();

    // The game owns the main loop from here on; frames are delimited by
    // VIWaitForRetrace() (port/vi_3ds.c)
    port_frame_pump_init();
    static char arg0[] = "star3d";
    static char* argv[] = { arg0, NULL };
    int result = gameMain(1, argv);

    LOG_WARN("init", "gameMain() returned %d", result);
    port_shutdown_and_exit(result);
}
