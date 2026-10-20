// Draw-source attribution, see draw_src.h. Time per tag is exclusive: a
// nested tag's time is not counted in its parent.

#include "draw_src.h"
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "profiler.h"

#define REPORT_FRAMES 300
#define STACK_DEPTH 32
#define CALLER_SLOTS 512
#define TOP_CALLERS 24

static const char* const s_names[DSRC_COUNT] = {
    "other", "scene", "transp", "map", "mappass", "obj", "fur",
    "shadow", "expgfx", "sky", "water", "refl", "postfx", "hud",
};

typedef struct {
    u64 ticks;
    u32 draws, tris, entries;
} TagStats;

typedef struct {
    void* pc;
    u8 tag;
    u32 draws, tris;
} CallerStats;

static TagStats s_tags[DSRC_COUNT];
static CallerStats s_callers[CALLER_SLOTS];
static u32 s_callers_lost;
static u8 s_stack[STACK_DEPTH];
static int s_depth;  // Entries past STACK_DEPTH are counted but not stored
static u64 s_last;
static void* s_caller;
static u8 s_batch_tag;
static void* s_batch_caller;
static u32 s_frames;

static DrawSrcTag current(void) {
    if (s_depth == 0) return DSRC_OTHER;
    return s_stack[(s_depth > STACK_DEPTH ? STACK_DEPTH : s_depth) - 1];
}

// Charge the time since the last switch to the current tag
static void charge(void) {
    u64 now = svcGetSystemTick();
    if (s_last) s_tags[current()].ticks += now - s_last;
    s_last = now;
}

void draw_src_push(DrawSrcTag tag) {
    charge();
    if (s_depth < STACK_DEPTH) s_stack[s_depth] = tag;
    s_depth++;
    s_tags[tag].entries++;
}

void draw_src_pop(void) {
    charge();
    if (s_depth > 0) s_depth--;
}

void draw_src_set_caller(void* pc) {
    s_caller = pc;
    // Immediate-mode batches start in GXBegin's vertex allocation, before
    // GXEnd reports who drew them: take the first GXEnd caller
    if (pc && !s_batch_caller) s_batch_caller = pc;
}

static void add(DrawSrcTag tag, void* pc, u32 tris) {
    s_tags[tag].draws++;
    s_tags[tag].tris += tris;
    u32 h = (((uintptr_t)pc >> 2) * 2654435761u + tag * 40503u) % CALLER_SLOTS;
    for (int i = 0; i < CALLER_SLOTS; i++, h = (h + 1) % CALLER_SLOTS) {
        CallerStats* c = &s_callers[h];
        if (c->draws == 0) { c->pc = pc; c->tag = tag; }
        if (c->pc == pc && c->tag == tag) {
            c->draws++;
            c->tris += tris;
            return;
        }
    }
    s_callers_lost++;
}

void draw_src_batch_begin(void) {
    s_batch_tag = current();
    s_batch_caller = s_caller;
}

void draw_src_draw(u32 tris) { add(current(), s_caller, tris); }
void draw_src_draw_batch(u32 tris) { add(s_batch_tag, s_batch_caller, tris); }

static int cmp_callers(const void* a, const void* b) {
    const CallerStats* x = a;
    const CallerStats* y = b;
    return (int)y->draws - (int)x->draws;
}

void draw_src_frame_end(void) {
    charge();
    if (++s_frames < REPORT_FRAMES) return;

    u32 n = s_frames;
    u32 draws = 0, tris = 0;
    u64 ticks = 0;
    for (int i = 0; i < DSRC_COUNT; i++) {
        draws += s_tags[i].draws;
        tris += s_tags[i].tris;
        ticks += s_tags[i].ticks;
    }
    LOG_INFO("dsrc", "draw sources over %lu frames: %.1f draws/frame, %lu tris/frame, %.2f ms/frame (stack depth %d)",
             (unsigned long)n, (double)draws / n, (unsigned long)(tris / n),
             (double)ticks / n / CPU_TICKS_PER_MSEC, s_depth);
    for (int i = 0; i < DSRC_COUNT; i++) {
        TagStats* t = &s_tags[i];
        if (!t->draws && !t->entries && i != DSRC_OTHER) continue;
        LOG_INFO("dsrc", "  %-8s draws %6.1f  tris %6lu  ms %6.2f  entries %6.1f", s_names[i],
                 (double)t->draws / n, (unsigned long)(t->tris / n), (double)t->ticks / n / CPU_TICKS_PER_MSEC,
                 (double)t->entries / n);
    }

    // Compact the hash table and list the busiest callers
    int used = 0;
    for (int i = 0; i < CALLER_SLOTS; i++)
        if (s_callers[i].draws) s_callers[used++] = s_callers[i];
    qsort(s_callers, used, sizeof(CallerStats), cmp_callers);
    for (int i = 0; i < used && i < TOP_CALLERS; i++) {
        CallerStats* c = &s_callers[i];
        LOG_INFO("dsrc", "  caller %08lx %-8s draws %6.1f  tris %6lu", (unsigned long)(uintptr_t)c->pc,
                 s_names[c->tag], (double)c->draws / n, (unsigned long)(c->tris / n));
    }
    if (s_callers_lost) LOG_INFO("dsrc", "  %lu draws lost (caller table full)", (unsigned long)s_callers_lost);
    log_flush();

    memset(s_tags, 0, sizeof(s_tags));
    memset(s_callers, 0, sizeof(s_callers));
    s_callers_lost = 0;
    s_frames = 0;
}
