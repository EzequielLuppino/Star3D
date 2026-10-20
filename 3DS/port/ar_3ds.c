// ARAM (auxiliary RAM) emulation, a C port of Aurora's lib/dolphin/AR.cpp.
// ARAM "addresses" are offsets into a host buffer; DMA requests are memcpy.
// The game uses ARAM only for MusyX audio sample/song data.

#include <dolphin/ar.h>
#include <dolphin/arq.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "port_3ds.h"

#define ARAM_STACK_START 0x4000

static u8* sAramBuffer;
static u32 sAramSize;
static u32 AR_StackPointer;
static u32* AR_BlockLength;
static u32 AR_FreeBlocks;
static BOOL AR_init_flag;

static u32 sAramHighWater;
u32 port_aram_high_water(void) { return sAramHighWater; }
// Bytes of ARAM a save state has to keep
u32 port_aram_used(void) {
    u32 used = sAramHighWater > AR_StackPointer ? sAramHighWater : AR_StackPointer;
    return used < sAramSize ? used : sAramSize;
}

static u8* aram_to_host(uintptr_t aramAddr, u32 length) {
    if (aramAddr + length > sAramHighWater && length < sAramSize / 2) sAramHighWater = aramAddr + length;
    if (!sAramBuffer || aramAddr >= sAramSize || length > sAramSize - aramAddr) {
        return NULL;
    }
    return sAramBuffer + aramAddr;
}

u32 ARAlloc(u32 length) {
    if (length > sAramSize - AR_StackPointer || AR_FreeBlocks == 0) {
        LOG_ERROR("ar", "ARAlloc(0x%lX): out of ARAM (sp 0x%lX, free blocks %lu)",
                  (unsigned long)length, (unsigned long)AR_StackPointer, (unsigned long)AR_FreeBlocks);
    }

    u32 addr = AR_StackPointer;
    AR_StackPointer += length;
    *AR_BlockLength = length;
    AR_BlockLength += 1;
    AR_FreeBlocks -= 1;
    return addr;
}

u32 ARFree(u32* length) {
    AR_BlockLength -= 1;
    if (length) {
        *length = *AR_BlockLength;
    }
    AR_StackPointer -= *AR_BlockLength;
    AR_FreeBlocks += 1;
    return AR_StackPointer;
}

BOOL ARCheckInit(void) { return AR_init_flag; }

u32 ARInit(u32* stack_index_addr, u32 num_entries) {
    if (AR_init_flag) {
        return ARAM_STACK_START;
    }

    sAramSize = PORT_ARAM_SIZE;
    sAramBuffer = calloc(1, sAramSize);
    if (sAramBuffer) {
        LOG_INFO("ar", "ARAM: 0x%lX bytes at %p", (unsigned long)sAramSize, sAramBuffer);
    } else {
        LOG_ERROR("ar", "failed to allocate 0x%lX bytes of ARAM", (unsigned long)sAramSize);
        sAramSize = 0;
    }

    AR_StackPointer = ARAM_STACK_START;
    AR_FreeBlocks = num_entries;
    AR_BlockLength = stack_index_addr;
    AR_init_flag = TRUE;
    return AR_StackPointer;
}

u32 ARGetSize(void) { return sAramSize; }

void* ARGetStorageAddress(void) { return sAramBuffer; }

void ARQInit(void) {}

void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, uintptr_t source,
                    uintptr_t dest, u32 length, ARQCallback callback) {
    (void)owner;
    (void)priority;

    if (type == ARAM_DIR_MRAM_TO_ARAM) {
        u8* dst = aram_to_host(dest, length);
        if (dst && source) {
            memcpy(dst, (const void*)source, length);
        } else {
            LOG_WARN("ar", "bad MRAM->ARAM request: src %p dst 0x%lX len 0x%lX",
                     (void*)source, (unsigned long)dest, (unsigned long)length);
        }
    } else {
        u8* src = aram_to_host(source, length);
        if (src && dest) {
            memcpy((void*)dest, src, length);
        } else {
            LOG_WARN("ar", "bad ARAM->MRAM request: src 0x%lX dst %p len 0x%lX",
                     (unsigned long)source, (void*)dest, (unsigned long)length);
        }
    }

    // Synchronous transfer: complete immediately
    if (callback) {
        callback((uintptr_t)request);
    }
}
