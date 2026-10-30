// Remote control for testing: the game polls sdmc:/3ds/star3d/cmd.txt (twice a
// second), runs its commands and deletes it, so a script on the PC can drive a
// running emulator. One command per line:
//   load [N]        load save state slot N (1 = newest, default 1)
//   save            save a new state (sdmc:/3ds/star3d/savestates)
//   clean           delete the states older than the newest three
//   tap X Y         tap the touch menu at bottom screen pixel X Y
//   flick DX DY     flick on the Map tab's item pad (pixels, y down), e.g. "flick 40 0"
//   gyro YAW PITCH [N] turn the console at YAW / PITCH degrees per second for N
//                   frames (default 30), right and up positive (gyro aiming)
//   press KEYS [N]  hold buttons for N frames (default 6). KEYS: A B X Y S(tart)
//                   C(select) L R Z U D < > (d-pad), e.g. "press A" or "press ZA 10"
//                   ("press SC" quits the game, same as hardware START+SELECT)
//   wait N          do nothing for N frames
//   set NAME VALUE  change a live setting (cfg_set), e.g. "set S3D_GPU_BLUR 0"
//   warpmap N       warpToMap(N) with a WARPTAB.bin index (hex ok, e.g. 0x35)
//   import list     log the importable save slots in sdmc:/3ds/star3d/saves
//   import N [SLOT] import entry N into card slot SLOT (1..3, default first free)
//   stats           log the "diag" stats line now
//   mark            log a "mark" snapshot (same as L+R+SELECT)
//   frametrace      start a frame trace (needs S3D_FRAME_TRACE, frametrace_3ds.c)
//   tod [SECONDS]   log the time of day, or set it (0-86400, 43200 = noon), e.g. "tod 0"
// Lines starting with # are ignored. Every command is logged.

#include <dolphin/pad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "diag_3ds.h"
#include "frametrace_3ds.h"
#include "log.h"
#include "remote_3ds.h"
#include "savestate_3ds.h"

void warpToMap(int idx, s8 transType);
void touchui_remote_import(const char* args);
void touchui_remote_tap(int x, int y);
void touchui_remote_flick(int dx, int dy);
void port_remote_gyro(float yaw_dps, float pitch_dps, int frames);  // controls_3ds.c
extern void* gSkyState;  // SkyState (include/main/sky_state.h), timeOfDay at 0x20C
void* Obj_GetPlayerObject(void);

#define CMD_FILE "sdmc:/3ds/star3d/cmd.txt"
#define POLL_FRAMES 240  // SD polls: first at frame 30, then every 240
#define MAX_CMDS 64

u16 remote_pad_buttons;
s8 remote_stick[2], remote_cstick[2];

static char s_cmds[MAX_CMDS][64];
static int s_count, s_next;
static int s_wait, s_poll;
static bool s_poll_force;  // remote_poll_now(): poll this frame

static u16 parse_keys(const char* s) {
    static const struct { char c; u16 bit; } keys[] = {
        { 'A', PAD_BUTTON_A }, { 'B', PAD_BUTTON_B }, { 'X', PAD_BUTTON_X }, { 'Y', PAD_BUTTON_Y },
        { 'S', PAD_BUTTON_START }, { 'L', PAD_TRIGGER_L }, { 'R', PAD_TRIGGER_R }, { 'Z', PAD_TRIGGER_Z },
        { 'U', PAD_BUTTON_UP }, { 'D', PAD_BUTTON_DOWN }, { '<', PAD_BUTTON_LEFT }, { '>', PAD_BUTTON_RIGHT },
        { 'C', PAD_BUTTON_SELECT },
    };
    u16 bits = 0;
    for (; *s && *s != ' '; s++)
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (keys[i].c == *s) bits |= keys[i].bit;
    return bits;
}

static void read_file(void) {
    FILE* f = fopen(CMD_FILE, "r");
    if (!f) return;
    char line[64];
    s_count = s_next = 0;
    bool swallow = false;  // Skipping the rest of a long comment line
    while (s_count < MAX_CMDS && fgets(line, sizeof(line), f)) {
        bool endline = strchr(line, '\n') != NULL;
        if (swallow) {
            if (endline) swallow = false;
            continue;
        }
        if (line[0] == '#') {
            // fgets() returns 63-char chunks, so an over-long comment's tail
            // would otherwise become a bogus command: swallow it
            if (!endline) swallow = true;
            continue;
        }
        if (!endline) {
            // Over-long command: keep the first chunk, swallow the rest
            snprintf(s_cmds[s_count++], sizeof(s_cmds[0]), "%s", line);
            swallow = true;
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (line[0]) snprintf(s_cmds[s_count++], sizeof(s_cmds[0]), "%s", line);
    }
    fclose(f);
    remove(CMD_FILE);
}

// findclr ADDR: objects whose active model's GX_VA_CLR0 array is ADDR (or all
// models with vertex colours if ADDR is 0). Raw offsets: GameObject classId
// 0x44, romDefNo 0x46, alpha 0x36/0x37, localPos 0x0C; ModelFileHeader
// modelId 0x04, flags24 0x24, colors 0x30, shaderFlags 0xE2, colorCount 0xE8
// (bug 36: C12 door disc drawn with vertex alpha 0)
extern void** ObjList_GetObjects(int* startIndex, int* objectCount);
extern void* Obj_GetActiveModel(void* obj);
static void find_colors(u32 addr) {
    int start = 0, count = 0;
    void** objs = ObjList_GetObjects(&start, &count);
    int found = 0;
    for (int i = 0; objs && i < count; i++) {
        u8* o = objs[i];
        if (!o) continue;
        u8** model = Obj_GetActiveModel(o);
        u8* m = model ? model[0] : NULL;
        if (!m) continue;
        u8* colors = *(u8**)(m + 0x30);
        u16 n = *(u16*)(m + 0xE8);
        if (addr ? (u32)(uintptr_t)colors != addr : !colors) continue;
        float* pos = (float*)(o + 0x0C);
        // ObjDef (modelInstance @0x50): dllId 0x50, name 0x91
        u8* def = *(u8**)(o + 0x50);
        if (def)
            LOG_WARN("remote", "obj %d def name '%.11s' dll %d (0x%X)", i, (const char*)(def + 0x91), *(s16*)(def + 0x50),
                     (u16) * (s16*)(def + 0x50));
        LOG_WARN("remote", "obj %d %p class %d def 0x%X alpha %u/%u pos %.0f %.0f %.0f model %p id %u flags24 %02X shader %04X colors %p x%u",
                 i, o, *(s16*)(o + 0x44), (u16) * (s16*)(o + 0x46), o[0x36], o[0x37], pos[0], pos[1], pos[2], m,
                 *(u16*)(m + 0x04), m[0x24], *(u16*)(m + 0xE2), colors, n);
        found++;
    }
    LOG_WARN("remote", "findclr: %d of %d objects", found, count);
}

static void run(const char* cmd) {
    LOG_WARN("remote", "%s", cmd);
    if (!strncmp(cmd, "load", 4)) {
        // load [slot 1-3]
        savestate_snap_request_load(cmd[4] == ' ' ? atoi(cmd + 5) - 1 : 0);
    } else if (!strcmp(cmd, "save")) {
        savestate_snap_request_save();
    } else if (!strcmp(cmd, "clean")) {
        LOG_WARN("remote", "clean: %d", savestate_snap_clean());
    } else if (!strncmp(cmd, "tap ", 4)) {
        int x, y;
        if (sscanf(cmd + 4, "%d %d", &x, &y) == 2) touchui_remote_tap(x, y);
        else LOG_WARN("remote", "usage: tap X Y");
    } else if (!strncmp(cmd, "flick ", 6)) {
        int dx = 0, dy = 0;
        sscanf(cmd + 6, "%d %d", &dx, &dy);
        touchui_remote_flick(dx, dy);
    } else if (!strncmp(cmd, "gyro ", 5)) {
        float yaw = 0, pitch = 0;
        int n = 30;
        sscanf(cmd + 5, "%f %f %d", &yaw, &pitch, &n);
        port_remote_gyro(yaw, pitch, n);
    } else if (!strncmp(cmd, "press ", 6)) {
        const char* n = strchr(cmd + 6, ' ');
        remote_pad_buttons = parse_keys(cmd + 6);
        s_wait = n ? atoi(n + 1) : 6;
    } else if (!strncmp(cmd, "stick ", 6) || !strncmp(cmd, "cstick ", 7)) {
        s8* st = cmd[0] == 'c' ? remote_cstick : remote_stick;
        int x = 0, y = 0, n = 6;
        sscanf(strchr(cmd, ' ') + 1, "%d %d %d", &x, &y, &n);
        st[0] = (s8)x;
        st[1] = (s8)y;
        s_wait = n;
    } else if (!strncmp(cmd, "set ", 4)) {
        char name[48];
        int value;
        if (sscanf(cmd + 4, "%47s %d", name, &value) == 2) cfg_set(name, value);
        else LOG_WARN("remote", "usage: set NAME VALUE");
    } else if (!strncmp(cmd, "warp ", 5)) {
        float x, y, z;
        if (sscanf(cmd + 5, "%f %f %f", &x, &y, &z) == 3) LOG_WARN("remote", "warp: %d", savestate_warp(x, y, z));
        else LOG_WARN("remote", "usage: warp X Y Z");
    } else if (!strncmp(cmd, "warpmap ", 8)) {
        warpToMap((int)strtol(cmd + 8, NULL, 0), 0);
    } else if (!strncmp(cmd, "import ", 7)) {
        touchui_remote_import(cmd + 7);
    } else if (!strncmp(cmd, "findclr", 7)) {
        find_colors(cmd[7] == ' ' ? (u32)strtoul(cmd + 8, NULL, 16) : 0);
    } else if (!strcmp(cmd, "stats")) {
        diag_stats();
    } else if (!strcmp(cmd, "mark")) {
        diag_mark();
    } else if (!strcmp(cmd, "frametrace")) {
        ft_request();
    } else if (!strncmp(cmd, "tod", 3)) {
        // The sky DLL's setTimeOfDay slot is a nop: write SkyState.timeOfDay
        float* t = gSkyState ? (float*)((char*)gSkyState + 0x20C) : NULL;
        if (!t) {
            LOG_WARN("remote", "tod: no sky");
        } else {
            if (cmd[3] == ' ') *t = (float)atof(cmd + 4);
            LOG_WARN("remote", "tod: %.0f s (%02d:%02d)", *t, (int)(*t / 3600) % 24, (int)(*t / 60) % 60);
        }
    } else if (!strncmp(cmd, "wait ", 5)) {
        s_wait = atoi(cmd + 5);
    } else {
        LOG_WARN("remote", "unknown command");
    }
}

// S3D_TOD=<seconds> in env.txt (0 = midnight, 43200 = noon): set once the
// player is in the world, again after the sky is recreated. For testing light
// at a given time on hardware without the remote (bug 27: night only)
static void apply_env_tod(void) {
    static void* applied;
    static int tod = -1;
    CFG_LIVE(tod, cfg_int("S3D_TOD", -1));
    if (tod < 0 || !gSkyState || gSkyState == applied || !Obj_GetPlayerObject()) return;
    applied = gSkyState;
    *(float*)((char*)gSkyState + 0x20C) = (float)tod;
    LOG_INFO("remote", "S3D_TOD: time of day set to %02d:%02d", tod / 3600 % 24, tod / 60 % 60);
}

void remote_poll_now(void) { s_poll_force = true; }

void remote_frame(void) {
    if (s_poll == 0) apply_env_tod();
    if (s_wait > 0 && --s_wait > 0) return;
    remote_pad_buttons = 0;
    remote_stick[0] = remote_stick[1] = remote_cstick[0] = remote_cstick[1] = 0;
    while (s_next < s_count) {
        run(s_cmds[s_next++]);
        if (s_wait > 0) return;
    }
    // The SD poll is rare: an fopen here stalls the frame (asset streaming
    // shares the card, and the stall shows as a bottom-screen flicker in the
    // hub). First poll at frame 30 (the boot drive script), then every 240
    // frames; the net server triggers an instant poll after a PUT /cmd.txt.
    bool poll = s_poll_force;
    s_poll_force = false;
    if (++s_poll == 30) poll = true;
    if (s_poll >= POLL_FRAMES) {
        poll = true;
        s_poll = 31;  // Skip the == 30 trip, poll again in POLL_FRAMES frames
    }
    if (poll) read_file();
}
