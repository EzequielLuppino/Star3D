// Memory pool for MusyX (salMalloc: synth voices, effects, the DSP voice table,
// sample records). It sits in the game's bss block (ld/star3d.ld), so a save
// state snapshot carries every MusyX allocation along with the game memory and
// the pointers between them stay valid. First fit with lazy coalescing; when
// the pool is full it falls back to malloc, and while such a block is alive
// snapshots are refused (see savestate_snap.c).

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include "log.h"
#include "snapmem_3ds.h"

#define POOL_SIZE (12 * 1024 * 1024)  // High water ~10.2 MB (boot, hub)
#define ALIGN 16

typedef struct {
    u32 size;  // Block size including this header
    u32 used;
    u32 pad[2];
} Block;

static u8 sPool[POOL_SIZE] __attribute__((aligned(ALIGN)));
static u32 sTop;        // End of the blocks carved so far
static u32 sHighWater;  // Largest sTop
static u32 sInUse;
static int sFallbackLive;
static LightLock sLock;
static bool sLockInit;

static void lock(void) {
    if (!sLockInit) {
        LightLock_Init(&sLock);
        sLockInit = true;
    }
    LightLock_Lock(&sLock);
}

void* port_musyx_alloc(size_t size) {
    u32 need = (u32)((size + sizeof(Block) + ALIGN - 1) & ~(ALIGN - 1));
    lock();
    Block* found = NULL;
    for (u32 off = 0; off < sTop;) {
        Block* b = (Block*)(sPool + off);
        if (!b->used) {
            // Merge the free blocks that follow
            while (off + b->size < sTop) {
                Block* n = (Block*)(sPool + off + b->size);
                if (n->used) break;
                b->size += n->size;
            }
            if (off + b->size == sTop) {
                // Trailing free space goes back to the untouched area
                sTop = off;
                break;
            }
            if (b->size >= need) {
                found = b;
                break;
            }
        }
        off += b->size;
    }
    if (found) {
        if (found->size - need >= sizeof(Block) + ALIGN) {
            Block* rest = (Block*)((u8*)found + need);
            rest->size = found->size - need;
            rest->used = 0;
            found->size = need;
        }
    } else if (sTop + need <= POOL_SIZE) {
        found = (Block*)(sPool + sTop);
        found->size = need;
        sTop += need;
        if (sTop > sHighWater) sHighWater = sTop;
    }
    if (found) {
        found->used = 1;
        sInUse += found->size;
    }
    LightLock_Unlock(&sLock);
    if (found) return found + 1;

    void* p = malloc(size);
    if (p) {
        sFallbackLive++;
        LOG_WARN("snapmem", "MusyX pool full (%u KB, top %lu KB), malloc %lu bytes: save states disabled while in use",
                 POOL_SIZE / 1024, (unsigned long)(sTop / 1024), (unsigned long)size);
    }
    return p;
}

void port_musyx_free(void* p) {
    if (!p) return;
    if ((u8*)p < sPool || (u8*)p >= sPool + POOL_SIZE) {
        free(p);
        sFallbackLive--;
        return;
    }
    lock();
    Block* b = (Block*)p - 1;
    b->used = 0;
    sInUse -= b->size;
    LightLock_Unlock(&sLock);
}

bool port_musyx_pool_snapshot_ok(void) { return sFallbackLive == 0; }
void* port_musyx_pool_base(void) { return sPool; }

void port_musyx_pool_log(void) {
    LOG_INFO("snapmem", "MusyX pool: %lu KB in use, high water %lu of %u KB, %d malloc fallbacks",
             (unsigned long)(sInUse / 1024), (unsigned long)(sHighWater / 1024), POOL_SIZE / 1024,
             sFallbackLive);
}
