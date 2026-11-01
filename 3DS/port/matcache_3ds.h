#pragma once
#include <3ds.h>

// The object material cache (RENDERER_CHANNEL.md, the fork): the patched
// setup call site invokes port_mat_setup instead of objSetupRenderOpGxState
// directly. A hit restores the stored material state (the delta shadow's
// regions and the game's stage counters) and returns the render-op index
// the setup produced (0xFFFFFFFF = miss); the miss path runs the setup and
// stores the result via port_mat_end. port_mat_advance returns the
// bit-stream advance to add to bs.pos after a hit.
u32 port_mat_setup(void* obj, void* m, void* am, u32 bs_pos, const void* bs_data, void* anim, int lights);
u32 port_mat_end(void* obj, void* m, void* am, u32 bs_pos, const void* bs_data, void* anim, int lights,
                 u32 idx, u32 advance);
u32 port_mat_advance(void);
