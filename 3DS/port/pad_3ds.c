// PAD (controller) for 3DS: maps the 3DS buttons/sticks onto GameCube PADStatus.
#include "log.h"
#include "remote_3ds.h"
#include <dolphin/pad.h>
#include <3ds.h>
#include <stdio.h>
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

BOOL PADInit(void) {
    // HID is initialized by libctru at startup; irrst provides the C-Stick
    // (and ZL/ZR) on New 3DS and the Circle Pad Pro on Old 3DS.
    irrstInit();

    // Clear initial state
    memset(&g_pad_status, 0, sizeof(PADStatus));

    g_input_initialized = true;

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
