// Records huge/NaN vertex positions and matrices (see gx_diag.h)

#include "gx_diag.h"
#include <stdio.h>
#include <string.h>
#include "gx_internal.h"
#include "log.h"


#define RING 8

typedef struct {
    const char* what;
    const void* caller;
    u32 id, frame;
    float v[3];
} BadValue;

static BadValue s_ring[RING];
static u32 s_count;

void gx_diag_bad_value(const char* what, const float* v, int n, u32 id, const void* caller) {
    BadValue* b = &s_ring[s_count % RING];
    b->what = what;
    b->caller = caller;
    b->id = id;
    b->frame = gx_render_frame_number();
    for (int i = 0; i < 3; i++) b->v[i] = i < n ? v[i] : 0.0f;
    s_count++;
    // First 16, then every 1024th
    if (s_count <= 16 || (s_count & 1023) == 0)
        LOG_WARN("badvtx", "#%lu %s id %lu (%g %g %g) caller %p", (unsigned long)s_count, what, (unsigned long)id,
                 (double)b->v[0], (double)b->v[1], (double)b->v[2], caller);
}

u32 gx_diag_dump_bad_values(void) {
    u32 first = s_count > RING ? s_count - RING : 0;
    for (u32 i = first; i < s_count; i++) {
        const BadValue* b = &s_ring[i % RING];
        LOG_WARN("mark", "bad #%lu frame %lu %s id %lu (%g %g %g) caller %p", (unsigned long)(i + 1),
                 (unsigned long)b->frame, b->what, (unsigned long)b->id, (double)b->v[0], (double)b->v[1],
                 (double)b->v[2], b->caller);
    }
    return s_count;
}
