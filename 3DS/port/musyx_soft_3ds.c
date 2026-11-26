// MusyX output through MusyX's own software renderer (foxhollow's hw_pc.c,
// hw_pc_output.c, hw_pc_rate.c): the default build (MUSYX_MIX=soft, which
// defines PORT_MUSYX_SOFT in the generated musyx_mix.h). MUSYX_MIX=dsp builds
// the older path instead, one ndsp channel per voice (port/musyx_ndsp.c).
//
// Why: playing each voice on its own ndsp channel loses what the GameCube DSP
// does per sample (volume and envelope ramps inside each 5 ms cycle, fade-out
// tails for stopped or stolen voices, the aux effects such as reverb) and
// limits the game to 21 voices instead of 48. It crackled on hardware; the
// software renderer is foxhollow's PC mixer, which sounds like Dolphin.
//
// A mixer thread renders MIX_FRAMES stereo frames at 32 kHz (four 5 ms MusyX
// cycles) per ndsp buffer and keeps sQueue of them queued on one stereo
// channel. Rendering also runs the MusyX ticks, so music timing follows the
// audio output, as on PC. 5 ms buffers made the emulator (Azahar) fall behind
// and stretch the audio; 20 ms ones fixed that.
//
// Cost on a New 3DS (2026-11-26, 10 min of play, up to 24 voices): 6.8 ms per
// 20 ms buffer on average, 13.9 ms in the busiest 5 s, no underruns. That
// needs patches-musyx/runtime/hw_pc.c.sed: the volume ramps did two integer
// divisions per sample and channel, and the ARM11 has no hardware divide
// (~2x slower, and the game thread waited on the MusyX lock meanwhile).
//
// The thread runs on core 3 when the system grants it, else core 2, else the
// game core (above the game thread). It is woken by the ndsp frame callback.
// The game thread's snd* calls and the renderer share one recursive lock
// (sndPCSetSynchronization, i.e. hwDisableIrq/hwEnableIrq), held while a
// buffer renders.
//
// Live settings (cmd.txt "set NAME VALUE"):
//   S3D_MUSYX_QUEUE  buffers kept queued, 2-7 (default 3 = 60 ms)
//   S3D_MUSYX_STATS  1: a stats line every 5 s (render time, underruns, voices)

#include "musyx_mix.h"
#ifdef PORT_MUSYX_SOFT

#include <3ds.h>
#include <string.h>

#include "musyx/hardware.h"
#include "musyx/musyx.h"
#include "musyx/pc.h"
#include "musyx/sal.h"
#include "musyx/synth.h"

#include "config.h"
#include "crash.h"
#include "log.h"
#include "port_3ds.h"
#include "profiler.h"

#define MIX_RATE     32000
#define MIX_FRAMES   640  // Four MusyX cycles (20 ms) per ndsp buffer
#define MIX_BUFS     8
#define MIX_CHANNEL  0    // ndsp channels 0-20 were the per-voice channels
#define MAX_CATCH_UP 4    // Silent mode: buffers rendered per game frame at most

static RecursiveLock sLock;
static bool sNdsp, sStarted;
static Thread sThread;
static int sThreadCore = -1;
static LightEvent sEvent;
static ndspWaveBuf sWb[MIX_BUFS];
static s16* sData;  // MIX_BUFS buffers of linear memory
static u32 sNext;
static bool sPrimed;
static int sQueue = 3, sStats;
static u64 sSilentStart, sSilentCycles;
static s16 sScratch[MIX_FRAMES * 2];

static struct {
    u32 cycles, underruns;
    u64 renderTicks, maxTicks;
} sSt;

static void lock_enter(void) { RecursiveLock_Lock(&sLock); }
static void lock_leave(void) { RecursiveLock_Unlock(&sLock); }

// Before sndInit (fhMusyxConfigure)
void port_musyx_configure(void) {
    static bool done;
    if (done) return;
    done = true;
    RecursiveLock_Init(&sLock);
    if (!sndPCSetSynchronization(lock_enter, lock_leave))
        LOG_ERROR("musyx", "cannot set the renderer lock (MusyX already active?)");
}

static u32 active_voices(void) {
    u32 n = 0;
    for (u32 i = 0; dspVoice && i < salNumVoices; ++i) n += dspVoice[i].state != DSP_VOICE_STATE_INACTIVE;
    return n;
}

static void stats_cycle(u64 ticks) {
    sSt.cycles++;
    sSt.renderTicks += ticks;
    if (ticks > sSt.maxTicks) sSt.maxTicks = ticks;
    if (sStats && sSt.cycles >= 5 * MIX_RATE / MIX_FRAMES) {  // 5 s
        LOG_WARN("musyx", "mixer 5s: %lu us avg, %lu us max per buffer, %lu underruns, %lu voices, core %d",
                 (unsigned long)(sSt.renderTicks * 1000000 / SYSCLOCK_ARM11 / sSt.cycles),
                 (unsigned long)(sSt.maxTicks * 1000000 / SYSCLOCK_ARM11), (unsigned long)sSt.underruns,
                 (unsigned long)active_voices(), sThreadCore);
        memset(&sSt, 0, sizeof(sSt));
    }
}

// Render cycles into free buffers until sQueue are queued (lock not needed:
// sndPCRender takes it)
static void fill(void) {
    CFG_LIVE(sQueue, cfg_int("S3D_MUSYX_QUEUE", 3));
    CFG_LIVE(sStats, cfg_int("S3D_MUSYX_STATS", 0));
    int target = sQueue < 2 ? 2 : sQueue > MIX_BUFS - 1 ? MIX_BUFS - 1 : sQueue;
    int queued = 0;
    for (int i = 0; i < MIX_BUFS; i++) {
        u8 st = sWb[i].status;
        queued += st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING;
    }
    if (sPrimed && queued == 0) sSt.underruns++;
    while (queued < target) {
        ndspWaveBuf* wb = &sWb[sNext];
        if (wb->status == NDSP_WBUF_QUEUED || wb->status == NDSP_WBUF_PLAYING) break;
        s16* buf = sData + sNext * MIX_FRAMES * 2;
        u64 start = svcGetSystemTick();
        if (!sndPCRender(buf, MIX_FRAMES)) memset(buf, 0, MIX_FRAMES * 4);
        stats_cycle(svcGetSystemTick() - start);
        DSP_FlushDataCache(buf, MIX_FRAMES * 4);
        memset(wb, 0, sizeof(*wb));
        wb->data_vaddr = buf;
        wb->nsamples = MIX_FRAMES;
        ndspChnWaveBufAdd(MIX_CHANNEL, wb);
        sNext = (sNext + 1) % MIX_BUFS;
        queued++;
        sPrimed = true;
    }
}

static void mixer_thread(void* arg) {
    (void)arg;
    crash_install_handler();
    for (;;) {
        LightEvent_Wait(&sEvent);
        fill();
    }
}

// After sndInit (fhMusyxStartOutput)
void port_musyx_start_output(void) {
    if (sStarted) return;
    sStarted = true;
    sSilentStart = svcGetSystemTick();
    sSilentCycles = 0;
    sNdsp = port_audio_ndsp_ready();
    if (sNdsp) {
        sData = linearAlloc(MIX_BUFS * MIX_FRAMES * 4);
        if (!sData) {
            LOG_ERROR("musyx", "no linear memory for the mixer buffers: silent");
            sNdsp = false;
        }
    }
    if (!sNdsp) {
        LOG_INFO("musyx", "software mixer, no DSP: silent, real-time cycles");
        return;
    }
    ndspChnReset(MIX_CHANNEL);
    ndspChnSetFormat(MIX_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    ndspChnSetRate(MIX_CHANNEL, MIX_RATE);
    ndspChnSetInterp(MIX_CHANNEL, NDSP_INTERP_POLYPHASE);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(MIX_CHANNEL, mix);
    memset(sWb, 0, sizeof(sWb));

    LightEvent_Init(&sEvent, RESET_ONESHOT);
    // Above the game thread and the other helpers: a late buffer is a gap
    static const int cores[] = { 3, 2, -2 };
    for (unsigned i = 0; i < sizeof(cores) / sizeof(cores[0]) && !sThread; i++) {
        if (cores[i] == 2 && !cfg_bool("S3D_USE_CORE2", true)) continue;
        sThread = threadCreate(mixer_thread, NULL, 64 * 1024, 0x18, cores[i], true);
        if (sThread) sThreadCore = cores[i] < 0 ? 0 : cores[i];
    }
    if (!sThread) LOG_WARN("musyx", "no mixer thread: mixing in the ndsp callback");
    LOG_INFO("musyx", "software mixer: %d voices, %d Hz, thread on core %d", salNumVoices, MIX_RATE, sThreadCore);
    LightEvent_Signal(&sEvent);
}

// ndsp frame callback (ndsp thread)
void port_musyx_frame(void) {
    if (!sStarted || !sNdsp || !sndActive) return;
    if (sThread) {
        LightEvent_Signal(&sEvent);
    } else if (RecursiveLock_TryLock(&sLock) == 0) {  // Busy: caught up at the next frame
        fill();
        RecursiveLock_Unlock(&sLock);
    }
}

// Without the DSP: render (and so tick) in real time, from the game thread
void port_musyx_pump_silent(void) {
    if (!sStarted || sNdsp || !sndActive) return;
    PROF_ZONE("audio.musyx_ticks");
    u64 due = (svcGetSystemTick() - sSilentStart) * (MIX_RATE / MIX_FRAMES) / SYSCLOCK_ARM11;
    for (u32 n = 0; sSilentCycles < due && n < MAX_CATCH_UP; n++, sSilentCycles++)
        sndPCRender(sScratch, MIX_FRAMES);
    if (due - sSilentCycles > MAX_CATCH_UP) sSilentCycles = due - MAX_CATCH_UP;
}

void port_musyx_log_stats(void) {
    LOG_DEBUG("musyx", "%lu voices active, software mixer on core %d", (unsigned long)active_voices(), sThreadCore);
}

// After game memory was replaced (MusyX lock held). The renderer's decoder
// state (hw_pc.c) is not in game memory, so restored voices continue from the
// renderer's current positions; a short glitch at most.
void port_musyx_after_restore(void) {}

#endif  // PORT_MUSYX_SOFT
