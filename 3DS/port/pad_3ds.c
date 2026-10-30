// PAD (controller) for 3DS: maps the 3DS buttons/sticks onto GameCube PADStatus.
#include "log.h"
#include "remote_3ds.h"
#include "config.h"
#include <dolphin/pad.h>
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// controls_3ds.c, touchui_3ds.c
bool port_cstick_to_game(void);
void port_cstick_read(s8* x, s8* y);
void touchui_pad_state(s8* cx, s8* cy, u16* buttons);

// Static PADStatus structure to hold controller state
static PADStatus g_pad_status;
static bool g_input_initialized = false;

// Forward declarations for internal functions
static void update_analog_sticks(void);
static void update_digital_buttons(void);
static void update_triggers(void);

// --- Input record / playback (env, default off) --------------------------
// S3D_PAD_RECORD=1 records the effective input of every PADRead (buttons,
// sticks, triggers - hardware, remote and touch menu combined) into RAM and
// writes sdmc:/3ds/star3d/padrec.bin at shutdown: play a route once, quit,
// and the file is there. S3D_PAD_PLAY=1 plays that file back, overriding the
// real inputs frame by frame, so the recorded route can be replayed hands
// off (for testing areas, A/B runs, captures).
#define PADREC_PATH "sdmc:/3ds/star3d/padrec.bin"
#define PADREC_MAX_BYTES (12u * 1024 * 1024)
// The game can poll PADRead several times per frame (loading loops poll
// thousands of times a second), so one frame is recorded (and played back)
// per ~15 ms; the played-back frame is held and re-applied on every poll of
// the same game frame
#define PADREC_FRAME_MS 15
typedef struct {
    u16 button;
    s8 stickX, stickY, substickX, substickY;
    u8 analogA, analogB, triggerL, triggerR;
} PadRecFrame;  // 10 bytes

static PadRecFrame* s_padrec;
static u32 s_padrec_count, s_padrec_cap, s_padrec_idx;
static bool s_padrec_recording, s_padrec_playing;

static void padrec_reserve(u32 need) {
    if (need <= s_padrec_cap) return;
    u32 cap = s_padrec_cap ? s_padrec_cap : 65536;
    while (cap < need) cap *= 2;
    if ((u64)cap * sizeof(PadRecFrame) > PADREC_MAX_BYTES) {
        LOG_ERROR("pad", "padrec: stopped at %lu frames (%.0f MB limit)", (unsigned long)s_padrec_count,
                  PADREC_MAX_BYTES / 1048576.0);
        s_padrec_recording = false;
        return;
    }
    PadRecFrame* p = (PadRecFrame*)realloc(s_padrec, cap * sizeof(PadRecFrame));
    if (!p) {
        LOG_ERROR("pad", "padrec: out of memory at %lu frames", (unsigned long)cap);
        s_padrec_recording = false;
        return;
    }
    s_padrec = p;
    s_padrec_cap = cap;
}

static void padrec_write(void) {
    if (!s_padrec_count) return;
    FILE* f = fopen(PADREC_PATH, "wb");
    if (!f) {
        LOG_ERROR("pad", "padrec: cannot write %s", PADREC_PATH);
        return;
    }
    size_t ok = fwrite(s_padrec, sizeof(PadRecFrame), s_padrec_count, f);
    fclose(f);
    LOG_WARN("pad", "padrec: wrote %lu frames to %s", (unsigned long)ok, PADREC_PATH);
}

void padrec_flush(void) {
    if (s_padrec_count) padrec_write();  // Even if the recording stopped early
}

static void padrec_init(void) {
    s_padrec_recording = cfg_int("S3D_PAD_RECORD", 0) != 0;
    s_padrec_playing = cfg_int("S3D_PAD_PLAY", 0) != 0;
    if (!s_padrec_recording && !s_padrec_playing) return;
    if (s_padrec_recording) LOG_WARN("pad", "padrec: recording input (flushed at shutdown)");
    if (s_padrec_playing) {
        FILE* f = fopen(PADREC_PATH, "rb");
        if (!f) {
            LOG_ERROR("pad", "padrec: cannot open %s for playback", PADREC_PATH);
            s_padrec_playing = false;
            return;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        s_padrec_count = (u32)(size / (long)sizeof(PadRecFrame));
        s_padrec = (PadRecFrame*)malloc((size_t)s_padrec_count * sizeof(PadRecFrame));
        if (!s_padrec) {
            LOG_ERROR("pad", "padrec: out of memory loading %s", PADREC_PATH);
            fclose(f);
            s_padrec_playing = false;
            return;
        }
        size_t got = fread(s_padrec, sizeof(PadRecFrame), s_padrec_count, f);
        fclose(f);
        LOG_WARN("pad", "padrec: playing %lu recorded frames", (unsigned long)got);
        s_padrec_count = (u32)got;
    }
}

BOOL PADInit(void) {
    // HID is initialized by libctru at startup; irrst provides the C-Stick
    // (and ZL/ZR) on New 3DS and the Circle Pad Pro on Old 3DS.
    irrstInit();

    // Clear initial state
    memset(&g_pad_status, 0, sizeof(PADStatus));

    g_input_initialized = true;
    padrec_init();

    LOG_INFO("pad", "PAD bridge initialized\n");
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    if (!g_input_initialized || !status) {
        return 0;
    }

    // Refresh 3DS hardware state at the start of each read
    hidScanInput();

    // Update all controller components
    update_digital_buttons();
    update_analog_sticks();
    update_triggers();
    g_pad_status.err = PAD_ERR_NONE;
    if (remote_pad_buttons) {
        g_pad_status.button |= remote_pad_buttons;
        if (remote_pad_buttons & PAD_BUTTON_A) g_pad_status.analogA = 255;
        if (remote_pad_buttons & PAD_BUTTON_B) g_pad_status.analogB = 255;
    }
    if (remote_stick[0] || remote_stick[1]) {
        g_pad_status.stickX = remote_stick[0];
        g_pad_status.stickY = remote_stick[1];
    }
    // The touch screen item-menu pad (touchui_3ds.c) stands in for the C-stick
    // and A when it is used
    s8 vx, vy;
    u16 vbuttons;
    touchui_pad_state(&vx, &vy, &vbuttons);
    if (vx || vy) {
        g_pad_status.substickX = vx;
        g_pad_status.substickY = vy;
    }
    g_pad_status.button |= vbuttons;
    if (vbuttons & PAD_BUTTON_A) g_pad_status.analogA = 255;

    // Playback: advance one recorded frame per game frame and hold it, so the
    // recorded route wins over the real inputs even if the game polls PAD
    // several times per frame
    if (s_padrec_playing) {
        static u64 last_ms;
        static PadRecFrame held;
        static bool have;
        u64 now = osGetTime();
        if (!have || now - last_ms >= PADREC_FRAME_MS) {
            last_ms = now;
            have = true;
            if (s_padrec_idx < s_padrec_count) {
                held = s_padrec[s_padrec_idx++];
            } else {
                memset(&held, 0, sizeof(held));
                s_padrec_playing = false;  // Route over: real inputs return
                have = false;
            }
        }
        if (have) {
            g_pad_status.button = held.button;
            g_pad_status.stickX = held.stickX;
            g_pad_status.stickY = held.stickY;
            g_pad_status.substickX = held.substickX;
            g_pad_status.substickY = held.substickY;
            g_pad_status.analogA = held.analogA;
            g_pad_status.analogB = held.analogB;
            g_pad_status.triggerLeft = held.triggerL;
            g_pad_status.triggerRight = held.triggerR;
        }
    }

    // Record: one frame per ~15 ms (see PADREC_FRAME_MS)
    if (s_padrec_recording) {
        static u64 last_ms;
        u64 now = osGetTime();
        if (now - last_ms >= PADREC_FRAME_MS) {
            last_ms = now;
            padrec_reserve(s_padrec_count + 1);
            if (s_padrec) {
                PadRecFrame* f = &s_padrec[s_padrec_count++];
                f->button = g_pad_status.button;
                f->stickX = g_pad_status.stickX;
                f->stickY = g_pad_status.stickY;
                f->substickX = g_pad_status.substickX;
                f->substickY = g_pad_status.substickY;
                f->analogA = g_pad_status.analogA;
                f->analogB = g_pad_status.analogB;
                f->triggerL = g_pad_status.triggerLeft;
                f->triggerR = g_pad_status.triggerRight;
            }
        }
    }

    // The game passes an array of PAD_MAX_CONTROLLERS entries; the 3DS only
    // has one controller, so channels 1-3 report as disconnected.
    status[0] = g_pad_status;
    for (int chan = 1; chan < PAD_MAX_CONTROLLERS; chan++) {
        memset(&status[chan], 0, sizeof(PADStatus));
        status[chan].err = PAD_ERR_NO_CONTROLLER;
    }

    return 0;
}

void PADSetSamplingRate(u32 msec) {
    // Sampling happens in hidScanInput() calls, so the rate is ignored
    (void)msec;
}

static void update_digital_buttons(void) {
    u32 keys = hidKeysHeld();

    // Clear all button states
    g_pad_status.button = 0;

    // Map 3DS buttons to GameCube button equivalents
    if (keys & KEY_A)       g_pad_status.button |= PAD_BUTTON_A;
    if (keys & KEY_B)       g_pad_status.button |= PAD_BUTTON_B;
    if (keys & KEY_X)       g_pad_status.button |= PAD_BUTTON_X;
    if (keys & KEY_Y)       g_pad_status.button |= PAD_BUTTON_Y;
    if (keys & KEY_START)   g_pad_status.button |= PAD_BUTTON_START;
    if (keys & KEY_L)       g_pad_status.button |= PAD_TRIGGER_L;
    if (keys & KEY_R)       g_pad_status.button |= PAD_TRIGGER_R;
    if (keys & KEY_ZR)      g_pad_status.button |= PAD_TRIGGER_Z;  // Z mapped to ZR
    if (keys & KEY_DUP)     g_pad_status.button |= PAD_BUTTON_UP;
    if (keys & KEY_DDOWN)   g_pad_status.button |= PAD_BUTTON_DOWN;
    if (keys & KEY_DLEFT)   g_pad_status.button |= PAD_BUTTON_LEFT;
    if (keys & KEY_DRIGHT)  g_pad_status.button |= PAD_BUTTON_RIGHT;
}

// Scale a 3DS stick axis (about -156..156) to the GameCube range (-128..127)
static s8 scale_axis(s16 value) {
    if (value > 156) value = 156;
    if (value < -156) value = -156;
    return (s8)((value * 127) / 156);
}

static void update_analog_sticks(void) {
    // Circle pad -> main stick
    circlePosition circle_pad;
    hidCircleRead(&circle_pad);
    g_pad_status.stickX = scale_axis(circle_pad.dx);
    g_pad_status.stickY = scale_axis(circle_pad.dy);

    // C-Stick (New 3DS) -> GameCube C-stick where the game uses it; in normal
    // play it moves the camera instead (controls_3ds.c)
    g_pad_status.substickX = g_pad_status.substickY = 0;
    if (port_cstick_to_game()) port_cstick_read(&g_pad_status.substickX, &g_pad_status.substickY);
}

static void update_triggers(void) {
    u32 keys = hidKeysHeld();

    // The 3DS shoulder buttons are digital: report fully pressed or released
    g_pad_status.triggerLeft = (keys & KEY_L) ? 255 : 0;
    g_pad_status.triggerRight = (keys & KEY_R) ? 255 : 0;
    g_pad_status.analogA = (keys & KEY_A) ? 255 : 0;
    g_pad_status.analogB = (keys & KEY_B) ? 255 : 0;
}
