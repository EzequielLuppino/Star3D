#include "crash.h"
#include <3ds.h>
#include <stdio.h>
#include "log.h"
#include "profiler.h"

extern volatile const char* g_last_stub;

// One handler stack shared by all threads: two threads faulting at the same
// time is not a case worth handling
static u8 s_crash_stack[0x4000] __attribute__((aligned(8)));

static const char* exception_name(ERRF_ExceptionType type) {
    switch (type) {
        case ERRF_EXCEPTION_PREFETCH_ABORT: return "prefetch abort (jump to a bad address)";
        case ERRF_EXCEPTION_DATA_ABORT:     return "data abort (bad memory access)";
        case ERRF_EXCEPTION_UNDEFINED:      return "undefined instruction";
        case ERRF_EXCEPTION_VFP:            return "VFP (floating point) exception";
        default:                            return "unknown exception";
    }
}

// The stack is read word by word only inside the main heap/stack ranges, so
// a corrupted sp does not fault again inside the handler
static bool readable(u32 addr) {
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return false;
    return (mi.perm & MEMPERM_READ) && mi.state != MEMSTATE_FREE;
}

static void __attribute__((noreturn)) crash_handler(ERRF_ExceptionInfo* excep, CpuRegisters* regs) {
    char line[200];
    snprintf(line, sizeof(line), "[CRASH] *** %s at frame %u ***", exception_name(excep->type), prof_frame_count());
    log_write_unlocked(line);
    snprintf(line, sizeof(line), "[CRASH] pc %08lX lr %08lX sp %08lX cpsr %08lX fsr %08lX far %08lX fpexc %08lX",
             (unsigned long)regs->pc, (unsigned long)regs->lr, (unsigned long)regs->sp, (unsigned long)regs->cpsr,
             (unsigned long)excep->fsr, (unsigned long)excep->far, (unsigned long)excep->fpexc);
    log_write_unlocked(line);
    for (int i = 0; i < 13; i += 4) {
        int n = snprintf(line, sizeof(line), "[CRASH]");
        for (int j = i; j < i + 4 && j < 13; j++)
            n += snprintf(line + n, sizeof(line) - n, " r%-2d %08lX", j, (unsigned long)regs->r[j]);
        log_write_unlocked(line);
    }
    snprintf(line, sizeof(line), "[CRASH] last stub called: %s", (const char*)g_last_stub);
    log_write_unlocked(line);

    // Stack words: return addresses into .text show the call chain
    u32 sp = regs->sp & ~3u;
    for (int row = 0; row < 16; row++) {
        u32 addr = sp + row * 16;
        if (!readable(addr) || !readable(addr + 12)) break;
        const u32* w = (const u32*)addr;
        snprintf(line, sizeof(line), "[CRASH] stack %08lX: %08lX %08lX %08lX %08lX", (unsigned long)addr,
                 (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3]);
        log_write_unlocked(line);
    }
    log_write_unlocked("[CRASH] resolve with: arm-none-eabi-addr2line -f -e star3d.elf <pc> <lr> <stack words>");
    log_flush_unlocked();

    printf("\n\x1b[31m*** CRASH: %s ***\x1b[0m\npc %08lX lr %08lX far %08lX\n"
           "Details: sdmc:/3ds/star3d/Star3D.log\nPress START to exit\n",
           exception_name(excep->type), (unsigned long)regs->pc, (unsigned long)regs->lr, (unsigned long)excep->far);
    for (int i = 0; i < 60 * 60 * 5; i++) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    svcExitProcess();
    for (;;) {}
}

// libctru panics (gxCmdQueueAdd on a full queue, ...) call svcBreak, which
// skips the exception handler: on hardware the process just stopped and the
// log ended with no [CRASH] block (23rd and 24th logs). Linked with
// --wrap=svcBreak, so calls from libctru and citro3d land here first
extern void __real_svcBreak(UserBreakType reason);
void __wrap_svcBreak(UserBreakType reason) {
    char line[160];
    snprintf(line, sizeof(line), "[CRASH] *** svcBreak(%d) at frame %u, called from lr %08lX ***", (int)reason,
             prof_frame_count(), (unsigned long)(uintptr_t)__builtin_return_address(0));
    log_write_unlocked(line);
    log_write_unlocked("[CRASH] resolve with: arm-none-eabi-addr2line -f -e star3d.elf <lr>");
    log_flush_unlocked();
    __real_svcBreak(reason);
}

void crash_install_handler(void) {
    threadOnException(crash_handler, s_crash_stack + sizeof(s_crash_stack), WRITE_DATA_TO_HANDLER_STACK);
}
