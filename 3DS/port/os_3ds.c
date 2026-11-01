// Dolphin OS layer for 3DS: arena and heaps (OSAlloc), time, interrupts,
// reporting/panics and misc system settings. Replaces Aurora's lib/dolphin/os
// and foxhollow's port/src/os_shim.c.

#include <3ds.h>
#include <dolphin/os.h>
#include <stdarg.h>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gx_rec.h"
#include "gx_init.h"
#include "log.h"
#include "netsrv_3ds.h"
#include "port_3ds.h"
#include "watchdog.h"

#define ALIGN32(x) (((x) + 31u) & ~31u)

// --- Arena -----------------------------------------------------------------

static u8* sMem1;
static void* sArenaLo;
static void* sArenaHi;

void* OSGetArenaLo(void) { return sArenaLo; }
void* OSGetArenaHi(void) { return sArenaHi; }
void OSSetArenaLo(void* newLo) { sArenaLo = newLo; }
void OSSetArenaHi(void* newHi) { sArenaHi = newHi; }
u32 OSGetPhysicalMemSize(void) { return PORT_MEM1_SIZE; }
u32 OSGetConsoleType(void) { return OS_CONSOLE_RETAIL1; }

// --- Heaps (OSAlloc) ---------------------------------------------------------
// First-fit allocator over an address-ordered block list. Every block has a
// 32-byte header so payloads stay 32-byte aligned like on GameCube.

typedef struct HeapBlock {
    struct HeapBlock* next;
    u32 size;  // Total size including header
    u32 used;
    u8 pad[32 - sizeof(void*) - 2 * sizeof(u32)];
} HeapBlock;

typedef struct {
    bool active;
    HeapBlock* first;
    u32 bytes_used;
} Heap;

#define MAX_HEAPS 8
static Heap sHeaps[MAX_HEAPS];
static int sMaxHeaps;
volatile OSHeapHandle __OSCurrHeap = -1;

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps) {
    (void)arenaEnd;
    memset(sHeaps, 0, sizeof(sHeaps));
    sMaxHeaps = maxHeaps > MAX_HEAPS ? MAX_HEAPS : maxHeaps;
    if (maxHeaps > MAX_HEAPS) {
        LOG_WARN("os", "OSInitAlloc: %d heaps requested, only %d supported", maxHeaps, MAX_HEAPS);
    }
    // Heap descriptors live in static storage, so the arena is not consumed
    return (void*)ALIGN32((uintptr_t)arenaStart);
}

OSHeapHandle OSCreateHeap(void* start, void* end) {
    uintptr_t lo = ALIGN32((uintptr_t)start);
    uintptr_t hi = (uintptr_t)end & ~31u;

    for (int i = 0; i < sMaxHeaps; i++) {
        if (!sHeaps[i].active && hi > lo + sizeof(HeapBlock)) {
            HeapBlock* block = (HeapBlock*)lo;
            block->next = NULL;
            block->size = hi - lo;
            block->used = 0;
            sHeaps[i].active = true;
            sHeaps[i].first = block;
            sHeaps[i].bytes_used = 0;
            LOG_INFO("os", "OSCreateHeap %d: %p-%p (%lu KB)", i, (void*)lo, (void*)hi,
                     (unsigned long)((hi - lo) / 1024));
            return i;
        }
    }
    LOG_ERROR("os", "OSCreateHeap(%p, %p) failed", start, end);
    return -1;
}

void OSDestroyHeap(OSHeapHandle heap) {
    if (heap >= 0 && heap < sMaxHeaps) {
        sHeaps[heap].active = false;
    }
}

OSHeapHandle OSSetCurrentHeap(OSHeapHandle heap) {
    OSHeapHandle prev = __OSCurrHeap;
    __OSCurrHeap = heap;
    return prev;
}

void* OSAllocFromHeap(OSHeapHandle heap, u32 size) {
    if (heap < 0 || heap >= sMaxHeaps || !sHeaps[heap].active) {
        LOG_ERROR("os", "OSAllocFromHeap: invalid heap %d", heap);
        return NULL;
    }

    u32 need = ALIGN32(size) + sizeof(HeapBlock);
    for (HeapBlock* block = sHeaps[heap].first; block; block = block->next) {
        if (block->used || block->size < need) continue;

        // Split if the remainder can hold another block
        if (block->size - need >= 2 * sizeof(HeapBlock)) {
            HeapBlock* rest = (HeapBlock*)((u8*)block + need);
            rest->next = block->next;
            rest->size = block->size - need;
            rest->used = 0;
            block->next = rest;
            block->size = need;
        }
        block->used = 1;
        sHeaps[heap].bytes_used += block->size;
        return block + 1;
    }

    LOG_ERROR("os", "OSAllocFromHeap(%d, 0x%lX): out of memory (%lu KB used)", heap,
              (unsigned long)size, (unsigned long)(sHeaps[heap].bytes_used / 1024));
    return NULL;
}

void OSFreeToHeap(OSHeapHandle heap, void* ptr) {
    if (!ptr || heap < 0 || heap >= sMaxHeaps || !sHeaps[heap].active) return;

    HeapBlock* target = (HeapBlock*)ptr - 1;
    HeapBlock* prev = NULL;
    for (HeapBlock* block = sHeaps[heap].first; block; prev = block, block = block->next) {
        if (block != target) continue;
        if (!block->used) {
            LOG_ERROR("os", "OSFreeToHeap: double free of %p", ptr);
            return;
        }

        block->used = 0;
        sHeaps[heap].bytes_used -= block->size;

        // Coalesce with the following and preceding free blocks
        if (block->next && !block->next->used) {
            block->size += block->next->size;
            block->next = block->next->next;
        }
        if (prev && !prev->used) {
            prev->size += block->size;
            prev->next = block->next;
        }
        return;
    }
    LOG_ERROR("os", "OSFreeToHeap: %p is not in heap %d", ptr, heap);
}

// --- Time ------------------------------------------------------------------
// OSTime counts OS_TIMER_CLOCK ticks since 2000-01-01 like on GameCube.

#define GC_EPOCH_UNIX 946684800LL

static u64 sBootSysTick;
static OSTime sBootOSTime;

static OSTime systicks_to_ostime(u64 ticks) {
    // Split to avoid 64-bit overflow of ticks * OS_TIMER_CLOCK
    return (OSTime)((ticks / SYSCLOCK_ARM11) * OS_TIMER_CLOCK +
                    (ticks % SYSCLOCK_ARM11) * OS_TIMER_CLOCK / SYSCLOCK_ARM11);
}

OSTime OSGetTime(void) {
    return sBootOSTime + systicks_to_ostime(svcGetSystemTick() - sBootSysTick);
}

OSTick OSGetTick(void) {
    return (OSTick)OSGetTime();
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td) {
    s64 secs = ticks / OS_TIMER_CLOCK;
    s64 sub = ticks % OS_TIMER_CLOCK;
    time_t unix_time = (time_t)(secs + GC_EPOCH_UNIX);
    struct tm tm;
    gmtime_r(&unix_time, &tm);

    td->sec = tm.tm_sec;
    td->min = tm.tm_min;
    td->hour = tm.tm_hour;
    td->mday = tm.tm_mday;
    td->mon = tm.tm_mon;
    td->year = tm.tm_year + 1900;
    td->wday = tm.tm_wday;
    td->yday = tm.tm_yday;
    td->msec = (int)(sub / (OS_TIMER_CLOCK / 1000));
    td->usec = (int)((sub * 8 / (OS_TIMER_CLOCK / 125000)) % 1000);
}

// --- Interrupts ----------------------------------------------------------------
// Single-threaded: "interrupts" only gate delivery of DVD completion callbacks.

static BOOL sInterruptsEnabled = TRUE;

BOOL OSDisableInterrupts(void) {
    BOOL prev = sInterruptsEnabled;
    sInterruptsEnabled = FALSE;
    return prev;
}

BOOL OSEnableInterrupts(void) {
    BOOL prev = sInterruptsEnabled;
    sInterruptsEnabled = FALSE;
    aurora_dvd_process_callbacks();
    sInterruptsEnabled = TRUE;
    return prev;
}

BOOL OSRestoreInterrupts(BOOL level) {
    BOOL prev = sInterruptsEnabled;
    if (level) {
        sInterruptsEnabled = FALSE;
        aurora_dvd_process_callbacks();
    }
    sInterruptsEnabled = level;
    return prev;
}

// --- Reporting, panics ---------------------------------------------------------

void OSReport(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    log_vwrite(LOG_LEVEL_INFO, "OSReport", msg, args);
    va_end(args);
}

void OSVReport(const char* msg, va_list list) {
    log_vwrite(LOG_LEVEL_INFO, "OSReport", msg, list);
}

// Show a fatal error on the bottom screen and wait for START, then exit
static void __attribute__((noreturn)) fatal_screen(const char* title, const char* message) {
    log_flush();
    printf("\n\x1b[31m*** %s ***\x1b[0m\n%s\n\nPress START to exit\n", title, message);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    port_shutdown_and_exit(1);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    char text[256];
    va_list args;
    va_start(args, msg);
    vsnprintf(text, sizeof(text), msg, args);
    va_end(args);

    LOG_ERROR("OSPanic", "%s:%d: %s", file, line, text);
    char screen[320];
    snprintf(screen, sizeof(screen), "%s:%d\n%s", file, line, text);
    fatal_screen("OSPanic", screen);
}

void OSFatal(GXColor fg, GXColor bg, const char* msg) {
    (void)fg;
    (void)bg;
    LOG_ERROR("OSFatal", "%s", msg);
    fatal_screen("OSFatal", msg);
}

// --- System settings -------------------------------------------------------------

static u32 sSoundMode = 1;
static u32 sProgressiveMode;
static u8 sLanguage;
static OSErrorHandler sErrorHandlers[17];

u32 OSGetSoundMode(void) { return sSoundMode; }
void OSSetSoundMode(u32 mode) { sSoundMode = mode; }
u32 OSGetProgressiveMode(void) { return sProgressiveMode; }
void OSSetProgressiveMode(u32 mode) { sProgressiveMode = mode; }
static u8 sLanguageRead;

// The GameCube language comes from the 3DS system setting (read once). It is
// the default for a new save's language (PAL language menu, see
// gameTextApplySaveLanguage). Languages the PAL disc has no text for,
// including Dutch, map to English, so the Dutch-only first-boot and memory
// card paths never run.
u8 OSGetLanguage(void) {
    if (!sLanguageRead) {
        u8 lang = CFG_LANGUAGE_EN;
        sLanguageRead = 1;
        if (R_SUCCEEDED(cfguInit())) {
            CFGU_GetSystemLanguage(&lang);
            cfguExit();
        }
        switch (lang) {
            case CFG_LANGUAGE_DE: sLanguage = OS_LANGUAGE_GERMAN; break;
            case CFG_LANGUAGE_FR: sLanguage = OS_LANGUAGE_FRENCH; break;
            case CFG_LANGUAGE_ES: sLanguage = OS_LANGUAGE_SPANISH; break;
            case CFG_LANGUAGE_IT: sLanguage = OS_LANGUAGE_ITALIAN; break;
            default:              sLanguage = OS_LANGUAGE_ENGLISH; break;
        }
    }
    return sLanguage;
}
void OSSetLanguage(u8 language) { sLanguage = language; sLanguageRead = 1; }
u32 OSGetResetCode(void) { return 0; }
BOOL OSGetResetButtonState(void) { return FALSE; }
void OSSetSaveRegion(void* start, void* end) { (void)start; (void)end; }

OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    OSErrorHandler prev = NULL;
    if (error < 17) {
        prev = sErrorHandlers[error];
        sErrorHandlers[error] = handler;
    }
    return prev;
}

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    LOG_INFO("os", "OSResetSystem(%d, 0x%lX, %d) requested, exiting", reset,
             (unsigned long)resetCode, forceMenu);
    port_shutdown_and_exit(0);
}

void OSInit(void) {}

// --- Init / shutdown -------------------------------------------------------------

void port_os_init(void) {
    sBootSysTick = svcGetSystemTick();
    sBootOSTime = (OSTime)(time(NULL) - GC_EPOCH_UNIX) * OS_TIMER_CLOCK;

    // In linear memory, so the GPU can read game data (textures) in place
    sMem1 = linearMemAlign(PORT_MEM1_SIZE, 0x1000);
    bool linear = sMem1 != NULL;
    if (!sMem1) {
        LOG_WARN("os", "no linear memory for MEM1, using the heap (textures will be copied)");
        sMem1 = memalign(32, PORT_MEM1_SIZE);
    }
    if (!sMem1) {
        LOG_ERROR("os", "failed to allocate %u MB for MEM1", PORT_MEM1_SIZE / (1024 * 1024));
        return;
    }
    LOG_INFO("os", "MEM1 in %s memory", linear ? "linear" : "heap");
    memset(sMem1, 0, PORT_MEM1_SIZE);
    // Like Aurora: arena starts after the low-memory globals area
    sArenaLo = sMem1 + 0x4000;
    sArenaHi = sMem1 + PORT_MEM1_SIZE;
    LOG_INFO("os", "MEM1 arena %p-%p (%u MB)", sArenaLo, sArenaHi, PORT_MEM1_SIZE / (1024 * 1024));
}

void* port_mem1_base(void) { return sMem1; }

void probe_shutdown(void);  // probe_3ds.c
void stereo_auto_shutdown(void);  // stereo_auto_3ds.c

void port_shutdown_and_exit(int code) {
    LOG_INFO("os", "shutting down (code %d)", code);
    stereo_auto_shutdown();  // Restore + save the boot quality settings
    netsrv_shutdown();  // socExit: release the listener for the next boot
    watchdog_stop();
    probe_shutdown();  // S3D_PROBE sampler thread (probe_3ds.c)
    gxrec_shutdown();  // Render thread off before the GPU state goes away
    // An overlapped frame end leaves the frame open (its boundary is recorded
    // after input, where START+SELECT and aptMainLoop exit): citro3d panics
    // when targets are deleted inside a frame
    gx_frame_end();
    gx_deinit();
    padrec_flush();  // Input recording (S3D_PAD_RECORD=1)
    log_shutdown();
    gfxExit();
    exit(code);
}
