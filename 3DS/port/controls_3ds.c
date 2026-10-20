// 3DS control extras (CONTROLS.md):
// - C-stick camera: in normal play the C-stick orbits the follow camera
//   (dlls/engine/66, patches/dlls/engine/66/66.c.sed) instead of opening the
//   item menu. The item menu moved to the touch screen (touchui_3ds.c, Map
//   tab). Menus, first person, the world map and the title still get the
//   C-stick as before (port_cstick_to_game).
// - Gyro aiming: while Fox aims the staff (Fire Blaster etc.) turning and
//   tilting the console moves the aim (patches/dlls/objects/195_Player).
// - Aim hold: the aim no longer springs back to the centre when the stick is
//   let go; the stick moves it, it stays where it was left.
// Replaces foxhollow's foxhollow_controls.cpp (JSON-configurable PC
// controller remapping). On 3DS the button mapping is fixed in port/pad_3ds.c.

#include <3ds.h>
#include <dolphin/pad.h>
#include <math.h>
#include "config.h"
#include "log.h"
#include "remote_3ds.h"

void fhControlsInit(void) {}
void fhControlsUpdate(void) {}

// Game state (dlls/engine/1_camcontrol/camcontrol.c, dlls/engine/0/0.c, main/frame_timing.h)
extern s32 gCamcontrolActiveActionId;
extern u8 pauseMenuState;
extern f32 timeDelta;
void* Obj_GetPlayerObject(void);

#define CAM_NORMAL 0x42      // DLL 66, third person follow
#define CAM_FIRST_PERSON 0x44  // DLL 68, Z viewfinder: C-stick Y zooms
#define CAM_DEBUG 0x46       // DLL 70: C-stick orbits
#define CAM_WORLD_MAP 0x4E   // DLL 78: C-stick turns the globe

// --- C-stick -------------------------------------------------------------------

static s8 scale_axis(s16 value) {
    if (value > 156) value = 156;
    if (value < -156) value = -156;
    return (s8)((value * 127) / 156);
}

// The physical C-stick (or the remote "cstick" command), -127..127
void port_cstick_read(s8* x, s8* y) {
    circlePosition c;
    irrstCstickRead(&c);
    *x = scale_axis(c.dx);
    *y = scale_axis(c.dy);
    if (remote_cstick[0] || remote_cstick[1]) {
        *x = remote_cstick[0];
        *y = remote_cstick[1];
    }
}

// S3D_CSTICK_CAMERA (default 1): whether the physical C-stick goes to the
// game's C-stick (menus, first person zoom, world map, title, no Fox) or
// moves the camera (everything else; the game sees a centred C-stick there)
bool port_cstick_to_game(void) {
    static int on = 1;
    CFG_LIVE(on, cfg_bool("S3D_CSTICK_CAMERA", true));
    if (!on) return true;
    if (Obj_GetPlayerObject() == NULL || pauseMenuState != 0) return true;
    int mode = gCamcontrolActiveActionId;
    return mode == CAM_FIRST_PERSON || mode == CAM_DEBUG || mode == CAM_WORLD_MAP;
}

// --- Camera orbit (DLL 66) -----------------------------------------------------

static float s_orbit_height;  // World units added to the camera's height band
static bool s_recenter;

static float dead_zone(float v, float dz) {
    if (v > dz) return (v - dz) / (1.0f - dz);
    if (v < -dz) return (v + dz) / (1.0f - dz);
    return 0.0f;
}

// Called by the normal camera's update, once per game frame: the yaw to turn
// the camera around Fox this frame, in radians. S3D_CAM_SPEED is degrees per
// second at full tilt (default 150), S3D_CAM_INVERT_X/Y flip the axes
float port_camera_orbit_yaw(void) {
    static int on = 1, speed = 150, inv_x = 0, inv_y = 0;
    CFG_LIVE(on, cfg_bool("S3D_CSTICK_CAMERA", true));
    CFG_LIVE(speed, cfg_int("S3D_CAM_SPEED", 150));
    CFG_LIVE(inv_x, cfg_bool("S3D_CAM_INVERT_X", false));
    CFG_LIVE(inv_y, cfg_bool("S3D_CAM_INVERT_Y", false));
    if (s_recenter) {
        s_recenter = false;
        s_orbit_height = 0.0f;
    }
    if (!on) {
        s_orbit_height = 0.0f;
        return 0.0f;
    }
    s8 cx, cy;
    port_cstick_read(&cx, &cy);
    float x = dead_zone(cx / 127.0f, 0.15f), y = dead_zone(cy / 127.0f, 0.15f);
    if (inv_x) x = -x;
    if (inv_y) y = -y;
    float dt = timeDelta / 60.0f;  // timeDelta counts 60 Hz frames
    // Up raises the camera (looks down on Fox), down lowers it
    s_orbit_height += y * 60.0f * dt;
    if (s_orbit_height > 45.0f) s_orbit_height = 45.0f;
    if (s_orbit_height < -25.0f) s_orbit_height = -25.0f;
    // Right swings the camera to Fox's left, so the view turns right
    return -x * speed * dt * (3.1415927f / 180.0f);
}

// Added to the normal camera's height band (66.c CameraModeNormal_updateSlide)
float port_camera_orbit_height(void) {
    return s_orbit_height;
}

// --- Gyro aiming -----------------------------------------------------------------

static bool s_gyro_ready;
static float s_gyro_coef = 14.375f;  // Raw units per degree/second
static float s_gyro_deg[2];          // This frame's turn: [0] yaw (right +), [1] pitch (up +)
static u64 s_gyro_tick;
static float s_remote_gyro[2];       // Remote "gyro" command, degrees per second
static int s_remote_gyro_frames;

void port_remote_gyro(float yaw_dps, float pitch_dps, int frames) {
    s_remote_gyro[0] = yaw_dps;
    s_remote_gyro[1] = pitch_dps;
    s_remote_gyro_frames = frames;
}

static void gyro_init(void) {
    if (R_FAILED(HIDUSER_EnableGyroscope()) || R_FAILED(HIDUSER_EnableAccelerometer())) {
        LOG_WARN("controls", "gyroscope unavailable, gyro aiming off");
        return;
    }
    float coef = 0.0f;
    if (R_SUCCEEDED(HIDUSER_GetGyroscopeRawToDpsCoefficient(&coef)) && coef > 1.0f) s_gyro_coef = coef;
    LOG_INFO("controls", "gyroscope on (%.3f raw per deg/s)", s_gyro_coef);
    s_gyro_ready = true;
}

// Once per game frame, after hidScanInput (vi_3ds.c). Yaw is the turn around
// the world's vertical (the angular rate projected on the accelerometer's up
// vector), so turning works however far the console is tilted back; pitch is
// the turn around the console's left-right axis
void port_controls_frame(void) {
    static bool inited;
    if (!inited) {
        inited = true;
        gyro_init();
    }
    // L recentres the camera behind Fox (DLL 67); the height goes back too
    static bool l_prev;
    bool l = (hidKeysHeld() & KEY_L) || (remote_pad_buttons & PAD_TRIGGER_L);
    if (l && !l_prev) s_recenter = true;
    l_prev = l;

    u64 now = svcGetSystemTick();
    float dt = s_gyro_tick ? (float)(now - s_gyro_tick) / SYSCLOCK_ARM11 : 0.0f;
    s_gyro_tick = now;
    if (dt > 0.1f) dt = 0.1f;  // After loads and the HOME menu
    s_gyro_deg[0] = s_gyro_deg[1] = 0.0f;
    if (s_remote_gyro_frames > 0) {
        s_remote_gyro_frames--;
        s_gyro_deg[0] = s_remote_gyro[0] * dt;
        s_gyro_deg[1] = s_remote_gyro[1] * dt;
        return;
    }
    if (!s_gyro_ready) return;
    angularRate w;
    accelVector a;
    hidGyroRead(&w);
    hidAccelRead(&a);
    float wx = w.x / s_gyro_coef, wy = w.y / s_gyro_coef, wz = w.z / s_gyro_coef;
    float ax = a.x, ay = a.y, az = a.z;
    float len = sqrtf(ax * ax + ay * ay + az * az);
    float yaw_rate;
    if (len > 100.0f) yaw_rate = (wx * ax + wy * ay + wz * az) / len;
    else yaw_rate = wy;  // Free fall or no reading: assume the console is upright
    // A positive turn around "up" is to the left; aim right is positive
    s_gyro_deg[0] = -yaw_rate * dt;
    s_gyro_deg[1] = wx * dt;
}

// --- Aiming (playerStateAimStaff) ------------------------------------------------

// S3D_AIM_HOLD (default 1): the aim stays where it was left. 0 restores the
// game's aim, which eases to the stick position (so it springs back to the
// centre on release, camera included)
bool port_aim_hold(void) {
    static int on = 1;
    CFG_LIVE(on, cfg_bool("S3D_AIM_HOLD", true));
    return on;
}

static float s_aim_turn;  // Body yaw the aim pushed past the screen edge (game units)

// One aim axis, called by playerStateAimStaff once per frame: axis 0 is
// aimInputX (right +), 1 is aimInputZ (stick up +, which aims down). stick is
// the main stick, -1..1. Returns the new value, -1..1
float port_aim_axis(float v, float stick, int axis) {
    static int gyro = 1, sens = 100, inv_x = 0, inv_y = 0;
    CFG_LIVE(gyro, cfg_bool("S3D_GYRO_AIM", true));
    CFG_LIVE(sens, cfg_int("S3D_GYRO_SENS", 100));
    CFG_LIVE(inv_x, cfg_bool("S3D_GYRO_INVERT_X", false));
    CFG_LIVE(inv_y, cfg_bool("S3D_GYRO_INVERT_Y", false));
    // Full tilt crosses from the centre to the edge in about half a second
    v += dead_zone(stick, 0.12f) * 2.0f * (timeDelta / 60.0f);
    if (gyro) {
        float deg = s_gyro_deg[axis];
        s_gyro_deg[axis] = 0.0f;
        if (axis == 0 ? inv_x : inv_y) deg = -deg;
        // 100 = 25 degrees of console turn from the centre to the edge
        float d = deg * (sens / 100.0f) / 25.0f;
        v += axis == 0 ? d : -d;  // Tilting up aims up, which is aimInputZ down
    }
    if (v > 1.0f || v < -1.0f) {
        float edge = v > 0.0f ? 1.0f : -1.0f;
        // Sideways past the edge turns Fox (the game's own edge spin starts
        // at 0.75 and keeps turning while the aim stays there)
        if (axis == 0) s_aim_turn += (v - edge) * 4096.0f;
        v = edge;
    }
    return v;
}

// Added to the body yaw in playerStateAimStaff, once per frame
float port_aim_take_turn(void) {
    float t = -s_aim_turn;  // The game's yaw grows to the left
    s_aim_turn = 0.0f;
    return t;
}
