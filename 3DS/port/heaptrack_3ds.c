// C heap census (bug 30: the heap ran dry over a session, from ~9.6 MB free
// in the hub to 1.5 MB, until a map load failed). malloc, calloc, realloc,
// memalign and free are wrapped (Makefile, normal builds; PROFILE builds
// have their own tracker in perf/perf_wrap.c) and every live block is kept
// with its call site. heaptrack_report() logs the sites holding the most
// bytes: from every diag/mark block, and on the first failed allocations.
// Resolve the sites with addr2line on star3d.elf (the address is the return
// address, i.e. just after the call).
// newlib's internal allocations (stdio buffers, strdup) use _malloc_r and are
// not seen; the gap between "used" and "tracked" in the report is theirs.
#include "heaptrack_3ds.h"
#include <malloc.h>
#include <stdlib.h>
#include "log.h"

#ifndef PERF_WRAP

#define HT_BITS 14
#define HT_SLOTS (1u << HT_BITS)  // 16384 blocks, 128 KB (taken from the heap, keep it small)
#define HT_MASK (HT_SLOTS - 1)
#define HT_MAX_LIVE (HT_SLOTS * 3 / 4)
#define HT_SITES 256
#define HT_SIZE_MAX 0xFFFFFFu
#define HT_FAIL_LOGS 3

typedef struct {
    u32 p;
    u32 meta;  // size (24 bits, clamped) | site << 24
} HtBlock;

typedef struct {
    u32 addr;
    u32 bytes, count, allocs;
} HtSite;

static HtBlock s_blocks[HT_SLOTS];
static HtSite s_sites[HT_SITES];
static HtSite s_snap[HT_SITES];
static u32 s_live, s_live_bytes, s_untracked, s_fails, s_fail_logs;
static LightLock s_lock = 1;  // 1 = unlocked (LightLock_Init)
static volatile bool s_reporting;

static inline u32 slot_of(u32 p) {
    return ((p >> 3) * 2654435761u) >> (32 - HT_BITS);
}

// Site index for a call site (the last entry collects the overflow)
static u32 site_of(u32 addr) {
    u32 h = (addr >> 2) * 2654435761u >> 24;
    for (u32 i = 0; i < HT_SITES - 1; i++) {
        u32 k = (h + i) % (HT_SITES - 1);
        if (s_sites[k].addr == addr) return k;
        if (!s_sites[k].addr) {
            s_sites[k].addr = addr;
            return k;
        }
    }
    return HT_SITES - 1;
}

static void add_block(void* p, size_t n, void* caller) {
    if (s_live >= HT_MAX_LIVE) {
        s_untracked++;
        return;
    }
    u32 size = n > HT_SIZE_MAX ? HT_SIZE_MAX : (u32)n;
    u32 site = site_of((u32)caller);
    u32 i = slot_of((u32)p);
    while (s_blocks[i].p) i = (i + 1) & HT_MASK;
    s_blocks[i].p = (u32)p;
    s_blocks[i].meta = size | site << 24;
    s_live++;
    s_live_bytes += size;
    s_sites[site].bytes += size;
    s_sites[site].count++;
    s_sites[site].allocs++;
}

// Linear probing with backward-shift deletion (no tombstones)
static void del_block(void* ptr) {
    u32 p = (u32)ptr;
    u32 i = slot_of(p);
    while (s_blocks[i].p != p) {
        if (!s_blocks[i].p) return;  // Not tracked (allocated while the table was full)
        i = (i + 1) & HT_MASK;
    }
    u32 size = s_blocks[i].meta & HT_SIZE_MAX, site = s_blocks[i].meta >> 24;
    s_live--;
    s_live_bytes -= size;
    s_sites[site].bytes -= size;
    s_sites[site].count--;
    for (;;) {
        s_blocks[i].p = 0;
        u32 j = i;
        for (;;) {
            j = (j + 1) & HT_MASK;
            if (!s_blocks[j].p) return;
            u32 k = slot_of(s_blocks[j].p);
            // Entry j may stay if its home slot k lies cyclically in (i, j]
            if (i <= j ? (i < k && k <= j) : (i < k || k <= j)) continue;
            break;
        }
        s_blocks[i] = s_blocks[j];
        i = j;
    }
}

static void failed(size_t n, void* caller) {
    LightLock_Lock(&s_lock);
    s_fails++;
    bool log = s_fail_logs < HT_FAIL_LOGS && !s_reporting;
    if (log) s_fail_logs++;
    LightLock_Unlock(&s_lock);
    if (!log) return;
    LOG_WARN("heap", "allocation of %lu bytes failed (site %08lX)", (unsigned long)n, (unsigned long)(u32)caller);
    heaptrack_report("heap");
}

static void note(void* p, size_t n, void* caller) {
    if (!p) {
        if (n) failed(n, caller);
        return;
    }
    LightLock_Lock(&s_lock);
    add_block(p, n, caller);
    LightLock_Unlock(&s_lock);
}

void* __real_malloc(size_t n);
void* __wrap_malloc(size_t n) {
    void* p = __real_malloc(n);
    note(p, n, __builtin_return_address(0));
    return p;
}

void* __real_calloc(size_t a, size_t b);
void* __wrap_calloc(size_t a, size_t b) {
    void* p = __real_calloc(a, b);
    note(p, a * b, __builtin_return_address(0));
    return p;
}

void* __real_memalign(size_t al, size_t n);
void* __wrap_memalign(size_t al, size_t n) {
    void* p = __real_memalign(al, n);
    note(p, n, __builtin_return_address(0));
    return p;
}

void* __real_realloc(void* old, size_t n);
void* __wrap_realloc(void* old, size_t n) {
    // Untrack first: once realloc returns, another thread may get 'old'
    if (old) {
        LightLock_Lock(&s_lock);
        del_block(old);
        LightLock_Unlock(&s_lock);
    }
    void* p = __real_realloc(old, n);
    if (p) note(p, n, __builtin_return_address(0));
    else if (n && old) note(old, malloc_usable_size(old), __builtin_return_address(0));  // Failed: old stays
    if (!p && n) failed(n, __builtin_return_address(0));
    return p;
}

void __real_free(void* p);
void __wrap_free(void* p) {
    if (p) {
        LightLock_Lock(&s_lock);
        del_block(p);
        LightLock_Unlock(&s_lock);
    }
    __real_free(p);
}

u32 heaptrack_heap_free(void) {
    extern u32 __ctru_heap_size;
    struct mallinfo mi = mallinfo();
    return (__ctru_heap_size - (u32)mi.arena) + (u32)mi.fordblks;
}

// Largest block malloc can still return (fragmentation), to 64 KB steps
static u32 largest_block(void) {
    u32 lo = 0, hi = 32u << 20;
    while (hi - lo > (64u << 10)) {
        u32 mid = (lo + hi) / 2;
        void* p = __real_malloc(mid);
        if (p) {
            __real_free(p);
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

void heaptrack_report(const char* tag) {
    LightLock_Lock(&s_lock);
    if (s_reporting) {
        LightLock_Unlock(&s_lock);
        return;
    }
    s_reporting = true;
    u32 n = 0;
    for (u32 i = 0; i < HT_SITES; i++) {
        if (s_sites[i].count) s_snap[n++] = s_sites[i];
    }
    u32 live = s_live, live_bytes = s_live_bytes, untracked = s_untracked, fails = s_fails;
    LightLock_Unlock(&s_lock);

    extern u32 __ctru_heap_size;
    extern u32 port_aram_high_water(void);
    extern u32 port_aram_used(void);
    LOG_INFO(tag, "heap: free %lu KB of %lu, largest block %lu KB | tracked %lu KB in %lu blocks, %lu untracked "
             "allocs, %lu failed | ARAM (16 MB, in the heap) used %lu KB, high water %lu KB",
             (unsigned long)(heaptrack_heap_free() >> 10), (unsigned long)(__ctru_heap_size >> 10),
             (unsigned long)(largest_block() >> 10), (unsigned long)(live_bytes >> 10), (unsigned long)live,
             (unsigned long)untracked, (unsigned long)fails, (unsigned long)(port_aram_used() >> 10),
             (unsigned long)(port_aram_high_water() >> 10));
    for (u32 k = 0; k < 12 && k < n; k++) {
        u32 best = k;
        for (u32 j = k + 1; j < n; j++)
            if (s_snap[j].bytes > s_snap[best].bytes) best = j;
        HtSite t = s_snap[k];
        s_snap[k] = s_snap[best];
        s_snap[best] = t;
        if (s_snap[k].bytes < (16u << 10)) break;
        LOG_INFO(tag, "heap   site %08lX: %6lu KB in %5lu blocks (%lu allocs)", (unsigned long)s_snap[k].addr,
                 (unsigned long)(s_snap[k].bytes >> 10), (unsigned long)s_snap[k].count,
                 (unsigned long)s_snap[k].allocs);
    }
    s_reporting = false;
}

#else

u32 heaptrack_heap_free(void) {
    extern u32 __ctru_heap_size;
    struct mallinfo mi = mallinfo();
    return (__ctru_heap_size - (u32)mi.arena) + (u32)mi.fordblks;
}

void heaptrack_report(const char* tag) {}

#endif
