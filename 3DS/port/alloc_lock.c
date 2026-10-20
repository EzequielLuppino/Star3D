// libctru's linear and VRAM heaps have no lock (checked in libctru.a: the
// allocators call MemPool directly). Since MULTICORE_PLAN.md step 2 the render
// thread (gx/gx_rec.c) allocates textures and GPU buffers while the game
// thread allocates sounds (musyx_ndsp.c) and movie frames, so every call is
// wrapped (-Wl,--wrap, Makefile ALLOC_LOCKED) and takes this lock. --wrap also
// catches the calls from citro3d, citro2d and libctru's other objects.

#include <3ds.h>

static RecursiveLock s_lock;
static bool s_ready;

static void lock(void) {
    // The first allocation comes from the startup code on the main thread,
    // before any other thread exists
    if (!s_ready) {
        RecursiveLock_Init(&s_lock);
        s_ready = true;
    }
    RecursiveLock_Lock(&s_lock);
}

static void unlock(void) { RecursiveLock_Unlock(&s_lock); }

#define LOCKED(ret, name, params, args) \
    ret __real_##name params; \
    ret __wrap_##name params { lock(); ret r = __real_##name args; unlock(); return r; }
#define LOCKED_VOID(name, params, args) \
    void __real_##name params; \
    void __wrap_##name params { lock(); __real_##name args; unlock(); }

LOCKED(void*, linearAlloc, (size_t size), (size))
LOCKED(void*, linearMemAlign, (size_t size, size_t alignment), (size, alignment))
LOCKED(void*, linearRealloc, (void* mem, size_t size), (mem, size))
LOCKED(size_t, linearGetSize, (void* mem), (mem))
LOCKED_VOID(linearFree, (void* mem), (mem))
LOCKED(u32, linearSpaceFree, (void), ())
LOCKED(void*, vramAlloc, (size_t size), (size))
LOCKED(void*, vramAllocAt, (size_t size, vramAllocPos pos), (size, pos))
LOCKED(void*, vramMemAlign, (size_t size, size_t alignment), (size, alignment))
LOCKED(void*, vramMemAlignAt, (size_t size, size_t alignment, vramAllocPos pos), (size, alignment, pos))
LOCKED(void*, vramRealloc, (void* mem, size_t size), (mem, size))
LOCKED(size_t, vramGetSize, (void* mem), (mem))
LOCKED_VOID(vramFree, (void* mem), (mem))
LOCKED(u32, vramSpaceFree, (void), ())
