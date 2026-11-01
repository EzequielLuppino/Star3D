// S3D_STEREO_AUTO: the 3D slider switches the quality profile. With the
// slider up the GPU draws both eyes - the game's vertex load doubles, which
// full quality cannot carry (measured ~66-73 ms GPU a frame, ~15 fps) - so
// the frame applies the cut-effects profile (the 30 fps chase config). With
// the slider down it restores the boot settings (the full-quality profile).
// The switch runs on the game thread at the frame start; every key in the
// profile is read live, so it takes effect from the next frame, no restart.
// The boot values are snapshotted at init: touch-menu changes made while the
// game runs are restored over on the next slider-down (they apply fully on
// the next boot).

#include <3ds.h>
#include "config.h"
#include "log.h"

typedef struct {
    const char* key;
    int stereo_value;
} ProfileKey;

// The cuts config the 30 fps-with-3D chase was measured with (27.4 ms/frame)
static const ProfileKey s_profile[] = {
    { "S3D_OBJ_DIST", 30 },
    { "S3D_OBJ_MINPX", 45 },
    { "S3D_PLANTS", 0 },
    { "S3D_SHADOWS", 0 },
    { "S3D_FUR_MODE", 0 },
    { "S3D_GRASS_BUDGET", 0 },
    { "S3D_GPU_BLUR", 0 },
    { "S3D_WATER_TESS", 0 },
    { "S3D_WATER_RIPPLE", 0 },
};
#define PROFILE_COUNT (sizeof(s_profile) / sizeof(s_profile[0]))

static int s_boot[PROFILE_COUNT];
static bool s_last_stereo;

void stereo_auto_init(void) {
    for (u32 i = 0; i < PROFILE_COUNT; i++) s_boot[i] = cfg_int(s_profile[i].key, 0);
    s_last_stereo = false;  // The first frame applies the profile if the slider is up
}

void stereo_auto_frame(float slider) {
    static int on = -1;
    CFG_LIVE(on, cfg_int("S3D_STEREO_AUTO", 1));
    bool stereo = on && slider > 0.0f;
    if (stereo == s_last_stereo) return;
    s_last_stereo = stereo;
    if (stereo) {
        LOG_WARN("stereo", "3D on: applying the cut-effects profile");
        for (u32 i = 0; i < PROFILE_COUNT; i++) cfg_set(s_profile[i].key, s_profile[i].stereo_value);
    } else {
        LOG_WARN("stereo", "3D off: restoring the boot quality settings");
        for (u32 i = 0; i < PROFILE_COUNT; i++) cfg_set(s_profile[i].key, s_boot[i]);
    }
}
