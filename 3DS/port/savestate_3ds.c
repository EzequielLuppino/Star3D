// One save state slot, built on the game's own savegame: the live savegame
// buffer (gamebits, map acts, object groups = world and enemy state, player
// stats) plus the player position, written to an editable text file. Loading
// copies it back and goes through loadMapForCurrentSaveGame(), the same path
// as "continue from last save", so all assets are reloaded and the player
// spawns at the stored position.
//
// It is the fallback block of every snapshot save state (savestate_snap.c),
// used when the snapshot cannot be restored (different build). Text format:
// "key=value" lines, '#' comments. The named keys (x, y, z, angle, map_layer, character, health, ...)
// are applied on top of the raw "data" hex block, so editing x/y/z/map_layer
// teleports the player on the next load, and editing the data block (or
// deleting it) gives full control over the world state.
//
// The game structures are accessed by offset (see foxhollow/game/src/dlls/
// engine/23/23.c SaveGameData and include/main/objanim_internal.h) so the
// port side does not need the game headers.

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "savestate_3ds.h"

// Game side (static DLLs)
extern unsigned char gSaveGameData[];
extern unsigned char gGameLoopMapLoaded, gGameLoopReloadRequested;
void* Obj_GetPlayerObject(void);
int getCurMapLayer(void);
void loadMapForCurrentSaveGame(void);
extern unsigned char* gSaveGameWorkBuffer;
int getCurUiDll(void);
void loadUiDll(int index);
void setCurUiDll(int idx);
void n_attractmode_releaseMovieBuffers(void);
void titleScreenDisableActors(void);
int mmSetFreeDelay(int v);
int mapUnload(int mapId, int flags);
void* ObjList_FindObjectById(unsigned int objectId);
void Obj_BuildTransformMatrices(void* obj);
void Obj_TransformLocalPointToWorld(float x, float y, float z, float* ox, float* oy, float* oz, void* obj);
void Obj_SetParent(void* obj, void* parent, int updateLocalTransform);
extern unsigned char* gSbGalleon;
void ObjHits_DisableObject(void* obj);
void Sfx_StopObjectChannel(void* obj, int channel);
extern void*** gMapEventInterface;
extern void* gSkyState;  // SkyState (include/main/sky_state.h), timeOfDay at 0x20C

#define UI_DLL_TITLE 4  // n_attractmode: title menu and attract movie
#define MAP_TITLE 0x3D

#define SAVE_ACTIVE_SIZE 0x6EC          // SAVEGAME_ACTIVE_SIZE
#define SAVE_CUR_CHAR 0x20
#define SAVE_STATUS(c) (0x00 + (c) * 12) // PlayerStatus characterStatus[2]
#define SAVE_CHARPOS(c) (0x684 + (c) * 16)

// PlayerStatus
#define ST_HEALTH 0
#define ST_MAX_HEALTH 1
#define ST_MAGIC 4
#define ST_MAX_MAGIC 6
#define ST_MONEY 8

// GameObject / PlayerState
#define OBJ_ROT_X 0x00
#define OBJ_SCALE 0x08
#define OBJ_LOCAL_POS 0x0C
#define OBJ_WORLD_POS 0x18
#define OBJ_VELOCITY 0x24
#define OBJ_PARENT 0x30
#define OBJ_PLACEMENT 0x4C   // placement def, objectId at +0x14
#define OBJ_PREV_LOCAL_POS 0x80
#define OBJ_PREV_WORLD_POS 0x8C
#define OBJ_EXTRA 0xB8
#define PS_STATUS 0x35C
#define MOONJUMP_SPEED 1.5f  // units per tick, a bit above a normal jump

static unsigned char s_pending[SAVE_ACTIVE_SIZE];
static bool s_load_pending;

// Moving platform the player stood on (e.g. the shipbattle galleon). The
// player's local position is relative to it, and the platform itself moves,
// so a plain world position lands somewhere off (or inside) it after a reload.
// The load puts the player back on the platform once it has spawned.
static unsigned int s_plat_id;
static float s_plat_rel[3];
static int s_plat_wait;  // frames left to find the platform after a load
static bool s_plat_armed;  // the old map has gone away since the load started
#define PLATFORM_LIFT 4.0f  // spawn slightly above the deck and drop onto it

// Shipbattle galleon (SB_Galleon, SBGalleonState in its extra data). Its battle
// state is not part of the savegame: on a reload it always restarts the flight
// battle around the CloudRunner, while a player saved on its deck spawns where
// the deck was, so she ends up inside or under the moving ship. The load puts
// the galleon back into the deck fight the way its own landing does.
#define GAL_PHASE 0x29
#define GAL_STAGE 0x2B
#define GAL_POS 0x2C
#define GAL_SWAY 0x38
#define GAL_CAM 0x70
#define GAL_MUSIC_LATCH 0x80
#define GAL_CAM_SHIELD 2   // deck fight
#define GAL_CAM_DONE 4
#define GAL_PHASE_LANDED 6
#define GAL_STAGE_LANDED 7
#define GAL_DECK_GROUP 2   // hosted map object group the landing enables
#define GAL_UNKNOWN -2     // state file from before galleon_state existed
#define MAPEVENT_GET_GROUP 0x4C  // MapEventInterface getObjGroupStatus / setObjGroupStatus
#define MAPEVENT_SET_GROUP 0x50
static int s_gal_cam = -1;     // cameraState to restore, -1 = leave alone
static int s_gal_wait;         // frames left to find the galleon after a load
static float s_gal_player[3];  // player position to put back on the deck

static unsigned char* player_status(void) {
    unsigned char* obj = Obj_GetPlayerObject();
    if (!obj) return NULL;
    unsigned char* ps = *(unsigned char**)(obj + OBJ_EXTRA);
    if (!ps) return NULL;
    return *(unsigned char**)(ps + PS_STATUS);
}

static bool in_game(void) {
    return gGameLoopMapLoaded && !gGameLoopReloadRequested && Obj_GetPlayerObject();
}
bool savestate_in_game(void) { return in_game(); }

// Title menu with no load in flight: the load starts the game from here.
static bool on_title(void) {
    return !gGameLoopReloadRequested && getCurUiDll() == UI_DLL_TITLE && !Obj_GetPlayerObject();
}

static int16_t rd16(const unsigned char* p) { int16_t v; memcpy(&v, p, 2); return v; }
static void wr16(unsigned char* p, int16_t v) { memcpy(p, &v, 2); }
static float rdf(const unsigned char* p) { float v; memcpy(&v, p, 4); return v; }
static void wrf(unsigned char* p, float v) { memcpy(p, &v, 4); }

// Player position and map layer, logged under tag; false outside the game
bool savestate_log_where(const char* tag) {
    if (!in_game()) return false;
    unsigned char* obj = Obj_GetPlayerObject();
    // Time of day (SkyState.timeOfDay, seconds): lighting depends on it (bug 27)
    float tod = gSkyState ? rdf((unsigned char*)gSkyState + 0x20C) : -1.0f;
    LOG_INFO(tag, "player %.0f %.0f %.0f angle %d layer %d time %02d:%02d", rdf(obj + OBJ_WORLD_POS),
             rdf(obj + OBJ_WORLD_POS + 4), rdf(obj + OBJ_WORLD_POS + 8), (rd16(obj + OBJ_ROT_X) >> 8) & 0xff,
             getCurMapLayer(), (int)(tod / 3600) % 24, (int)(tod / 60) % 60);
    return true;
}

// Player position next to every fps report, to tie perf and glitches to a place
void prof_report_extra(void) { savestate_log_where("where"); }

// Appends to the format buffer
typedef struct { char* p; int len, cap; } Out;
static void outf(Out* o, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(Out* o, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int room = o->len < o->cap ? o->cap - o->len : 0;
    int n = vsnprintf(o->p + o->len, room, fmt, ap);
    va_end(ap);
    if (n > 0) o->len += n;
}

int savestate_format(char* out, int cap) {
    if (!in_game()) return 0;
    unsigned char buf[SAVE_ACTIVE_SIZE];
    memcpy(buf, gSaveGameData, SAVE_ACTIVE_SIZE);
    int cur = buf[SAVE_CUR_CHAR] & 1;
    unsigned char* obj = Obj_GetPlayerObject();
    unsigned char* pos = buf + SAVE_CHARPOS(cur);
    // Same values the game's own save points store (savePoint(&localPosX, rotX, ..)),
    // in world space when the player is attached to a platform
    unsigned char* parent = *(unsigned char**)(obj + OBJ_PARENT);
    const unsigned char* pdef = parent ? *(unsigned char**)(parent + OBJ_PLACEMENT) : NULL;
    unsigned int plat = 0;
    float rel[3] = { rdf(obj + OBJ_LOCAL_POS), rdf(obj + OBJ_LOCAL_POS + 4), rdf(obj + OBJ_LOCAL_POS + 8) };
    float x = rel[0], y = rel[1], z = rel[2];
    int16_t rot = rd16(obj + OBJ_ROT_X);
    if (parent) {
        x = rdf(obj + OBJ_WORLD_POS); y = rdf(obj + OBJ_WORLD_POS + 4); z = rdf(obj + OBJ_WORLD_POS + 8);
        rot = (int16_t)(rot + rd16(parent + OBJ_ROT_X));
        if (pdef) memcpy(&plat, pdef + 0x14, 4);
    }
    int angle = (rot >> 8) & 0xff;
    int layer = getCurMapLayer();
    wrf(pos, x); wrf(pos + 4, y); wrf(pos + 8, z);
    pos[12] = (unsigned char)angle;
    pos[13] = (unsigned char)layer;
    const unsigned char* st = buf + SAVE_STATUS(cur);

    Out o = { out, 0, cap };
    Out* f = &o;
    outf(f, "# Star 3D save state, reload block (used when the snapshot does not fit this build)\n"
               "# The named values are applied on top of the data block when loading:\n"
               "# change x/y/z/map_layer to teleport, health etc. to edit the player.\n"
               "# character: savegame character slot (0/1). angle: 0-255. map_layer: -1..2.\n");
    outf(f, "character=%d\nx=%.3f\ny=%.3f\nz=%.3f\nangle=%d\nmap_layer=%d\n", cur, x, y, z, angle, layer);
    if (plat)
        outf(f, "# Standing on a moving object: placed back on it at this offset\n"
                   "platform=%x\nplatform_x=%.3f\nplatform_y=%.3f\nplatform_z=%.3f\n", plat, rel[0], rel[1], rel[2]);
    unsigned char* gal = gSbGalleon ? *(unsigned char**)(gSbGalleon + OBJ_EXTRA) : NULL;
    outf(f, "# Shipbattle galleon: -1 none, 0 flight battle, 1-2 deck fight, 3-4 over\n"
               "galleon_state=%d\n", gal ? (signed char)gal[GAL_CAM] : -1);
    outf(f, "health=%d\nmax_health=%d\nmagic=%d\nmax_magic=%d\nmoney=%d\n", (signed char)st[ST_HEALTH],
            (signed char)st[ST_MAX_HEALTH], rd16(st + ST_MAGIC), rd16(st + ST_MAX_MAGIC), st[ST_MONEY]);
    outf(f, "# Raw savegame (0x%X bytes: stats, gamebits, map acts, object groups)\n", SAVE_ACTIVE_SIZE);
    for (int i = 0; i < SAVE_ACTIVE_SIZE; i += 32) {
        outf(f, "data=");
        for (int j = i; j < i + 32 && j < SAVE_ACTIVE_SIZE; j++) outf(f, "%02x", buf[j]);
        outf(f, "\n");
    }
    if (o.len >= cap) return 0;
    LOG_WARN("savestate", "saved: char %d pos %.1f %.1f %.1f layer %d parent %p id %x", cur, x, y, z, layer,
             (void*)parent, plat);
    return o.len;
}

bool savestate_apply_text(const char* text) {
    if (!text) return false;
    unsigned char buf[SAVE_ACTIVE_SIZE];
    memcpy(buf, gSaveGameData, SAVE_ACTIVE_SIZE);
    int data_pos = 0;
    // Named values are collected first and applied after the data block
    int cur = -1, angle = -1, layer = 1000, hp = -1000, maxhp = -1000, mp = -1, maxmp = -1, money = -1;
    float x = NAN, y = NAN, z = NAN;
    unsigned int plat = 0;
    float rel[3] = { 0, 0, 0 };
    int gal = GAL_UNKNOWN;
    char line[256];
    for (const char* t = text; *t;) {
        size_t n = strcspn(t, "\r\n");
        size_t c = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, t, c);
        line[c] = 0;
        t += n;
        while (*t == '\r' || *t == '\n') t++;
        char* eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        if (!strcmp(k, "data")) {
            for (; v[0] && v[1] && data_pos < SAVE_ACTIVE_SIZE; v += 2) {
                char hex[3] = { v[0], v[1], 0 };
                buf[data_pos++] = (unsigned char)strtoul(hex, NULL, 16);
            }
        }
        else if (!strcmp(k, "character")) cur = atoi(v) & 1;
        else if (!strcmp(k, "x")) x = strtof(v, NULL);
        else if (!strcmp(k, "y")) y = strtof(v, NULL);
        else if (!strcmp(k, "z")) z = strtof(v, NULL);
        else if (!strcmp(k, "angle")) angle = atoi(v) & 0xff;
        else if (!strcmp(k, "map_layer")) layer = atoi(v);
        else if (!strcmp(k, "health")) hp = atoi(v);
        else if (!strcmp(k, "max_health")) maxhp = atoi(v);
        else if (!strcmp(k, "magic")) mp = atoi(v);
        else if (!strcmp(k, "max_magic")) maxmp = atoi(v);
        else if (!strcmp(k, "money")) money = atoi(v);
        else if (!strcmp(k, "platform")) plat = (unsigned int)strtoul(v, NULL, 16);
        else if (!strcmp(k, "platform_x")) rel[0] = strtof(v, NULL);
        else if (!strcmp(k, "platform_y")) rel[1] = strtof(v, NULL);
        else if (!strcmp(k, "platform_z")) rel[2] = strtof(v, NULL);
        else if (!strcmp(k, "galleon_state")) gal = atoi(v);
    }
    if (cur >= 0) buf[SAVE_CUR_CHAR] = (unsigned char)cur;
    cur = buf[SAVE_CUR_CHAR] & 1;
    unsigned char* pos = buf + SAVE_CHARPOS(cur);
    if (!isnan(x)) wrf(pos, x);
    if (!isnan(y)) wrf(pos + 4, y);
    if (!isnan(z)) wrf(pos + 8, z);
    if (angle >= 0) pos[12] = (unsigned char)angle;
    if (layer != 1000) pos[13] = (unsigned char)layer;
    unsigned char* st = buf + SAVE_STATUS(cur);
    if (maxhp != -1000) st[ST_MAX_HEALTH] = (unsigned char)maxhp;
    if (hp != -1000) st[ST_HEALTH] = (unsigned char)hp;
    if ((signed char)st[ST_HEALTH] < 1) st[ST_HEALTH] = 1;
    if (maxmp >= 0) wr16(st + ST_MAX_MAGIC, maxmp);
    if (mp >= 0) wr16(st + ST_MAGIC, mp);
    if (money >= 0) st[ST_MONEY] = (unsigned char)money;
    memcpy(s_pending, buf, SAVE_ACTIVE_SIZE);
    s_plat_id = plat;
    memcpy(s_plat_rel, rel, sizeof(rel));
    s_plat_wait = 0;
    // Older state files: a player on foot on the shipbattle map was on the deck
    s_gal_cam = gal == GAL_UNKNOWN ? (plat ? -1 : GAL_CAM_SHIELD) : gal;
    s_gal_wait = 0;
    memcpy(s_gal_player, pos, sizeof(s_gal_player));
    s_load_pending = true;
    return true;
}

// Called once per frame from VIWaitForRetrace: the load waits until no map
// load is in flight, then hands over to the game's own reload path. From the
// title menu it first leaves the front end the way n_attractmode does when
// starting a game (movie buffers, in-game UI DLL, title actors, title map).
// Put the player back on the platform once the reloaded map has spawned it
static void place_on_platform(void) {
    if (--s_plat_wait < 0) return;
    unsigned char* plat = ObjList_FindObjectById(s_plat_id);
    if (!plat) {
        if (s_plat_wait == 0) LOG_WARN("savestate", "platform %x not found", s_plat_id);
        return;
    }
    s_plat_wait = 0;
    unsigned char* obj = Obj_GetPlayerObject();
    float w[3];
    Obj_SetParent(obj, NULL, 1);
    Obj_BuildTransformMatrices(plat);
    Obj_TransformLocalPointToWorld(s_plat_rel[0], s_plat_rel[1] + PLATFORM_LIFT, s_plat_rel[2], &w[0], &w[1], &w[2], plat);
    for (int i = 0; i < 3; i++) {
        wrf(obj + OBJ_LOCAL_POS + i * 4, w[i]);
        wrf(obj + OBJ_WORLD_POS + i * 4, w[i]);
        wrf(obj + OBJ_PREV_LOCAL_POS + i * 4, w[i]);
        wrf(obj + OBJ_PREV_WORLD_POS + i * 4, w[i]);
        wrf(obj + OBJ_VELOCITY + i * 4, 0);
    }
    Obj_SetParent(obj, plat, 1);
    LOG_WARN("savestate", "placed on platform %x at %.1f %.1f %.1f", s_plat_id, w[0], w[1], w[2]);
}

static void set_obj_pos(unsigned char* obj, const float* p) {
    for (int i = 0; i < 3; i++) {
        wrf(obj + OBJ_LOCAL_POS + i * 4, p[i]);
        wrf(obj + OBJ_WORLD_POS + i * 4, p[i]);
        wrf(obj + OBJ_PREV_LOCAL_POS + i * 4, p[i]);
        wrf(obj + OBJ_PREV_WORLD_POS + i * 4, p[i]);
        wrf(obj + OBJ_VELOCITY + i * 4, 0);
    }
}

// Debug teleport (remote "warp X Y Z"); detaches from any platform first
bool savestate_warp(float x, float y, float z) {
    if (!in_game()) return false;
    unsigned char* obj = Obj_GetPlayerObject();
    if (*(void**)(obj + OBJ_PARENT)) Obj_SetParent(obj, NULL, 1);
    float p[3] = { x, y, z };
    set_obj_pos(obj, p);
    LOG_WARN("savestate", "warp to %.1f %.1f %.1f", x, y, z);
    return true;
}

// Put the reloaded galleon into the saved part of the battle: for the deck
// fight and later this is what SB_Galleon_updateFlight does when it lands
// (the landing sequence itself is skipped), then the player goes back on deck
static void restore_galleon(void) {
    if (--s_gal_wait < 0) return;
    unsigned char* g = gSbGalleon;
    unsigned char* st = g ? *(unsigned char**)(g + OBJ_EXTRA) : NULL;
    if (!st) {
        if (s_gal_wait == 0 && s_gal_cam > 0) LOG_WARN("savestate", "galleon not found");
        return;
    }
    s_gal_wait = 0;
    if (s_gal_cam <= 0) return;  // flight battle: restarts from the galleon's init
    const unsigned char* pdef = *(unsigned char**)(g + OBJ_PLACEMENT);
    float p[3] = { rdf(pdef + 0x08), -1.0f, rdf(pdef + 0x10) };
    wr16(g + OBJ_ROT_X, 0);
    wr16(g + OBJ_ROT_X + 2, 0);
    wr16(g + OBJ_ROT_X + 4, 0);
    set_obj_pos(g, p);
    for (int i = 0; i < 3; i++) {
        wrf(st + GAL_POS + i * 4, p[i]);
        wrf(st + GAL_SWAY + i * 4, 0);
    }
    st[GAL_PHASE] = GAL_PHASE_LANDED;
    st[GAL_STAGE] = GAL_STAGE_LANDED;
    st[GAL_CAM] = (unsigned char)(s_gal_cam <= GAL_CAM_SHIELD ? GAL_CAM_SHIELD : GAL_CAM_DONE);
    st[GAL_MUSIC_LATCH] = 1;
    ObjHits_DisableObject(g);
    Sfx_StopObjectChannel(g, 1);
    int slot = g[0x34];  // hostedMapSlot
    void** mapev = *gMapEventInterface;
    if (!((int (*)(int, int))mapev[MAPEVENT_GET_GROUP / 4])(slot, GAL_DECK_GROUP))
        ((void (*)(int, int, int))mapev[MAPEVENT_SET_GROUP / 4])(slot, GAL_DECK_GROUP, 1);
    unsigned char* obj = Obj_GetPlayerObject();
    float pp[3] = { s_gal_player[0], s_gal_player[1] + PLATFORM_LIFT, s_gal_player[2] };
    if (!*(void**)(obj + OBJ_PARENT)) set_obj_pos(obj, pp);
    LOG_WARN("savestate", "galleon restored to state %d at %.1f %.1f %.1f", st[GAL_CAM], p[0], p[1], p[2]);
}

void savestate_cancel_pending(void) {
    s_load_pending = false;
    s_plat_wait = s_gal_wait = 0;
}

void savestate_frame(void) {
    if (s_plat_wait > 0 || s_gal_wait > 0) {
        // Wait for the old map to go away, then for the reloaded objects
        if (!in_game()) s_plat_armed = true;
        else if (s_plat_armed) {
            if (s_gal_wait > 0) restore_galleon();
            if (s_plat_wait > 0) place_on_platform();
        }
    }
    if (!s_load_pending) return;
    bool title = !in_game() && on_title();
    if (!title && !in_game()) return;
    s_load_pending = false;
    if (title) {
        n_attractmode_releaseMovieBuffers();
        loadUiDll(1);
        // The switch is only queued; mark in-game now so the player spawn picks
        // the default camera (title camera mode leaves no default handler -> crash)
        setCurUiDll(1);
        titleScreenDisableActors();
        int delay = mmSetFreeDelay(0);
        mapUnload(MAP_TITLE, 0x20000000);
        mmSetFreeDelay(delay);
        // Also the "last save" a game over returns to
        if (gSaveGameWorkBuffer) memcpy(gSaveGameWorkBuffer, s_pending, SAVE_ACTIVE_SIZE);
    }
    memcpy(gSaveGameData, s_pending, SAVE_ACTIVE_SIZE);
    const unsigned char* pos = gSaveGameData + SAVE_CHARPOS(gSaveGameData[SAVE_CUR_CHAR] & 1);
    LOG_WARN("savestate", "loading%s: char %d pos %.1f %.1f %.1f layer %d", title ? " from title" : "", gSaveGameData[SAVE_CUR_CHAR] & 1,
             rdf(pos), rdf(pos + 4), rdf(pos + 8), (signed char)pos[13]);
    s_plat_wait = s_plat_id ? 120 : 0;
    s_gal_wait = s_gal_cam > 0 ? 120 : 0;
    s_plat_armed = false;
    loadMapForCurrentSaveGame();
}

// --- Cheats ------------------------------------------------------------------

static void* s_scaled_obj;
static float s_base_scale;

void savestate_cheats(int health, int magic, int money, int scale) {
    unsigned char* obj = Obj_GetPlayerObject();
    unsigned char* st = player_status();
    if (st) {
        if (health && (signed char)st[ST_HEALTH] < (signed char)st[ST_MAX_HEALTH]) st[ST_HEALTH] = st[ST_MAX_HEALTH];
        if (magic && rd16(st + ST_MAGIC) < rd16(st + ST_MAX_MAGIC)) wr16(st + ST_MAGIC, rd16(st + ST_MAX_MAGIC));
        if (money) st[ST_MONEY] = 255;
    }
    // Player model scale; the original value is remembered per player object
    // (it changes on map load / character switch) and restored when turned off.
    if (obj != s_scaled_obj) {
        s_scaled_obj = obj;
        s_base_scale = obj ? rdf(obj + OBJ_SCALE) : 0;
    }
    if (obj && s_base_scale > 0) {
        float want = scale > 0 ? s_base_scale * 2.0f : scale < 0 ? s_base_scale * 0.5f : s_base_scale;
        if (rdf(obj + OBJ_SCALE) != want) wrf(obj + OBJ_SCALE, want);
    }
}

// Moon jump: while held, the player keeps rising
void savestate_moonjump(bool held) {
    if (!held || !in_game()) return;
    unsigned char* obj = Obj_GetPlayerObject();
    if (rdf(obj + OBJ_VELOCITY + 4) < MOONJUMP_SPEED) wrf(obj + OBJ_VELOCITY + 4, MOONJUMP_SPEED);
    // Standing on the ground the player code drops the upward speed, so lift her as well
    wrf(obj + OBJ_LOCAL_POS + 4, rdf(obj + OBJ_LOCAL_POS + 4) + MOONJUMP_SPEED);
    static int frames;
    if (++frames % 30 == 0) LOG_INFO("cheat", "moon jump y %.1f", rdf(obj + OBJ_WORLD_POS + 4));
}

bool savestate_cheat_heal(void) {
    unsigned char* st = player_status();
    if (!st) return false;
    st[ST_HEALTH] = st[ST_MAX_HEALTH];
    wr16(st + ST_MAGIC, rd16(st + ST_MAX_MAGIC));
    return true;
}

// --- Win Ice Mountain race cheat ---------------------------------------------
// The ice mountain SnowBike race (SnowBikes DLL 597 vs the two SnowClaw riders,
// DLL 604) is decided by the map controller IMIceMountain (DLL 361) from the
// player's race rank; see icemountain_win(). race_controller() (CRCloudRace,
// DLL 605) is only used by the "raceinfo" log.

#define DLL_CRCLOUDRACE 605
#define DLL_SNOWBIKE 597
#define OBJ_DEF 0x50     // GameObject -> ObjDef (modelInstance)
#define DEF_DLLID 0x50   // ObjDef -> DLL id
#define RACEBIT_COMPLETION_EVENT 0xDCA
#define RACEBIT_RACE_STARTED 0x49D
#define RACEBIT_RACE_ACTIVE 0x497
#define RACEBIT_IN_FINISH_VOLUME 0x499
#define RACEBIT_RACE_CAN_FINISH 0x4A9
#define RACEBIT_ABORT_TRIGGER 0x2E8
#define RACEBIT_IM_RACE_OVER 0x3A3  // GAMEBIT_IM_BikeRelated03A3, the player bike's stop bit
#define DLL_ICEMOUNTAIN 361
#define IM_EVENT_STATE 0x00      // IMIceMountainState.eventState
#define IM_STATE_RACING 4
#define IM_STATE_WON 5           // won, waiting for the end sequence
#define IM_STATE_AFTER_WIN 0     // after state 5: setMapAct 2
#define RACE_DISMOUNT_TIMEOUT 120  // frames to wait for the controller
#define BIKE_ROUTE 0x28          // SnowBikeState.routeState
#define ROUTE_PROGRESS 0x0C      // CheckpointRouteState.routeProgress
#define ROUTE_LINK_DEPTH 0x1C    // CheckpointRouteState.linkDepth
#define BIKE_ROUTE_RANK 0x422    // SnowBikeState.routeRank

extern void mainSetBits(int gameBit, int value);
extern int mainGetBit(int gameBit);
extern void** ObjList_GetObjects(int* startIndex, int* objectCount);
extern void*** gGameUIInterface;         // GameUIInterface (dll_0000_gameui.h)
extern void*** gObjectTriggerInterface;  // ObjectTriggerInterface (objseq.h)
extern void* playerGetFocusObject(void* player);
extern int playerStopRidingObject(void* player);

static unsigned char* player_bike(void);
static unsigned char* icemountain_obj(void);
static unsigned char* s_race_guard_im;  // armed after a cheat win: the IMIceMountain object
static int s_race_wait_frames;           // frames left for the controller to decide

// Run sequence idx on the player's bike (the race-over script; testing)
bool savestate_race_runseq(int idx) {
    if (!in_game()) return false;
    unsigned char* bike = player_bike();
    if (!bike) return false;
    ((int (*)(int, void*, int))(*(void***)gObjectTriggerInterface)[0x48 / 4])(idx, bike, -1);
    LOG_WARN("cheat", "ran sequence %d on bike %p", idx, (void*)bike);
    return true;
}

// Put the player's bike ahead of every other bike's route (linkDepth and
// routeProgress) so the checkpoint ranking puts it first; also run its path
// progress to the end (180). Testing helper for the race win condition.
bool savestate_race_bikefirst(void) {
    if (!in_game()) return false;
    unsigned char* pb = player_bike();
    if (!pb) return false;
    int start = 0, count = 0;
    unsigned char** objs = (unsigned char**)ObjList_GetObjects(&start, &count);
    int maxLink = 0;
    float maxProg = 0;
    for (int i = 0; objs && i < count; i++) {
        unsigned char* o = objs[i];
        if (!o) continue;
        unsigned char* def = *(unsigned char**)(o + OBJ_DEF);
        if (!def || *(int16_t*)(def + DEF_DLLID) != DLL_SNOWBIKE) continue;
        unsigned char* ex = *(unsigned char**)(o + OBJ_EXTRA);
        if (!ex) continue;
        int l = *(int*)(ex + 0x28 + 0x1C);
        float p = rdf(ex + 0x28 + 0x0C);
        if (l > maxLink || (l == maxLink && p > maxProg)) {
            maxLink = l;
            maxProg = p;
        }
    }
    unsigned char* pex = *(unsigned char**)(pb + OBJ_EXTRA);
    *(int*)(pex + 0x28 + 0x1C) = maxLink + 1;
    wrf(pex + 0x28 + 0x0C, maxProg + 1000.0f);
    wrf(pex + 0x68, 180.0f);
    LOG_WARN("cheat", "bikefirst: linkDepth %d routeProgress %.1f (was max %d / %.1f)", maxLink + 1, maxProg + 1000.0f,
             maxLink, maxProg);
    return true;
}

// The race controller object, or NULL when the current map has none
static unsigned char* race_controller(void) {
    int start = 0, count = 0;
    unsigned char** objs = (unsigned char**)ObjList_GetObjects(&start, &count);
    for (int i = 0; objs && i < count; i++) {
        unsigned char* o = objs[i];
        if (!o) continue;
        unsigned char* def = *(unsigned char**)(o + OBJ_DEF);
        if (def && *(int16_t*)(def + DEF_DLLID) == DLL_CRCLOUDRACE) return o;
    }
    return NULL;
}

// The player's ice mountain bike (rom 0x72 = SNOWBIKE_IM_BIKE_OBJ)
static unsigned char* player_bike(void) {
    int start = 0, count = 0;
    unsigned char** objs = (unsigned char**)ObjList_GetObjects(&start, &count);
    for (int i = 0; objs && i < count; i++) {
        unsigned char* o = objs[i];
        if (!o) continue;
        unsigned char* def = *(unsigned char**)(o + OBJ_DEF);
        if (!def || *(int16_t*)(def + DEF_DLLID) != DLL_SNOWBIKE) continue;
        if ((unsigned)*(int16_t*)(o + 0x46) == 0x72) return o;  // romDefNo
    }
    return NULL;
}

// Race state to the log (remote "raceinfo", for testing)
void savestate_race_log(void) {
    unsigned char* race = race_controller();
    LOG_WARN("cheat", "race: controller %p started %d active %d finish %d can_finish %d abort %d done %d",
             (void*)race, mainGetBit(RACEBIT_RACE_STARTED), mainGetBit(RACEBIT_RACE_ACTIVE),
             mainGetBit(RACEBIT_IN_FINISH_VOLUME), mainGetBit(RACEBIT_RACE_CAN_FINISH),
             mainGetBit(RACEBIT_ABORT_TRIGGER), mainGetBit(RACEBIT_COMPLETION_EVENT));
    if (race) {
        unsigned char* ex = *(unsigned char**)(race + OBJ_EXTRA);
        if (ex) LOG_WARN("cheat", "race: phase %d flags %02X", ex[8], ex[9]);
    }
    // SnowBike route state: the bikes' race progress (routeState @0x28,
    // gameBitPtr @0x60 = start/stop gamebit pair, pathProgress @0x68,
    // riderMode @0x421, routeRank @0x422, routeFlags @0x428)
    int start = 0, count = 0;
    unsigned char** objs = (unsigned char**)ObjList_GetObjects(&start, &count);
    for (int i = 0; objs && i < count; i++) {
        unsigned char* o = objs[i];
        if (!o) continue;
        unsigned char* def = *(unsigned char**)(o + OBJ_DEF);
        if (!def || *(int16_t*)(def + DEF_DLLID) != DLL_SNOWBIKE) continue;
        unsigned char* ex = *(unsigned char**)(o + OBJ_EXTRA);
        unsigned char* gbp = ex ? *(unsigned char**)(ex + 0x60) : NULL;
        int b0 = gbp ? *(int16_t*)gbp : -1;
        int b1 = gbp ? *(int16_t*)(gbp + 2) : -1;
        LOG_WARN("cheat", "bike rom 0x%X pos %.0f %.0f %.0f bits %d/%d progress %.1f rank %d rider %d flags %02X",
                 (unsigned)*(int16_t*)(o + 0x46), rdf(o + OBJ_LOCAL_POS), rdf(o + OBJ_LOCAL_POS + 4),
                 rdf(o + OBJ_LOCAL_POS + 8), b0, b1, ex ? rdf(ex + 0x68) : -1.0f, ex ? (signed char)ex[0x422] : -1,
                 ex ? ex[0x421] : -1, ex ? ex[0x428] : 0);
    }
    unsigned char* im = icemountain_obj();
    unsigned char* ims = im ? *(unsigned char**)(im + OBJ_EXTRA) : NULL;
    LOG_WARN("cheat", "icemountain %p state %d act %d bits 3A2 %d 3A3 %d 379 %d", (void*)im, ims ? ims[0] : -1,
             ims ? ims[0x0C] : -1, mainGetBit(0x3A2), mainGetBit(0x3A3), mainGetBit(0x379));
    savestate_log_where("race");
}

// The Ice Mountain controller (IMIceMountain, DLL 361), or NULL
static unsigned char* icemountain_obj(void) {
    int start = 0, count = 0;
    unsigned char** objs = (unsigned char**)ObjList_GetObjects(&start, &count);
    for (int i = 0; objs && i < count; i++) {
        unsigned char* o = objs[i];
        if (!o) continue;
        unsigned char* def = *(unsigned char**)(o + OBJ_DEF);
        if (def && *(int16_t*)(def + DEF_DLLID) == DLL_ICEMOUNTAIN) return o;
    }
    return NULL;
}

// Ice Mountain race win. The race is decided by IMIceMountain
// (foxhollow/game/src/dlls/objects/361_IMIceMounta/IMIceMounta.c): in event
// state 4 (racing) it waits for gamebit 0x3A3 (race over), then reads the
// player's bike getRacePosition() (= SnowBikeState.routeRank): 1 -> state 5
// (won: gamebit 0x379 = 1, then setMapAct 2 after the end sequence), anything
// else -> state 6 (lost: warpToMap(0x1A), the race restarts).
// The rank is recomputed every frame from the live route states of all bikes
// (Checkpoint_getRouteRank compares linkDepth, then routeProgress), so the
// rivals are pushed far behind; the player's stored rank is also set to 1 in
// case its bike updates after the controller this frame.
static bool icemountain_win(void) {
    unsigned char* im = icemountain_obj();
    if (!im) return false;
    unsigned char* ims = *(unsigned char**)(im + OBJ_EXTRA);
    if (!ims || ims[IM_EVENT_STATE] != IM_STATE_RACING) {
        LOG_WARN("cheat", "ice race: not racing (state %d)", ims ? ims[IM_EVENT_STATE] : -1);
        return false;
    }
    unsigned char* bike = (unsigned char*)playerGetFocusObject(Obj_GetPlayerObject());
    if (!bike) bike = player_bike();
    if (!bike) return false;
    int start = 0, count = 0;
    unsigned char** objs = (unsigned char**)ObjList_GetObjects(&start, &count);
    for (int i = 0; objs && i < count; i++) {
        unsigned char* o = objs[i];
        if (!o || o == bike) continue;
        unsigned char* def = *(unsigned char**)(o + OBJ_DEF);
        if (!def || *(int16_t*)(def + DEF_DLLID) != DLL_SNOWBIKE) continue;
        unsigned char* ex = *(unsigned char**)(o + OBJ_EXTRA);
        if (!ex) continue;
        *(int*)(ex + BIKE_ROUTE + ROUTE_LINK_DEPTH) = -100000;
        wrf(ex + BIKE_ROUTE + ROUTE_PROGRESS, -1.0e9f);
        ex[BIKE_ROUTE_RANK] = 2;
    }
    unsigned char* bex = *(unsigned char**)(bike + OBJ_EXTRA);
    if (bex) bex[BIKE_ROUTE_RANK] = 1;
    // The finish-line event of the player's bike (seq event 2) sets this too
    mainSetBits(RACEBIT_IN_FINISH_VOLUME, 1);
    // Race over: the controller decides win/lose on its next update
    mainSetBits(RACEBIT_IM_RACE_OVER, 1);
    // Fox gets off the bike in savestate_race_frame(), once the controller
    // has read the rank: it reads it from the player's focus object, so
    // dismounting now would count as no position and lose the race.
    s_race_guard_im = im;
    s_race_wait_frames = RACE_DISMOUNT_TIMEOUT;
    // Shut the HUD air meter and run the bike's race-over sequence, as
    // SnowBike_UpdateAirMeter in 597.c does. The controller waits for that
    // sequence before it moves on to the after-race act.
    ((void (*)(void))(*(void***)gGameUIInterface)[0x60 / 4])();
    ((int (*)(int, void*, int))(*(void***)gObjectTriggerInterface)[0x48 / 4])(0, bike, -1);
    LOG_WARN("cheat", "ice race won: bike %p rank %d", (void*)bike, bex ? (signed char)bex[BIKE_ROUTE_RANK] : -1);
    return true;
}

// Per frame (touchui_frame): keeps the player off the bike after a cheat win.
// The real finish sequence takes the player off the bike with a player seq
// event (event 2 -> playerStopRidingObject, player.c), which also hands the
// camera back to the player. The race-over sequence the cheat runs has no
// such event and even re-mounts the nearest bike (player seq event 3, to get
// ready for the restart); pressing A next to the bike mounts it as well.
// Riding a bike after the race crashes in playerStateOnBike once the bike
// object is freed (console tests v6 and v8), so while the guard is armed
// (until the map changes) the player is taken off any SnowBike right away.
// The first dismount waits until IMIceMountain has left state 4: it reads the
// race position through the player's focus object, and no bike = lost.
// Never just clear focusObject: the rider camera deadlocks.
void savestate_race_frame(void) {
    if (!s_race_guard_im) return;
    unsigned char* im = in_game() ? icemountain_obj() : NULL;
    if (im != s_race_guard_im) {
        LOG_WARN("cheat", "ice race: guard off (map changed)");
        s_race_guard_im = NULL;
        return;
    }
    unsigned char* ims = *(unsigned char**)(im + OBJ_EXTRA);
    int state = ims ? ims[IM_EVENT_STATE] : -1;
    if (state == IM_STATE_RACING) {
        if (s_race_wait_frames > 0 && --s_race_wait_frames == 0) {
            LOG_WARN("cheat", "ice race: controller never decided, guard off");
            s_race_guard_im = NULL;
        }
        return;
    }
    if (state != IM_STATE_WON && state != IM_STATE_AFTER_WIN) {
        LOG_WARN("cheat", "ice race: controller state %d (not a win), guard off", state);
        s_race_guard_im = NULL;
        return;
    }
    void* player = Obj_GetPlayerObject();
    unsigned char* focus = (unsigned char*)playerGetFocusObject(player);
    if (!focus) return;
    unsigned char* def = *(unsigned char**)(focus + OBJ_DEF);
    if (!def || *(int16_t*)(def + DEF_DLLID) != DLL_SNOWBIKE) return;
    int r = playerStopRidingObject(player);
    LOG_WARN("cheat", "ice race: off the bike %p (%d), controller state %d", (void*)focus, r, state);
}

bool savestate_cheat_winrace(void) {
    if (!in_game()) return false;
    return icemountain_win();
}
