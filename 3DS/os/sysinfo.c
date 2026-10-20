// System information written to the log at boot (hardware profiling needs to
// know which console, firmware, memory mode and CPU setup a log came from).

#include "sysinfo.h"
#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include "log.h"

static const char* model_name(u8 model) {
    switch (model) {
        case CFG_MODEL_3DS:    return "Old 3DS";
        case CFG_MODEL_3DSXL:  return "Old 3DS XL";
        case CFG_MODEL_N3DS:   return "New 3DS";
        case CFG_MODEL_2DS:    return "2DS";
        case CFG_MODEL_N3DSXL: return "New 3DS XL";
        case CFG_MODEL_N2DSXL: return "New 2DS XL";
        default:               return "unknown model";
    }
}

void sysinfo_log(void) {
    u8 model = 0xFF;
    if (R_SUCCEEDED(cfguInit())) {
        CFGU_GetSystemModel(&model);
        cfguExit();
    }
    bool n3ds = false;
    APT_CheckNew3DS(&n3ds);
    u32 kver = osGetKernelVersion(), fver = osGetFirmVersion();
    LOG_INFO("sys", "console: %s (%s hardware), kernel %lu.%lu-%lu, firm %lu.%lu-%lu", model_name(model),
             n3ds ? "New 3DS" : "Old 3DS", (unsigned long)GET_VERSION_MAJOR(kver),
             (unsigned long)GET_VERSION_MINOR(kver), (unsigned long)GET_VERSION_REVISION(kver),
             (unsigned long)GET_VERSION_MAJOR(fver), (unsigned long)GET_VERSION_MINOR(fver),
             (unsigned long)GET_VERSION_REVISION(fver));

    // The application memory region's size tells which memory mode the
    // launcher gave us (64/72/80/96 MB on Old 3DS, 124/178 MB on New 3DS)
    u32 app_size = osGetMemRegionSize(MEMREGION_APPLICATION);
    u32 app_used = osGetMemRegionUsed(MEMREGION_APPLICATION);
    u32 app_free = osGetMemRegionFree(MEMREGION_APPLICATION);
    LOG_INFO("sys", "memory mode: application region %lu MB (used %lu KB, free %lu KB), system %lu MB, base %lu MB",
             (unsigned long)(app_size >> 20), (unsigned long)(app_used >> 10), (unsigned long)(app_free >> 10),
             (unsigned long)(osGetMemRegionSize(MEMREGION_SYSTEM) >> 20),
             (unsigned long)(osGetMemRegionSize(MEMREGION_BASE) >> 20));
    if (app_size < 120u << 20) {
        LOG_WARN("sys", "small memory mode (%lu MB): texture and display list caches are reduced. "
                 "Install the CIA, or start the .3dsx through a title that runs in 124/178 MB mode",
                 (unsigned long)(app_size >> 20));
    }

    extern u32 __ctru_heap_size, __ctru_linear_heap_size;
    LOG_INFO("sys", "heap %lu KB, linear heap %lu KB (free %lu KB)", (unsigned long)(__ctru_heap_size >> 10),
             (unsigned long)(__ctru_linear_heap_size >> 10), (unsigned long)(linearSpaceFree() >> 10));
    LOG_INFO("sys", "VRAM free %lu KB", (unsigned long)(vramSpaceFree() >> 10));

    u32 limit = 0;
    if (R_SUCCEEDED(APT_GetAppCpuTimeLimit(&limit)))
        LOG_INFO("sys", "syscore (core 1) time limit for the app: %lu%%", (unsigned long)limit);

    u8 battery = 0;
    bool ptm = R_SUCCEEDED(ptmuInit());
    if (ptm) {
        PTMU_GetBatteryLevel(&battery);
        ptmuExit();
    }
    LOG_INFO("sys", "3D slider %.2f, battery level %u/5", osGet3DSliderState(), battery);
}
