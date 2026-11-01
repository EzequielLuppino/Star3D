#include <3ds.h>
#include <stdlib.h>
#include "dl_guard.h"
#include "log.h"
#include "profiler.h"

#define DL_GUARD_MAGIC 0xD1CA17E5u
#define DL_GUARD_MAX (1u << 28)

// Layout: [magic][size] | interior (n bytes) | [magic]
void* dl_alloc(size_t n) {
    if (n > DL_GUARD_MAX) return NULL;
    u8* base = malloc(n + 12);
    if (!base) return NULL;
    u32* h = (u32*)base;
    h[0] = DL_GUARD_MAGIC;
    h[1] = (u32)n;
    ((u32*)(base + 8 + n))[0] = DL_GUARD_MAGIC;
    return base + 8;
}

void* dl_realloc(void* p, size_t n) {
    if (!p) return dl_alloc(n);
    if (n > DL_GUARD_MAX) return NULL;
    u8* base = (u8*)p - 8;
    u8* nb = realloc(base, n + 12);
    if (!nb) return NULL;
    u32* h = (u32*)nb;
    h[0] = DL_GUARD_MAGIC;
    h[1] = (u32)n;
    ((u32*)(nb + 8 + n))[0] = DL_GUARD_MAGIC;
    return nb + 8;
}

void dl_free(void* p, const char* what, const void* entry) {
    if (!p) return;
    u8* base = (u8*)p - 8;
    u32* h = (u32*)base;
    u32 n = h[1];
    u32 tail = n <= DL_GUARD_MAX ? ((u32*)(base + 8 + n))[0] : 0;
    if (h[0] != DL_GUARD_MAGIC || n > DL_GUARD_MAX || tail != DL_GUARD_MAGIC) {
        LOG_ERROR("dlguard", "BAD BLOCK in '%s': ptr %p head %08lX size %lu tail %08lX, entry %p (frame f%u)",
                  what, p, (unsigned long)h[0], (unsigned long)n, (unsigned long)tail, entry, prof_frame_count());
        log_flush();
        // The crash handler (os/crash.c) logs a [CRASH] block and dumps the
        // state; this is the moment the corruption is observable, where the
        // plain free() would have faulted with far less context
        svcBreak(USERBREAK_ASSERT);
    }
    free(base);
}
