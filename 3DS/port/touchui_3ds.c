// Touch screen UI on the bottom screen, in the style of the game's menus
// (dark blue panels with a gold frame). Three tabs:
// - Settings: the player-facing S3D_* switches (cfg_set, so the renderer picks
//   them up on the next frame) and logging, with the save states on top: Load
//   State, the three newest states (tap one to pick it), Save State and an FPS
//   readout. Changes are kept in sdmc:/3ds/star3d/settings.txt, loaded at boot
//   after env.txt.
//   Only the everyday settings are listed; the rest (renderer and debug
//   switches) keep their defaults unless S3D_MENU_ADVANCED=1 is in env.txt.
// - Map: the tab bar, the game's minimap (gx_hud_begin) and, right of it, the
//   game's C-menu ring, drawn there all the time (patches/dlls/engine/0). The
//   ring is also the touch pad for it: tap opens the menu, a left or right
//   flick turns to the next colour, an up or down flick picks an item, a double
//   tap uses it. The C-stick moves the camera instead (controls_3ds.c).
// - Cheats: applied once per frame by touchui_frame().
// citro2d draws after the game's frame (gx_ui_begin/gx_ui_end), touch input is
// read in VIWaitForRetrace right after hidScanInput.

#include <3ds.h>
#include <citro2d.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "diag_3ds.h"
#include "gx_init.h"
#include "log.h"
#include "savestate_3ds.h"
#include "remote_3ds.h"
#include "saveimport_3ds.h"
#include "touchui_3ds.h"
#include "watchdog.h"

#define SETTINGS_FILE "sdmc:/3ds/star3d/settings.txt"
// Version 2 (new defaults, hidden advanced rows): files written before it are
// ignored once, so everyone starts from the new defaults
#define SETTINGS_VERSION 2

enum { TAB_SETTINGS, TAB_MAP, TAB_CHEATS, TAB_COUNT };
enum { ROW_BOOL, ROW_CHOICE, ROW_LOG, ROW_ACTION, ROW_CHEAT, ROW_INFO, ROW_SLOTS, ROW_FPS };

typedef struct {
    const char* label;
    int kind;
    const char* key;      // S3D_* switch, or NULL
    int def;
    const int* choices;   // ROW_CHOICE: values cycled through, ending in INT32_MIN
    const char* const* names;  // ROW_CHOICE: shown instead of the value (indexed by value), or NULL
    int* cheat;           // ROW_CHEAT
    void (*action)(void); // ROW_ACTION
    bool advanced;        // Listed only with S3D_MENU_ADVANCED=1 (and then saved)
} Row;

int port_cheat_ohk;  // patches/dlls/objects/202/*.c.sed
static int cheat_health, cheat_magic, cheat_money, cheat_giant, cheat_tiny, cheat_moonjump;

static const int grass_choices[] = { 0, 2, 4, 8, 16, INT32_MIN };
static const int fur_choices[] = { 1, 2, 0, INT32_MIN };
static const char* const fur_names[] = { "Off", "Cutscene", "Always" };
static const int anim_lod_choices[] = { 1, 2, 0, INT32_MIN };
static const char* const anim_lod_names[] = { "Off", "On", "Strong" };
static const int lod_choices[] = { 0, 2, 4, -2, INT32_MIN };
// S3D_RENDER_THREAD (gx/gx_rec.h): 1 records and replays on the game thread
static const int rt_choices[] = { 2, 0, 1, INT32_MIN };
static const char* const rt_names[] = { "Off", "Test (1 core)", "Core 2" };
// S3D_VSH_LEAN bits: 1 affine texture coordinates, 2 skip unused slots
static const int lean_choices[] = { 3, 0, 1, 2, INT32_MIN };
static const char* const lean_names[] = { "Off", "Fast math", "Skip slots", "On" };
static const char* const lod_names[] = { "0", NULL, "+0.5", NULL, "+1" };
// S3D_DBG_MIP: sample every mipmapped texture at one level (debug)
static const int mip_choices[] = { 0, 1, 2, 3, 4, 5, INT32_MIN };
static const char* const mip_names[] = { "Off", "Level 0", "Level 1", "Level 2", "Level 3", "Level 4" };

static char s_status[48];
static int s_status_frames;

static void status(const char* msg) {
    snprintf(s_status, sizeof(s_status), "%s", msg);
    s_status_frames = 120;
}

static int s_slot;  // Picked save state slot, 0 = newest
static bool s_confirm_clean;
static char s_confirm_label[48];  // "Delete N old save states?"
static float s_fps;

static void do_save_state(void) {
    savestate_snap_request_save();
    s_slot = 0;
}
static void do_load_state(void) { savestate_snap_request_load(s_slot); }
static void do_clean(void) {
    int n = savestate_snap_old_count();
    if (n <= 0) {
        status("No old states to clean");
        return;
    }
    snprintf(s_confirm_label, sizeof(s_confirm_label), "Delete %d old save state%s?", n, n == 1 ? "" : "s");
    s_confirm_clean = true;
}
static void do_mark(void) {
    diag_mark();
    status("Snapshot written to Star3D.log");
}
static void do_heal(void) { status(savestate_cheat_heal() ? "Healed" : "No player"); }
static void open_import(void);

static const Row settings_rows[] = {
    { "Load State", ROW_ACTION, .action = do_load_state },  // the picked slot
    { "", ROW_SLOTS },
    { "Save State", ROW_ACTION, .action = do_save_state },
    { "FPS", ROW_FPS },
    // Defaults here must match the ones where the switch is read (the first
    // cfg_* lookup of a name fixes its default)
    { "30 FPS cap", ROW_BOOL, "S3D_FPS_CAP30", 1 },
    { "Adaptive frame pacing", ROW_BOOL, "S3D_FRAME_PACE_ADAPTIVE", 1 },
    { "Render thread", ROW_CHOICE, "S3D_RENDER_THREAD", 2, rt_choices, rt_names, .advanced = true },
    { "Shadows", ROW_BOOL, "S3D_SHADOWS", 1 },
    { "Far animation LOD", ROW_CHOICE, "S3D_ANIM_LOD", 2, anim_lod_choices, anim_lod_names, .advanced = true },
    { "Back-face culling", ROW_BOOL, "S3D_CULL", 1, .advanced = true },
    { "Fog", ROW_BOOL, "S3D_FOG", 1 },
    { "Per-pixel lighting", ROW_BOOL, "S3D_FRAG_LIGHT", 1, .advanced = true },
    { "Fur shading", ROW_BOOL, "S3D_FUR_SHADE", 1, .advanced = true },
    { "Fur wind", ROW_BOOL, "S3D_FUR_WIND", 1, .advanced = true },
    { "Fur layer budget", ROW_BOOL, "S3D_FUR_BUDGET", 1, .advanced = true },
    { "Fur", ROW_CHOICE, "S3D_FUR_MODE", 1, fur_choices, fur_names },
    { "Grass layers", ROW_CHOICE, "S3D_GRASS_BUDGET", 8, grass_choices },
    { "Water ripples", ROW_BOOL, "S3D_WATER_WARP", 1, .advanced = true },
    { "Caustics", ROW_BOOL, "S3D_CAUSTIC_WARP", 1, .advanced = true },
    { "Heat haze", ROW_BOOL, "S3D_SCREEN_WARP", 1, .advanced = true },
    { "Depth of field blur", ROW_BOOL, "S3D_GPU_BLUR", 1 },
    // S3D_STEREO_REDUCED: half the fur and grass layers and no blur while the
    // 3D slider is up. S3D_STEREO_AUTOCONV: the screen depth follows Fox
    { "3D: faster (fewer effects)", ROW_BOOL, "S3D_STEREO_REDUCED", 0 },
    { "3D: depth follows Fox", ROW_BOOL, "S3D_STEREO_AUTOCONV", 1 },
    { "Gyro aiming", ROW_BOOL, "S3D_GYRO_AIM", 1 },
    { "C-stick moves camera", ROW_BOOL, "S3D_CSTICK_CAMERA", 1 },
    { "Texture blur (LOD bias)", ROW_CHOICE, "S3D_TEX_LODBIAS_X4", 0, lod_choices, lod_names, .advanced = true },
    { "Lean shaders", ROW_CHOICE, "S3D_VSH_LEAN", 1, lean_choices, lean_names, .advanced = true },
    { "Debug: mip level", ROW_CHOICE, "S3D_DBG_MIP", 0, mip_choices, mip_names, .advanced = true },
    { "Logging", ROW_LOG },
    { "Log snapshot (mark)", ROW_ACTION, .action = do_mark },
    { "Import saves", ROW_ACTION, .action = open_import },
    // Last, away from the rest so it is not hit by accident (asks first)
    { "Clean old save states", ROW_ACTION, .action = do_clean },
};

// Settings > Clean old save states confirmation
static const Row confirm_rows[] = {
    { s_confirm_label, ROW_INFO },
    { "Yes, delete them", ROW_ACTION },
    { "Cancel", ROW_ACTION },
};

static const Row cheat_rows[] = {
    { "Infinite health", ROW_CHEAT, .cheat = &cheat_health },
    { "Infinite magic", ROW_CHEAT, .cheat = &cheat_magic },
    { "Max scarabs", ROW_CHEAT, .cheat = &cheat_money },
    { "One-hit kill", ROW_CHEAT, .cheat = &port_cheat_ohk },
    { "Giant Fox", ROW_CHEAT, .cheat = &cheat_giant },
    { "Tiny Fox", ROW_CHEAT, .cheat = &cheat_tiny },
    { "Moon jump (hold L)", ROW_CHEAT, .cheat = &cheat_moonjump },
    { "Heal now", ROW_ACTION, .action = do_heal },
};

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

// Settings rows shown: the advanced ones only with S3D_MENU_ADVANCED=1
static bool s_menu_advanced;
static Row s_settings_vis[COUNT(settings_rows)];
static int s_settings_nvis;

static void build_settings_rows(void) {
    s_menu_advanced = cfg_bool("S3D_MENU_ADVANCED", false);
    s_settings_nvis = 0;
    for (int i = 0; i < COUNT(settings_rows); i++)
        if (!settings_rows[i].advanced || s_menu_advanced) s_settings_vis[s_settings_nvis++] = settings_rows[i];
}

// Layout (bottom screen is 320x240)
#define TAB_H 30
#define ROW_H 24
#define LIST_Y (TAB_H + 6)
#define LIST_H (240 - LIST_Y - 4)

static bool s_ready, s_failed;
static int s_tab = TAB_MAP;
static int s_test_savestate, s_test_loadstate, s_test_frame, s_test_moonjump;
static int s_scroll[TAB_COUNT];  // pixels
static C2D_TextBuf s_textbuf;
static int s_touch_x0, s_touch_y0, s_touch_scroll0, s_touch_moved;
static bool s_touching, s_touch_prev;
static bool s_log_default_pending;

// --- Import saves ----------------------------------------------------------------
// Settings > Import saves replaces the Settings rows with the slots found in
// SAVEIMPORT_DIR; picking one imports it into the first free card slot, or asks
// which slot to replace when the card is full (the card is backed up first).

enum { IMPORT_OFF, IMPORT_LIST, IMPORT_REPLACE };
static int s_import_mode, s_import_count, s_import_pick;
static ImportEntry s_imports[SAVEIMPORT_MAX];
static Row s_import_rows[SAVEIMPORT_MAX + 2];
static char s_import_labels[SAVEIMPORT_MAX + 2][64];
static int s_import_nrows;

static void import_row(int kind, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void import_row(int kind, const char* fmt, ...) {
    int i = s_import_nrows++;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_import_labels[i], sizeof(s_import_labels[i]), fmt, ap);
    va_end(ap);
    s_import_rows[i] = (Row){ .label = s_import_labels[i], .kind = kind };
}

static void import_set_mode(int mode) {
    s_import_mode = mode;
    s_import_nrows = 0;
    s_scroll[TAB_SETTINGS] = 0;
    if (mode == IMPORT_LIST) {
        import_row(ROW_ACTION, "< Back");
        if (s_import_count == 0) import_row(ROW_INFO, "No saves in /3ds/star3d/saves");
        for (int i = 0; i < s_import_count; i++) {
            const ImportEntry* e = &s_imports[i];
            import_row(ROW_ACTION, "%s %3d%% %2dh  %.22s", e->name, e->percent, e->minutes / 60, e->file);
        }
    } else if (mode == IMPORT_REPLACE) {
        char names[3][4];
        if (!saveimport_current(names)) for (int i = 0; i < 3; i++) strcpy(names[i], "---");
        import_row(ROW_INFO, "Card full: replace which slot?");
        for (int i = 0; i < 3; i++) import_row(ROW_ACTION, "Slot %d: %s", i + 1, names[i]);
        import_row(ROW_ACTION, "Cancel");
    }
}

static void open_import(void) {
    s_import_count = saveimport_scan(s_imports, SAVEIMPORT_MAX);
    import_set_mode(IMPORT_LIST);
}

static void import_do(int dst) {
    char msg[48];
    saveimport_run(&s_imports[s_import_pick], dst, msg, sizeof(msg));
    status(msg);
    import_set_mode(IMPORT_OFF);
}

static void import_tap(int idx) {
    if (s_import_rows[idx].kind != ROW_ACTION) return;
    if (s_import_mode == IMPORT_LIST) {
        if (idx == 0) { import_set_mode(IMPORT_OFF); return; }
        s_import_pick = idx - (s_import_count ? 1 : 2);
        if (saveimport_free_slot() < 0) import_set_mode(IMPORT_REPLACE);
        else import_do(-1);
    } else if (idx >= 1 && idx <= 3) {
        import_do(idx - 1);
    } else {
        import_set_mode(IMPORT_LIST);
    }
}

// Remote "import" command: "import ui", "import list" or "import N [SLOT]" (N from the list,
// SLOT 1..3, default the first free one)
void touchui_remote_import(const char* args) {
    if (!strncmp(args, "ui", 2)) {  // open the menu view (screenshots)
        s_tab = TAB_SETTINGS;
        open_import();
        return;
    }
    s_import_count = saveimport_scan(s_imports, SAVEIMPORT_MAX);
    if (!strncmp(args, "list", 4)) {
        for (int i = 0; i < s_import_count; i++)
            LOG_WARN("import", "%d: %s %d%% %dmin %s slot %d", i, s_imports[i].name, s_imports[i].percent,
                     s_imports[i].minutes, s_imports[i].file, s_imports[i].slot + 1);
        return;
    }
    int n = -1, slot = 0;
    sscanf(args, "%d %d", &n, &slot);
    if (n < 0 || n >= s_import_count) { LOG_WARN("import", "no entry %d", n); return; }
    char msg[48];
    bool ok = saveimport_run(&s_imports[n], slot - 1, msg, sizeof(msg));
    LOG_WARN("import", "%s: %s", ok ? "ok" : "failed", msg);
    status(msg);
}

// --- Settings persistence ----------------------------------------------------

static int row_value(const Row* r) {
    switch (r->kind) {
    case ROW_BOOL: return cfg_bool(r->key, r->def) ? 1 : 0;
    case ROW_CHOICE: return cfg_int(r->key, r->def);
    case ROW_LOG: return log_get_verbose() ? 1 : 0;
    case ROW_CHEAT: return *r->cheat;
    default: return 0;
    }
}

static void save_settings(void) {
    FILE* f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "# Star 3D settings (touch screen menu); env.txt overrides these\n");
    fprintf(f, "S3D_SETTINGS_VERSION=%d\n", SETTINGS_VERSION);
    for (int i = 0; i < COUNT(settings_rows); i++) {
        const Row* r = &settings_rows[i];
        if (r->advanced && !s_menu_advanced) continue;  // Not shown, keeps its default
        if (r->key) fprintf(f, "%s=%d\n", r->key, row_value(r));
        else if (r->kind == ROW_LOG) fprintf(f, "S3D_LOG_VERBOSE=%d\n", row_value(r));
    }
    fclose(f);
}

static bool is_advanced_key(const char* key) {
    for (int i = 0; i < COUNT(settings_rows); i++)
        if (settings_rows[i].key && !strcmp(settings_rows[i].key, key)) return settings_rows[i].advanced;
    return false;
}

void touchui_load_settings(void) {
    build_settings_rows();
    FILE* f = fopen(SETTINGS_FILE, "r");
    if (f) {
        char line[128];
        int version = 0;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "S3D_SETTINGS_VERSION=", 21)) version = atoi(line + 21);
        if (version < SETTINGS_VERSION) {
            LOG_INFO("touchui", "settings.txt is from an older build (version %d), using the new defaults", version);
            fclose(f);
            f = NULL;
        } else {
            rewind(f);
        }
    }
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = 0;
            char* eq = strchr(line, '=');
            if (line[0] == '#' || !eq || eq == line) continue;
            *eq = 0;
            // Hidden rows keep their defaults (a value saved while
            // S3D_MENU_ADVANCED was on only counts while it still is)
            if (!s_menu_advanced && is_advanced_key(line)) continue;
            // Removed from the menu, so a value saved by an older build must not
            // come back: reduced resolution (freezes the top screen, then
            // crashes) and the GPU overlap toggles (bug 19 followed turning
            // overlap off). env.txt can still set them
            if (!strcmp(line, "S3D_LOWRES") || !strcmp(line, "S3D_GPU_PIPELINE") ||
                !strcmp(line, "S3D_GPU_SPLIT_FULLFLUSH"))
                continue;
            if (!strcmp(line, "S3D_FUR_GAMEPLAY")) {    // Old "Fur always on", now Fur: Always
                if (atoi(eq + 1)) setenv("S3D_FUR_MODE", "2", 0);
                continue;
            }
            setenv(line, eq + 1, 0);
        }
        fclose(f);
    }
    // Logging is off by default (Settings > Logging, or S3D_LOG_VERBOSE=1 in
    // env.txt for test builds). Boot is logged either way: it is switched off
    // on the first frame, after the build id and the init lines
    if (getenv("S3D_LOG_VERBOSE") && atoi(getenv("S3D_LOG_VERBOSE"))) log_set_verbose(true);
    else s_log_default_pending = true;
    // Test hooks (env.txt), the emulator test script cannot tap the screen:
    // S3D_TOUCHUI_TAB=0..2, S3D_TEST_CHEATS=letters (h)ealth (m)agic (s)carabs
    // (o)ne-hit (g)iant (t)iny moon (j)ump, S3D_TEST_SAVESTATE=frame (save there,
    // load slot 1 300 later), S3D_TEST_LOADSTATE=frame (load slot 1),
    // S3D_TEST_MOONJUMP=1 (L counts as held)
    const char* e;
    if ((e = getenv("S3D_TOUCHUI_TAB"))) s_tab = atoi(e) % TAB_COUNT;
    if ((e = getenv("S3D_TEST_CHEATS"))) {
        cheat_health = strchr(e, 'h') != NULL; cheat_magic = strchr(e, 'm') != NULL;
        cheat_money = strchr(e, 's') != NULL; port_cheat_ohk = strchr(e, 'o') != NULL;
        cheat_giant = strchr(e, 'g') != NULL; cheat_tiny = strchr(e, 't') != NULL;
        cheat_moonjump = strchr(e, 'j') != NULL;
    }
    if ((e = getenv("S3D_TEST_MOONJUMP"))) s_test_moonjump = atoi(e);
    if ((e = getenv("S3D_TEST_SAVESTATE"))) s_test_savestate = atoi(e);
    if ((e = getenv("S3D_TEST_LOADSTATE"))) s_test_loadstate = atoi(e); // load only, at this frame
}

// --- Cheats ---------------------------------------------------------------------

static void item_pad_frame(void);

void touchui_frame(void) {
    if (s_log_default_pending) {
        s_log_default_pending = false;
        LOG_WARN("touchui", "logging off from here (Settings > Logging, or S3D_LOG_VERBOSE=1 in env.txt)");
        log_set_verbose(false);
    }
    item_pad_frame();
    remote_frame();
    if (s_test_savestate > 0) {
        s_test_frame++;
        if (s_test_frame == s_test_savestate) savestate_snap_request_save();
        if (s_test_frame == s_test_savestate + 300) savestate_snap_request_load(0);
    }
    if (s_test_loadstate > 0 && ++s_test_frame == s_test_loadstate) savestate_snap_request_load(0);
    savestate_cheats(cheat_health, cheat_magic, cheat_money, cheat_giant ? 2 : cheat_tiny ? -1 : 0);
    savestate_moonjump(cheat_moonjump && (s_test_moonjump || (hidKeysHeld() & KEY_L) || (remote_pad_buttons & PAD_TRIGGER_L)));
    savestate_frame();
    const char* msg = savestate_snap_message();
    if (msg) {
        status(msg);
        LOG_INFO("touchui", "%s", msg);
    }
    if (s_status_frames > 0) s_status_frames--;
    // FPS readout, averaged over half a second
    static u64 t0;
    static int frames;
    u64 now = svcGetSystemTick();
    if (!t0) t0 = now;
    frames++;
    double ms = (double)(now - t0) / CPU_TICKS_PER_MSEC;
    if (ms >= 500.0) {
        s_fps = (float)(frames * 1000.0 / ms);
        frames = 0;
        t0 = now;
    }
}

// --- C-menu ring (Map tab) ---------------------------------------------------------
// The C-stick menu (the red / blue / yellow ring: items, staff powers,
// Tricky), which the C-stick no longer opens (controls_3ds.c), is drawn right
// of the minimap every frame: hudDrawButtons' ring block runs inside a HUD
// pass mapped to RING_* (patches/dlls/engine/0/0.c.sed), with the fade forced
// to full so it stays visible while the menu is closed (B closes the menu, the
// ring stays). The ring's area is the touch pad. The gestures become short
// C-stick and A pulses for the game's own menu code (dlls/engine/0/0.c
// GameUI_frameEnd and cMenuRun), merged by PADRead:
// - tap: open the menu (any C-stick tilt over 5 does, the game picks the page
//   from the context like before)
// - flick left / right: the next colour (C-stick X past 15, edge triggered)
// - flick up / down: the previous / next item (C-stick Y past 10, edge
//   triggered; under 60 so one flick is one step)
// - double tap: use the item (A), only once the menu is fully open so A
//   never reaches Fox
// A flick or a use while the menu is closed opens it first and runs once it
// is fully open (s_pending).

extern u8 cMenuOpen;             // dlls/engine/0/0.c
extern short gCMenuOpenAnim;
extern s16 gCMenuOpenAnimMax;
extern s16 cMenuFadeCounter;
extern u8 cMenuEnabled;
void* Obj_GetPlayerObject(void);

// The ring's EFB region (about x 535-613, y 105-271 on the GameCube screen:
// the ring, its item frame and the page icon) lands right of the minimap at
// RING_SCALE (about x 222-308, y 42-223 of the bottom screen; 1.15 touched
// the bottom edge in Azahar)
#define RING_SRC_X 532.0f
#define RING_SRC_Y 100.0f
#define RING_SCALE 1.1f
#define RING_DST_X 220.0f
#define RING_DST_Y 36.0f

#define IPAD_X 214
#define IPAD_Y (TAB_H + 2)
#define IPAD_W (320 - IPAD_X)
#define IPAD_H (240 - IPAD_Y)
#define FLICK_PX 18
#define DOUBLE_TAP_MS 400
#define PULSE_FRAMES 2  // Game frames a gesture holds the stick or A

static s8 s_vstick[2];
static int s_vstick_frames;  // Counts PULSE_FRAMES + 1 down; the last frame is centred
static int s_va_frames;
static bool s_ipad_touch, s_ipad_flicked;
static int s_ipad_x0, s_ipad_y0;
static u64 s_last_tap;
static int s_last_tap_x, s_last_tap_y;
enum { PENDING_NONE, PENDING_STICK, PENDING_USE };
static int s_pending, s_pending_frames;
static s8 s_pending_stick[2];

static bool cmenu_ready(void) {
    return cMenuOpen && gCMenuOpenAnim == gCMenuOpenAnimMax;
}

static bool cmenu_closed(void) {
    return !cMenuOpen && gCMenuOpenAnim == 0;
}

// Item counts of the ring's icons, drawn by the touch UI: written by
// hudDrawButtons (game thread) into one buffer, published at the frame end
#define RING_COUNTS 8
typedef struct {
    char text[8];
    float x, y;
    u8 alpha;
} RingCount;
static RingCount s_counts[2][RING_COUNTS];
static int s_ncounts[2];
static volatile int s_counts_pub;  // Buffer touchui_draw reads
static s16 s_saved_fade;

// hudDrawButtons, before the ring block: 1 = the block runs on the bottom screen
int port_cmenu_hud_begin(void) {
    static int on = 1;
    CFG_LIVE(on, cfg_bool("S3D_CMENU_BOTTOM", true));
    if (!on || !cMenuEnabled || !gx_hud_enabled() || !gx_hud_begin()) return 0;
    gx_hud_set_map(RING_SRC_X, RING_SRC_Y, RING_SCALE, RING_DST_X, RING_DST_Y);
    s_saved_fade = cMenuFadeCounter;
    cMenuFadeCounter = 0xff;
    return 1;
}

void port_cmenu_hud_end(void) {
    cMenuFadeCounter = s_saved_fade;
    gx_hud_end();
}

// An item count (game text, EFB position of its top left)
void port_cmenu_count(const char* text, int x, int y, int alpha) {
    int b = s_counts_pub ^ 1;
    if (s_ncounts[b] >= RING_COUNTS || alpha <= 0) return;
    RingCount* c = &s_counts[b][s_ncounts[b]++];
    snprintf(c->text, sizeof(c->text), "%s", text);
    c->x = RING_DST_X + (x - RING_SRC_X) * RING_SCALE;
    c->y = RING_DST_Y + (y - RING_SRC_Y) * RING_SCALE;
    c->alpha = alpha > 255 ? 255 : (u8)alpha;
}

static bool vstick(int x, int y) {
    if (s_vstick_frames) return false;  // Still pulsing, or the centred frame after it
    s_vstick[0] = (s8)x;
    s_vstick[1] = (s8)y;
    s_vstick_frames = PULSE_FRAMES + 1;
    return true;
}

// For PADRead (pad_3ds.c), any number of times per frame
void touchui_pad_state(s8* cx, s8* cy, u16* buttons) {
    bool on = s_vstick_frames > 1;
    *cx = on ? s_vstick[0] : 0;
    *cy = on ? s_vstick[1] : 0;
    *buttons = s_va_frames > 0 ? PAD_BUTTON_A : 0;
}

static void item_pad_frame(void) {
    // Publish the counts of the frame that just ended
    int b = s_counts_pub ^ 1;
    s_counts_pub = b;
    s_ncounts[b ^ 1] = 0;
    if (s_vstick_frames > 0) s_vstick_frames--;
    if (s_va_frames > 0) s_va_frames--;
    // A gesture made while the menu was closed, once it is open (or given up
    // after 2 s, e.g. when the game did not open it)
    if (s_pending && --s_pending_frames <= 0) s_pending = PENDING_NONE;
    if (s_pending && cmenu_ready() && !s_vstick_frames) {
        if (s_pending == PENDING_USE) s_va_frames = PULSE_FRAMES;
        else vstick(s_pending_stick[0], s_pending_stick[1]);
        s_pending = PENDING_NONE;
    }
}

// Opens the closed menu and runs the gesture once it is open
static void open_then(int what, int x, int y) {
    if (!cMenuOpen && gCMenuOpenAnim != 0) return;  // Closing: drop it
    if (cmenu_closed()) vstick(8, 0);  // Else it is opening already
    s_pending = what;
    s_pending_stick[0] = (s8)x;
    s_pending_stick[1] = (s8)y;
    s_pending_frames = 60;
}

static void ipad_tap(int x, int y) {
    u64 now = svcGetSystemTick();
    bool dbl = s_last_tap && now - s_last_tap < (u64)DOUBLE_TAP_MS * CPU_TICKS_PER_MSEC &&
               abs(x - s_last_tap_x) < 40 && abs(y - s_last_tap_y) < 40;
    if (dbl) {
        s_last_tap = 0;
        if (cmenu_ready()) s_va_frames = PULSE_FRAMES;
        else open_then(PENDING_USE, 0, 0);  // The first tap is still opening it
        return;
    }
    s_last_tap = now;
    s_last_tap_x = x;
    s_last_tap_y = y;
    if (cmenu_closed()) vstick(8, 0);
}

// dx, dy in screen pixels (y down)
static void ipad_flick(int dx, int dy) {
    s_last_tap = 0;
    int x = 0, y = 0;
    if (abs(dx) >= abs(dy)) x = dx > 0 ? 0x30 : -0x30;
    else y = dy > 0 ? -0x30 : 0x30;  // Stick down picks the next item
    if (cmenu_ready()) vstick(x, y);
    else open_then(PENDING_STICK, x, y);
}

static bool in_item_pad(int x, int y) {
    return s_tab == TAB_MAP && x >= IPAD_X && x < IPAD_X + IPAD_W && y >= IPAD_Y && y < IPAD_Y + IPAD_H;
}

// Bumped by touch input, config changes and animations: touchui_draw skips
// frames where nothing changed (the render thread pays ~1 ms per frame for
// the bottom screen, A1C2F0C5)
static u32 s_ui_gen;

// Remote "flick DX DY" (remote_3ds.c)
void touchui_remote_flick(int dx, int dy) {
    s_ui_gen++;
    ipad_flick(dx, dy);
}

// --- Input ----------------------------------------------------------------------

static const Row* tab_rows(int tab, int* count) {
    if (tab == TAB_SETTINGS && s_confirm_clean) { *count = COUNT(confirm_rows); return confirm_rows; }
    if (tab == TAB_SETTINGS && s_import_mode) { *count = s_import_nrows; return s_import_rows; }
    if (tab == TAB_SETTINGS) {
        if (!s_settings_nvis) build_settings_rows();
        *count = s_settings_nvis;
        return s_settings_vis;
    }
    if (tab == TAB_CHEATS) { *count = COUNT(cheat_rows); return cheat_rows; }
    *count = 0;
    return NULL;
}

static int max_scroll(int tab) {
    int n;
    tab_rows(tab, &n);
    int m = n * ROW_H - LIST_H;
    return m > 0 ? m : 0;
}

static void activate(const Row* r) {
    switch (r->kind) {
    case ROW_BOOL:
        cfg_set(r->key, !row_value(r));
        save_settings();
        break;
    case ROW_CHOICE: {
        int v = row_value(r), i = 0;
        while (r->choices[i] != INT32_MIN && r->choices[i] != v) i++;
        if (r->choices[i] != INT32_MIN) i++;
        if (r->choices[i] == INT32_MIN) i = 0;
        cfg_set(r->key, r->choices[i]);
        save_settings();
        break;
    }
    case ROW_LOG:
        log_set_verbose(!log_get_verbose());
        save_settings();
        break;
    case ROW_CHEAT:
        *r->cheat = !*r->cheat;
        if (r->cheat == &cheat_giant && cheat_giant) cheat_tiny = 0;
        if (r->cheat == &cheat_tiny && cheat_tiny) cheat_giant = 0;
        break;
    case ROW_ACTION:
        r->action();
        break;
    case ROW_SLOTS: {
        int s = (s_touch_x0 - 6) * SAVESTATE_SLOTS / 308;
        s_slot = s < 0 ? 0 : s >= SAVESTATE_SLOTS ? SAVESTATE_SLOTS - 1 : s;
        break;
    }
    }
}

static void confirm_tap(int idx) {
    if (idx == 1) {
        char msg[48];
        snprintf(msg, sizeof(msg), "Deleted %d old states", savestate_snap_clean());
        status(msg);
    }
    if (idx >= 1) s_confirm_clean = false;
}

// Taps the row under (x, y), y below the tab bar
static void tap_row(int x, int y) {
    int n;
    const Row* rows = tab_rows(s_tab, &n);
    int idx = (y - LIST_Y + s_scroll[s_tab]) / ROW_H;
    if (!rows || y < LIST_Y || idx < 0 || idx >= n) return;
    s_touch_x0 = x;
    if (s_tab == TAB_SETTINGS && s_confirm_clean) confirm_tap(idx);
    else if (s_tab == TAB_SETTINGS && s_import_mode) import_tap(idx);
    else activate(&rows[idx]);
}

// Remote "tap X Y" (remote_3ds.c), run on the next touchui_input
static int s_rtap_x = -1, s_rtap_y;
void touchui_remote_tap(int x, int y) {
    s_rtap_x = x;
    s_rtap_y = y;
    s_ui_gen++;
}

void touchui_input(void) {
    if (!gx_hud_enabled()) return;
    if (s_rtap_x >= 0) {
        int x = s_rtap_x, y = s_rtap_y;
        s_rtap_x = -1;
        s_ui_gen++;
        if (y < TAB_H) {
            s_tab = x * TAB_COUNT / 320;
            if (s_tab >= TAB_COUNT) s_tab = TAB_COUNT - 1;
            s_confirm_clean = false;
        } else if (in_item_pad(x, y)) {
            ipad_tap(x, y);
        } else tap_row(x, y);
        return;
    }
    // Edges from the held state: PADRead scans the HID too, so hidKeysDown
    // here can miss a touch that started between the two scans
    u32 held = hidKeysHeld();
    bool touch_down = (held & KEY_TOUCH) && !s_touch_prev;
    s_touch_prev = (held & KEY_TOUCH) != 0;
    touchPosition tp;
    hidTouchRead(&tp);
    if (touch_down && in_item_pad(tp.px, tp.py)) {
        s_ipad_touch = true;
        s_ipad_flicked = false;
        s_ipad_x0 = tp.px;
        s_ipad_y0 = tp.py;
        s_ui_gen++;
        return;
    }
    if (s_ipad_touch) {
        if (held & KEY_TOUCH) {
            int dx = tp.px - s_ipad_x0, dy = tp.py - s_ipad_y0;
            if (abs(dx) >= FLICK_PX || abs(dy) >= FLICK_PX) {
                ipad_flick(dx, dy);
                s_ui_gen++;
                // Keep flicking without lifting: measure the next one from here
                s_ipad_flicked = true;
                s_ipad_x0 = tp.px;
                s_ipad_y0 = tp.py;
            }
            return;
        }
        s_ipad_touch = false;
        if (!s_ipad_flicked) {
            ipad_tap(s_ipad_x0, s_ipad_y0);
            s_ui_gen++;
        }
        return;
    }
    if (touch_down) {
        s_ui_gen++;
        if (tp.py < TAB_H) {
            s_tab = tp.px * TAB_COUNT / 320;
            s_confirm_clean = false;
            if (s_tab >= TAB_COUNT) s_tab = TAB_COUNT - 1;
            s_touching = false;
            return;
        }
        s_touching = true;
        s_touch_x0 = tp.px;
        s_touch_y0 = tp.py;
        s_touch_scroll0 = s_scroll[s_tab];
        s_touch_moved = 0;
        return;
    }
    if (!s_touching) return;
    if (held & KEY_TOUCH) {
        int dy = tp.py - s_touch_y0;
        if (abs(dy) > 6) s_touch_moved = 1;
        if (s_touch_moved) {
            int s = s_touch_scroll0 - dy, m = max_scroll(s_tab);
            s_scroll[s_tab] = s < 0 ? 0 : s > m ? m : s;
        }
        return;
    }
    // Released: a tap (no drag) activates the row under the start point
    s_touching = false;
    if (!s_touch_moved) tap_row(s_touch_x0, s_touch_y0);
}

// --- Drawing --------------------------------------------------------------------

#define COL(r, g, b, a) C2D_Color32(r, g, b, a)
#define PANEL COL(16, 28, 72, 235)
#define PANEL_HI COL(36, 60, 130, 245)
#define GOLD COL(232, 184, 72, 255)
#define GOLD_DIM COL(140, 104, 40, 255)
#define TEXT COL(240, 240, 255, 255)
#define TEXT_DIM COL(150, 160, 200, 255)
#define ON_COL COL(120, 230, 120, 255)
#define OFF_COL COL(230, 110, 100, 255)

static void frame_rect(float x, float y, float w, float h, u32 fill, u32 border) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, border);
    C2D_DrawRectSolid(x + 2, y + 2, 0.5f, w - 4, h - 4, fill);
}

// Parsed texts kept across frames (S3D_RT_MEMO bit 8, MULTICORE_PLAN.md
// "Render-thread memos"): parsing and optimizing every row's text each frame
// cost the render thread ~1 ms on the Settings tab (5C202467). Looked up by
// content; when the buffer or the table fills up, everything is parsed again
#define TEXT_CACHE 64
static C2D_TextBuf s_text_cache_buf;
static struct {
    char str[40];
    C2D_Text t;
} s_text_cache[TEXT_CACHE];
static int s_text_cached;

static bool text_cached(const char* str, C2D_Text* out) {
    if (strlen(str) >= sizeof(s_text_cache[0].str)) return false;
    if (!s_text_cache_buf && !(s_text_cache_buf = C2D_TextBufNew(1024))) return false;
    for (int i = 0; i < s_text_cached; i++) {
        if (strcmp(s_text_cache[i].str, str) == 0) {
            *out = s_text_cache[i].t;
            return true;
        }
    }
    if (s_text_cached == TEXT_CACHE || C2D_TextBufGetNumGlyphs(s_text_cache_buf) + strlen(str) > 1024) {
        // The texts drawn so far this frame already emitted their vertices
        C2D_TextBufClear(s_text_cache_buf);
        s_text_cached = 0;
    }
    C2D_TextParse(&s_text_cache[s_text_cached].t, s_text_cache_buf, str);  // Room checked above
    C2D_TextOptimize(&s_text_cache[s_text_cached].t);
    strcpy(s_text_cache[s_text_cached].str, str);
    *out = s_text_cache[s_text_cached++].t;
    return true;
}

static bool s_text_memo = true;

static void text(const char* str, float x, float y, float size, u32 color, bool right) {
    C2D_Text t;
    if (!s_text_memo || !text_cached(str, &t)) {
        C2D_TextParse(&t, s_textbuf, str);
        C2D_TextOptimize(&t);
    }
    float w = 0;
    if (right) C2D_TextGetDimensions(&t, size, size, &w, NULL);
    C2D_DrawText(&t, C2D_WithColor, x - w, y, 0.5f, size, size, color);
}

static void draw_tabs(void) {
    static const char* names[TAB_COUNT] = { "Settings", "Map", "Cheats" };
    float w = 320.0f / TAB_COUNT;
    for (int i = 0; i < TAB_COUNT; i++) {
        bool sel = i == s_tab;
        frame_rect(i * w + 1, 1, w - 2, TAB_H - 2, sel ? PANEL_HI : PANEL, sel ? GOLD : GOLD_DIM);
        C2D_Text t;
        if (!s_text_memo || !text_cached(names[i], &t)) {
            C2D_TextParse(&t, s_textbuf, names[i]);
            C2D_TextOptimize(&t);
        }
        float tw;
        C2D_TextGetDimensions(&t, 0.6f, 0.6f, &tw, NULL);
        C2D_DrawText(&t, C2D_WithColor, i * w + (w - tw) / 2, 7, 0.5f, 0.6f, 0.6f, sel ? GOLD : TEXT_DIM);
    }
}

// The three newest save states, the picked one framed in gold
static void draw_slots(float y) {
    char labels[SAVESTATE_SLOTS][24];
    int n = savestate_snap_slots(labels);
    float w = 308.0f / SAVESTATE_SLOTS;
    for (int i = 0; i < SAVESTATE_SLOTS; i++) {
        bool sel = i == s_slot;
        float x = 6 + i * w;
        frame_rect(x + 2, y + 1, w - 4, ROW_H - 2, sel ? PANEL_HI : PANEL, sel ? GOLD : GOLD_DIM);
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        text(num, x + 8, y + 4, 0.55f, sel ? GOLD : TEXT, false);
        text(i < n ? labels[i] : "empty", x + w - 8, y + 6, 0.45f, i < n ? TEXT : TEXT_DIM, true);
    }
}

// names[] is indexed by value and has an entry for every non-negative choice
static const char* choice_name(const Row* r, int v) {
    if (!r->names || v < 0) return NULL;
    for (int i = 0; r->choices[i] != INT32_MIN; i++)
        if (r->choices[i] == v) return r->names[v];
    return NULL;
}

// Map tab: the ring's item counts (the ring itself is drawn by the game) and
// the gestures, under the minimap
static void draw_ring_extras(void) {
    int b = s_counts_pub;
    for (int i = 0; i < s_ncounts[b]; i++) {
        const RingCount* c = &s_counts[b][i];
        if (c->y < IPAD_Y || c->y > 232) continue;
        text(c->text, c->x + 1, c->y + 1, 0.45f, COL(0, 0, 0, c->alpha), false);
        text(c->text, c->x, c->y, 0.45f, COL(255, 255, 255, c->alpha), false);
    }
    text("Ring: tap = open, tap twice = use", 6, 204, 0.42f, TEXT_DIM, false);
    text("Flick < > = colour, flick ^ v = item", 6, 220, 0.42f, TEXT_DIM, false);
}

static void draw_rows(void) {
    int n;
    const Row* rows = tab_rows(s_tab, &n);
    frame_rect(2, TAB_H + 2, 316, 240 - TAB_H - 4, PANEL, GOLD_DIM);
    C2D_SetTintMode(C2D_TintSolid);
    for (int i = 0; i < n; i++) {
        float y = LIST_Y + i * ROW_H - s_scroll[s_tab];
        if (y + ROW_H <= LIST_Y || y >= LIST_Y + LIST_H) continue;
        const Row* r = &rows[i];
        if (r->kind == ROW_SLOTS) {
            draw_slots(y);
            continue;
        }
        if (i & 1) C2D_DrawRectSolid(6, y, 0.5f, 308, ROW_H, COL(255, 255, 255, 16));
        text(r->label, 12, y + 4, 0.55f, TEXT, false);
        char val[16];
        u32 col = TEXT;
        switch (r->kind) {
        case ROW_BOOL: case ROW_LOG: case ROW_CHEAT:
            snprintf(val, sizeof(val), row_value(r) ? "On" : "Off");
            col = row_value(r) ? ON_COL : OFF_COL;
            break;
        case ROW_CHOICE:
            if (choice_name(r, row_value(r))) snprintf(val, sizeof(val), "%s", choice_name(r, row_value(r)));
            else snprintf(val, sizeof(val), "%d", row_value(r));
            col = GOLD;
            break;
        case ROW_INFO:
            val[0] = 0;
            break;
        case ROW_FPS:
            snprintf(val, sizeof(val), "%.1f", s_fps);
            col = s_fps >= 28.0f ? ON_COL : s_fps >= 18.0f ? GOLD : OFF_COL;
            break;
        default:
            snprintf(val, sizeof(val), s_import_mode || s_confirm_clean ? "" : "Tap");
            col = GOLD;
            break;
        }
        text(val, 308, y + 4, 0.55f, col, true);
    }
    // Scroll bar
    int m = max_scroll(s_tab);
    if (m > 0) {
        float bar_h = LIST_H * (float)LIST_H / (LIST_H + m);
        float bar_y = LIST_Y + (LIST_H - bar_h) * s_scroll[s_tab] / m;
        C2D_DrawRectSolid(313, bar_y, 0.5f, 3, bar_h, GOLD_DIM);
    }
}

// Phases (watchdog.h) split the draw for the probe's render-thread sampler.
// S3D_TOUCHUI_DRAW=0 skips it (A/B S3D_TOUCHUI_AB, port/diag_3ds.c: what the
// menu costs the render thread; MULTICORE_PLAN.md "Render-thread steps")
void touchui_draw(void) {
    static int draw = 1;
    CFG_LIVE(draw, cfg_int("S3D_TOUCHUI_DRAW", 1));
    static int memo = 15;
    CFG_LIVE(memo, cfg_int("S3D_RT_MEMO", 15));
    s_text_memo = (memo & 8) != 0;
    static int logged_tab = -1;
    if (s_tab != logged_tab) {
        logged_tab = s_tab;
        s_ui_gen++;
        LOG_INFO("touchui", "tab %d (%s)", s_tab, s_tab == TAB_SETTINGS ? "settings" : s_tab == TAB_MAP ? "map" : "cheats");
    }
    if (s_failed || !draw) return;
    // Skip unchanged frames: the touch screen content is static while the
    // menu is closed, nothing is touched and the item counts and settings
    // did not change. A safety net redraws every 30 frames.
    static u32 s_drawn_gen, s_drawn_cfggen, s_drawn_frame;
    static RingCount s_drawn_counts[RING_COUNTS];
    static int s_drawn_ncounts;
    static bool s_drawn_valid;
    u32 gen = s_ui_gen;
    if (!cmenu_ready() && !cmenu_closed()) gen++;  // Ring open/close animation
    if (cfg_gen() != s_drawn_cfggen) gen++;        // Live settings may be listed
    const RingCount* cur = s_counts[s_counts_pub];
    int ncur = s_ncounts[s_counts_pub];
    bool counts_same = s_drawn_valid && ncur == s_drawn_ncounts &&
                       memcmp(cur, s_drawn_counts, (size_t)ncur * sizeof(RingCount)) == 0;
    bool skip = s_drawn_valid && gen == s_drawn_gen && counts_same && s_vstick_frames == 0 && s_va_frames == 0 &&
                prof_frame_count() - s_drawn_frame < 30;
    if (s_drawn_valid && !skip) {
        if (gen != s_drawn_gen) PROF_COUNT("touchui.noskip_gen", 1);
        if (!counts_same) PROF_COUNT("touchui.noskip_counts", 1);
        if (s_vstick_frames || s_va_frames) PROF_COUNT("touchui.noskip_pulse", 1);
        if (prof_frame_count() - s_drawn_frame >= 30) PROF_COUNT("touchui.noskip_net", 1);
    }
    if (skip) {
        PROF_COUNT("touchui.skips", 1);
        return;
    }
    s_drawn_valid = true;
    s_drawn_gen = gen;
    s_drawn_cfggen = cfg_gen();
    s_drawn_frame = prof_frame_count();
    s_drawn_ncounts = ncur;
    memcpy(s_drawn_counts, cur, (size_t)ncur * sizeof(RingCount));
    WD_PHASE("touchui: ui begin (flush, GPU wait)");
    C3D_RenderTarget* target = gx_ui_begin();
    if (!target) return;
    if (!s_ready) {
        if (!gx_c2d_init()) {
            LOG_WARN("touchui", "C2D_Init failed, touch menu disabled");
            s_failed = true;
            gx_ui_end();
            return;
        }
        s_textbuf = C2D_TextBufNew(2048);
        s_ready = true;
    }
    WD_PHASE("touchui: C2D prepare");
    C2D_TextBufClear(s_textbuf);
    C2D_Prepare();
    C2D_SceneBegin(target);
    if (s_tab != TAB_MAP) {
        WD_PHASE("touchui: rows");
        // The panels cover the minimap; clip the list rows to the panel
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        draw_rows();
    } else {
        WD_PHASE("touchui: ring extras");
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
        C2D_SetTintMode(C2D_TintSolid);
        draw_ring_extras();
    }
    WD_PHASE("touchui: tabs");
    draw_tabs();
    if (s_status_frames > 0) {
        frame_rect(60, 200, 200, 30, PANEL_HI, GOLD);
        text(s_status, 160, 207, 0.6f, GOLD, false);
    }
    WD_PHASE("touchui: C2D flush");
    C2D_Flush();
    WD_PHASE("touchui: ui end (state restore)");
    gx_ui_end();
}
