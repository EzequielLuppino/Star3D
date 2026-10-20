// MusyX hardware layer for the 3DS DSP, replacing foxhollow's software
// renderer (hw_pc.c, hw_pc_output.c, hw_pc_rate.c and the PC part of
// hw_aramdma.c).
//
// On the GameCube, MusyX's synthesizer runs every 5 ms and hands per-voice
// parameters (sample, pitch, volumes, envelope) to the DSP, which decodes
// DSP-ADPCM and mixes. The 3DS DSP decodes the same ADPCM format, so each
// MusyX voice plays on its own ndsp channel with no CPU mixing: a voice is one
// buffer up to the loop start plus one looping buffer (the build aligns loop
// starts to ADPCM frames, see tools/audio_convert.py). The ADSR envelope is
// still computed here, once per millisecond as on the GameCube, and applied
// with the voice volumes as the channel's mix level every 5 ms tick.
//
// Ticks are paced by the ndsp frame callback (port_musyx_frame, ~4.9 ms). They
// run on a thread on core 1 (MULTICORE_PLAN.md step 1: ~175 us per tick, about
// 3.5% of a core, that used to preempt the game thread on core 0), which the
// callback wakes; the voice changes then reach the DSP with the next ndsp
// frame. The game thread's snd* calls take the same recursive lock
// (hwDisableIrq/hwEnableIrq), and the tick thread waits for it.
// S3D_AUDIO_CORE1=0, or no core 1, runs the ticks in the callback as before:
// there the lock is only tried, and a busy lock retries at the next frame
// instead of blocking the ndsp thread. Without the DSP firmware, ticks run in
// real time from fhAIPump and voices end on time.
//
// Not implemented: auxiliary effects (the game's reverb; libctru has no aux
// bus callback) and ITD (interaural delay).

#include <3ds.h>
#include <string.h>

#include "musyx/adsr.h"
#include "musyx/debugger.h"
#include "musyx/hardware.h"
#include "musyx/musyx.h"
#include "musyx/pc.h"
#include "musyx/sal.h"
#include "musyx/synth.h"

#include "log.h"
#include "port_3ds.h"
#include "profiler.h"
#include "config.h"
#include "crash.h"
#include "worker.h"

#define DSP_RATE      32000  // MusyX pitch 0x10000 plays a sample at this rate
#define NDSP_FRAME_HZ 32728  // ndsp frames are 160 samples at the DSP's ~32728 Hz
#define MAX_CATCH_UP  4      // Ticks run per frame at most after the lock was busy

extern u32 last_rnd;

typedef struct {
    ndspWaveBuf wb[2];         // Up to the loop start, then the looping part
    ndspAdpcmData adpcm[2];
    bool playing;
    bool looping;
    u32 rate;                  // Last values sent to ndsp
    u16 interp;
    float mix[2];
    u64 silentPos;             // No-DSP mode: simulated position (16.16)
} HwVoice;

static HwVoice sHw[PORT_NDSP_MUSYX_CHANNELS];
static bool sNdsp;
static SND_SOME_CALLBACK sTick;
static volatile bool sTicking;
static u64 sFrames, sTicksDone;
static u64 sSilentStart;
static u64 sTickTime;  // CPU ticks spent in MusyX ticks, for the stats line
static Thread sTickThread;   // Core 1 tick thread, NULL = ticks in the ndsp callback
static LightEvent sTickEvent;

// --- Locking (hwDisableIrq/hwEnableIrq in hw_pc_null.c) -------------------------

static RecursiveLock sLock;
static bool sLockReady;

static void lock_init(void) {
    if (!sLockReady) {
        RecursiveLock_Init(&sLock);
        sLockReady = true;
    }
}

bool salPCExternalEnter(void) {
    lock_init();
    RecursiveLock_Lock(&sLock);
    return true;
}

bool salPCExternalLeave(void) {
    RecursiveLock_Unlock(&sLock);
    return true;
}

bool sndPCSetSynchronization(void (*enter)(void), void (*leave)(void)) {
    (void)enter;
    (void)leave;
    return false;
}

// --- Output configuration (the PC renderer's API, kept for the game's shims) ------

bool sndPCConfigure(const SND_PC_CONFIG* config) { return config && config->channels == 2; }
u32 salPCMixRate(void) { return DSP_RATE; }
u32 salPCChannels(void) { return 2; }
void sndPCSetOutputCallback(SND_PC_OUTPUT_CALLBACK callback) { (void)callback; }
bool sndPCRender(s16* output, size_t frames) {
    (void)output;
    (void)frames;
    return false;
}
SND_PC_RENDER_INFO sndPCGetRenderInfo(void) {
    SND_PC_RENDER_INFO info = {0};
    info.controlTicks = sTicksDone * 5;
    info.mixRate = DSP_RATE;
    info.channels = 2;
    return info;
}

void* salAiGetDest(void) { return NULL; }

bool salInitAi(SND_SOME_CALLBACK callback, u32 flags, u32* outFreq) {
    (void)flags;
    last_rnd = 1;
    sTick = callback;
    synthInfo.numSamples = 32;
    *outFreq = DSP_RATE;
    return true;
}

static void tick_thread(void* arg);

bool salStartAi(void) {
    sNdsp = port_audio_ndsp_ready();
    sFrames = sTicksDone = 0;
    sSilentStart = svcGetSystemTick();
    if (sNdsp && !sTickThread && cfg_bool("S3D_AUDIO_CORE1", true) && core1_init()) {
        lock_init();
        LightEvent_Init(&sTickEvent, RESET_ONESHOT);
        // Above the other core 1 helpers: a late tick is audible
        sTickThread = threadCreate(tick_thread, NULL, 16 * 1024, 0x18, 1, true);
        if (!sTickThread) LOG_WARN("musyx", "no tick thread on core 1: ticks run in the ndsp callback");
    }
    sTicking = true;
    LOG_INFO("musyx", "%d voices on ndsp channels 0-%d%s%s", salNumVoices, salNumVoices - 1,
             sNdsp ? "" : " (no DSP: silent, real-time ticks)", sTickThread ? ", ticks on core 1" : "");
    return true;
}

bool salExitAi(void) {
    sTicking = false;
    return true;
}

bool salInitDsp(u32 flags) {
    (void)flags;
    return true;
}
bool salExitDsp(void) { return true; }
void salStartDsp(u16* cmdList) { (void)cmdList; }
void salInitHRTFBuffer(void) {}

bool salInitDspCtrl(u8 voices, u8 studios, u32 dpl2) {
    if (voices > PORT_NDSP_MUSYX_CHANNELS) {
        LOG_ERROR("musyx", "%d voices requested, only %d DSP channels", voices, PORT_NDSP_MUSYX_CHANNELS);
        return false;
    }
    salNumVoices = voices;
    salMaxStudioNum = studios;
    memset(dspStudio, 0, sizeof(dspStudio));
    dspVoice = salMalloc(voices * sizeof(*dspVoice));
    if (!dspVoice) return false;
    memset(dspVoice, 0, voices * sizeof(*dspVoice));
    memset(sHw, 0, sizeof(sHw));
    for (u32 i = 0; i < voices; ++i) {
        dspVoice[i].virtualSampleID = 0xffffffff;
        memset(&dspVoice[i].lastUpdate, 0xff, sizeof(dspVoice[i].lastUpdate));
    }
    salActivateStudio(0, 1, dpl2 ? SND_STUDIO_TYPE_DPL2 : SND_STUDIO_TYPE_STD);
    return true;
}

bool salExitDspCtrl(void) {
    for (u32 i = 0; i < salNumVoices; ++i)
        if (sNdsp) ndspChnWaveBufClear(i);
    memset(dspStudio, 0, sizeof(dspStudio));
    if (dspVoice) salFree(dspVoice);
    dspVoice = NULL;
    return true;
}

void salActivateStudio(u8 studio, u32 isMaster, SND_STUDIO_TYPE type) {
    DSPstudioinfo* state = &dspStudio[studio];
    state->voiceRoot = NULL;
    state->alienVoiceRoot = NULL;
    state->state = DSP_STUDIO_STATE_ACTIVE;
    state->isMaster = isMaster;
    state->numInputs = 0;
    state->type = type;
    state->auxAHandler = state->auxBHandler = NULL;
    state->auxAUser = state->auxBUser = NULL;
}

// --- Voices on ndsp channels ------------------------------------------------------

static void hw_stop(u32 idx) {
    HwVoice* hw = &sHw[idx];
    if (hw->playing && sNdsp) ndspChnWaveBufClear(idx);
    hw->playing = false;
}

void salDeactivateVoice(DSPvoice* voice) {
    if (voice->state == DSP_VOICE_STATE_INACTIVE) return;
    hw_stop(voice - dspVoice);
    if (voice->prev)
        voice->prev->next = voice->next;
    else
        dspStudio[voice->studio].voiceRoot = voice->next;
    if (voice->next) voice->next->prev = voice->prev;
    voice->state = DSP_VOICE_STATE_INACTIVE;
    if (voice->virtualSampleID != UINT32_MAX) {
        salSynthSendMessage(voice, HW_MESSAGE_VIRTUAL_SAMPLE_END);
        voice->virtualSampleID = UINT32_MAX;
    }
    voice->vSampleInfo.inLoopBuffer = 0;
}

void salReconnectVoice(DSPvoice* voice, u8 studio) {
    if (voice->studio == studio) return;
    if (voice->state != DSP_VOICE_STATE_INACTIVE) {
        if (voice->prev)
            voice->prev->next = voice->next;
        else
            dspStudio[voice->studio].voiceRoot = voice->next;
        if (voice->next) voice->next->prev = voice->prev;
        voice->next = dspStudio[studio].voiceRoot;
        if (voice->next) voice->next->prev = voice;
        voice->prev = NULL;
        dspStudio[studio].voiceRoot = voice;
    }
    voice->studio = studio;
}

static u32 sample_bytes(u8 type, u32 samples) {
    switch (type) {
    case SAMPLE_TYPE_PCM16: return samples * 2;
    case SAMPLE_TYPE_PCM8: return samples;
    default: return samples / 14 * 8;  // Frame-aligned positions only
    }
}

// Start playing a voice's sample. Returns false if it cannot be played.
static bool hw_start(u32 idx, DSPvoice* vp) {
    HwVoice* hw = &sHw[idx];
    const SAMPLE_INFO* s = &vp->smp_info;
    u8 type = s->compType;
    if (!s->addr || !s->length || (type != SAMPLE_TYPE_ADPCM && type != SAMPLE_TYPE_PCM16 &&
                                   type != SAMPLE_TYPE_PCM8)) {
        LOG_ONCE(LOG_LEVEL_WARN, "musyx", "sample type %d not supported on the DSP", type);
        return false;
    }
    bool loop = s->loopLength != 0 && s->loop + s->loopLength <= s->length;
    u32 loopStart = loop ? s->loop : 0;
    if (loop && type == SAMPLE_TYPE_ADPCM && loopStart % 14) {
        LOG_ONCE(LOG_LEVEL_WARN, "musyx", "sample %d: loop at %lu is not frame aligned (not converted?)",
                 vp->smp_id, (unsigned long)loopStart);
        loopStart -= loopStart % 14;
    }

    hw->playing = true;
    hw->looping = loop;
    hw->rate = 0;
    hw->interp = 0xffff;
    hw->mix[0] = hw->mix[1] = -1.0f;
    hw->silentPos = 0;
    if (!sNdsp) return true;

    ndspChnWaveBufClear(idx);
    const DSPADPCMplusInfo* adpcm = s->extraData;
    if (type == SAMPLE_TYPE_ADPCM) {
        ndspChnSetFormat(idx, NDSP_FORMAT_MONO_ADPCM);
        if (adpcm) ndspChnSetAdpcmCoefs(idx, (u16*)adpcm->coefTab);
    } else {
        ndspChnSetFormat(idx, type == SAMPLE_TYPE_PCM16 ? NDSP_FORMAT_MONO_PCM16 : NDSP_FORMAT_MONO_PCM8);
    }
    ndspChnIirMonoSetEnable(idx, false);
    float mute[12] = {0};
    ndspChnSetMix(idx, mute);

    memset(hw->wb, 0, sizeof(hw->wb));
    hw->adpcm[0] = (ndspAdpcmData){adpcm ? adpcm->initialPS : ((u8*)s->addr)[0], 0, 0};
    hw->adpcm[1] = (ndspAdpcmData){adpcm ? adpcm->loopPS : 0, adpcm ? adpcm->loopY1 : 0,
                                   adpcm ? adpcm->loopY0 : 0};
    u32 head = loop ? loopStart : s->length;
    if (head) {
        hw->wb[0].data_vaddr = s->addr;
        hw->wb[0].nsamples = head;
        hw->wb[0].adpcm_data = type == SAMPLE_TYPE_ADPCM ? &hw->adpcm[0] : NULL;
        ndspChnWaveBufAdd(idx, &hw->wb[0]);
    }
    if (loop) {
        hw->wb[1].data_vaddr = (const u8*)s->addr + sample_bytes(type, loopStart);
        hw->wb[1].nsamples = s->loop + s->loopLength - loopStart;
        hw->wb[1].looping = true;
        hw->wb[1].adpcm_data = type == SAMPLE_TYPE_ADPCM ? &hw->adpcm[1] : NULL;
        ndspChnWaveBufAdd(idx, &hw->wb[1]);
    }
    return true;
}

// True once a non-looping sample has played to its end
static bool hw_finished(u32 idx, DSPvoice* vp) {
    HwVoice* hw = &sHw[idx];
    if (hw->looping) return false;
    if (sNdsp) return hw->wb[0].status == NDSP_WBUF_DONE;
    return (hw->silentPos >> 16) >= vp->smp_info.length;
}

static float gain(u16 vol, u32 env) { return (float)((u32)vol * env) * (1.0f / (32767.0f * 32767.0f)); }

// Send the voice's current pitch and volume to its channel
static void hw_update(u32 idx, DSPvoice* vp, u32 env) {
    HwVoice* hw = &sHw[idx];
    u32 pitch = vp->playInfo.pitch ? vp->playInfo.pitch : vp->pitch[0];
    u32 rate = vp->srcTypeSelect == SAL_SRC_NONE ? DSP_RATE : (u32)(((u64)pitch * DSP_RATE) >> 16);

    if (!sNdsp) {
        hw->silentPos += (u64)rate * 65536 / 200;  // One 5 ms tick
        return;
    }

    if (rate != hw->rate) {
        hw->rate = rate;
        ndspChnSetRate(idx, (float)rate);
        hw->interp = 0xffff;  // The polyphase variant depends on the rate
    }
    u16 interp = vp->srcTypeSelect == SAL_SRC_NONE     ? NDSP_INTERP_NONE
                 : vp->srcTypeSelect == SAL_SRC_LINEAR ? NDSP_INTERP_LINEAR
                                                       : NDSP_INTERP_POLYPHASE;
    if (interp != hw->interp) {
        hw->interp = interp;
        ndspChnSetInterp(idx, interp);
    }

    // Surround goes to both sides at -3 dB, as in foxhollow's stereo downmix
    float s = gain(vp->volS, env) * 0.70710678f;
    float l = gain(vp->volL, env) + s;
    float r = gain(vp->volR, env) + s;
    if (l != hw->mix[0] || r != hw->mix[1]) {
        hw->mix[0] = l;
        hw->mix[1] = r;
        float mix[12] = {l, r};
        ndspChnSetMix(idx, mix);
    }

    u32 pos = ndspChnGetSamplePos(idx);
    if (hw->looping && ndspChnGetWaveBufSeq(idx) == hw->wb[1].sequence_id)
        pos += vp->smp_info.loop - vp->smp_info.loop % 14;
    vp->playInfo.posHi = pos;
}

static void end_voice(DSPvoice* vp) {
    salSynthSendMessage(vp, HW_MESSAGE_SAMPLE_END);
    salDeactivateVoice(vp);
}

// One 5 ms cycle, called by snd_handle_irq before the synthesizer runs
void salCtrlDsp(s16* dest) {
    (void)dest;
    for (u8 st = 0; st < salMaxStudioNum; ++st) {
        DSPstudioinfo* stp = &dspStudio[st];
        if (stp->state != DSP_STUDIO_STATE_ACTIVE) continue;
        DSPvoice* next;
        for (DSPvoice* vp = stp->voiceRoot; vp; vp = next) {
            next = vp->next;  // vp may be deactivated
            if (vp->state == DSP_VOICE_STATE_INACTIVE) continue;
            u32 idx = vp - dspVoice;
            u8 first = 0;

            // A break ends the previous note; one carried into a new start does not
            if (vp->postBreak || (vp->changed[0] & 0x20)) {
                if (vp->state != DSP_VOICE_STATE_STARTING || vp->startupBreak) {
                    salDeactivateVoice(vp);
                    vp->startupBreak = 0;
                    continue;
                }
                vp->changed[0] &= ~0x20u;
            }

            if (vp->state == DSP_VOICE_STATE_STARTING) {
                if (adsrSetup(&vp->adsr) != 0 || !hw_start(idx, vp)) {
                    end_voice(vp);
                    continue;
                }
                vp->playInfo.posHi = vp->playInfo.posLo = 0;
                vp->playInfo.pitch = 0;
                vp->state = DSP_VOICE_STATE_PLAYING;
                first = vp->singleOffset;
            }

            bool ended = false;
            for (u8 sub = first; sub < 5 && !ended; ++sub) {
                u32 changed = vp->changed[sub];
                if (changed & 0x20) {
                    vp->postBreak = 1;
                    salDeactivateVoice(vp);
                    ended = true;
                    break;
                }
                if (!vp->postBreak) {
                    if (changed & 0x40) adsrRelease(&vp->adsr);
                    if (changed & 8) vp->playInfo.pitch = vp->pitch[sub];
                }
                u16 start, delta;
                if (((changed & 0x10) && adsrSetup(&vp->adsr) != 0) ||
                    adsrHandle(&vp->adsr, &start, &delta)) {
                    end_voice(vp);
                    ended = true;
                }
            }
            if (ended) continue;
            if (hw_finished(idx, vp)) {
                end_voice(vp);
                continue;
            }
            s32 env = vp->adsr.currentVolume >> 16;
            hw_update(idx, vp, env < 0 ? 0 : env > 0x7fff ? 0x7fff : env);
        }
    }
}

// --- Tick scheduling ----------------------------------------------------------------

static void run_ticks(u64 due) {
    u32 n = 0;
    u64 start = svcGetSystemTick();
    while (sTicksDone < due && n++ < MAX_CATCH_UP) {
        sTick();
        sTicksDone++;
    }
    sTickTime += svcGetSystemTick() - start;
    if (sTicksDone < due && due - sTicksDone > MAX_CATCH_UP) sTicksDone = due - MAX_CATCH_UP;
}

// ndsp frame callback (ndsp thread), before channel updates go to the DSP
void port_musyx_frame(void) {
    if (!sTicking || !sTick || !sndActive) return;
    sFrames++;
    if (sTickThread) {
        LightEvent_Signal(&sTickEvent);
        return;
    }
    if (RecursiveLock_TryLock(&sLock) != 0) return;  // Caught up at the next frame
    run_ticks(sFrames * 160 * 200 / NDSP_FRAME_HZ);
    RecursiveLock_Unlock(&sLock);
}

// Core 1: the ticks the ndsp frames have made due. Waiting for the lock is
// fine here (the game thread never waits for this thread).
static void tick_thread(void* arg) {
    (void)arg;
    crash_install_handler();
    t_thread_slot = THREAD_SLOT_HELPER;
    for (;;) {
        LightEvent_Wait(&sTickEvent);
        if (!sTicking || !sTick || !sndActive) continue;
        RecursiveLock_Lock(&sLock);
        run_ticks(sFrames * 160 * 200 / NDSP_FRAME_HZ);
        RecursiveLock_Unlock(&sLock);
    }
}

// Without the DSP: ticks in real time, from the game thread once per frame
void port_musyx_pump_silent(void) {
    if (sNdsp || !sTicking || !sTick || !sndActive) return;
    PROF_ZONE("audio.musyx_ticks");
    u64 due = (svcGetSystemTick() - sSilentStart) * 200 / SYSCLOCK_ARM11;
    salPCExternalEnter();
    run_ticks(due);
    salPCExternalLeave();
}

// --- Sample memory: the DSP reads samples from linear memory -------------------------

typedef struct SampleAllocation {
    struct SampleAllocation* next;
    void* data;
} SampleAllocation;
static SampleAllocation* sSamples;
static struct {
    void* data;
    size_t length;
} sStreams[HW_MAX_STREAM_BUFFERS];
static ARAMUploadCallback sUploadCallback;
static u32 sUploadChunk;
static u32 sSampleBytes;

void aramUploadData(void* source, size_t destination, u32 length, u32 highPriority,
                    void (*callback)(size_t), MUSY_HOST_USER user) {
    (void)highPriority;
    if (length) {
        memmove((void*)destination, source, length);
        DSP_FlushDataCache((void*)destination, length);
    }
    if (callback) callback(user);
}

void aramSyncTransferQueue(void) {}

void aramInit(unsigned long length) {
    (void)length;
    sUploadCallback = NULL;
    sUploadChunk = 4096;
}

void aramExit(void) {
    while (sSamples) {
        SampleAllocation* next = sSamples->next;
        linearFree(sSamples->data);
        salFree(sSamples);
        sSamples = next;
    }
    for (u32 i = 0; i < HW_MAX_STREAM_BUFFERS; ++i) aramFreeStreamBuffer(i);
}

unsigned long aramGetZeroBuffer(void) { return 0; }

void aramSetUploadCallback(ARAMUploadCallback callback, unsigned long chunkSize) {
    sUploadCallback = callback;
    sUploadChunk = chunkSize && chunkSize <= UINT32_MAX - 31 ? (chunkSize + 31) & ~31u : 4096;
}

void* aramStoreData(void* source, unsigned long length) {
    SampleAllocation* entry = salMalloc(sizeof(*entry));
    u8* data = entry ? linearAlloc(length ? length : 1) : NULL;
    if (!data) {
        LOG_ERROR("musyx", "out of linear memory for a %lu byte sample (%lu KB stored, %lu KB free)",
                  length, (unsigned long)(sSampleBytes / 1024), (unsigned long)(linearSpaceFree() / 1024));
        if (entry) salFree(entry);
        return NULL;
    }
    if (sUploadCallback) {
        // The SDK's upload callback receives file offsets (sndPushGroup with no sample pointer)
        size_t offset = (size_t)source;
        for (u32 copied = 0; copied < length;) {
            u32 chunk = MIN(sUploadChunk, length - copied);
            const void* block = sUploadCallback((u32)offset + copied, chunk);
            if (!block) {
                linearFree(data);
                salFree(entry);
                return NULL;
            }
            memcpy(data + copied, block, chunk);
            copied += chunk;
        }
    } else {
        if (!source && length) {
            linearFree(data);
            salFree(entry);
            return NULL;
        }
        memcpy(data, source, length);
    }
    DSP_FlushDataCache(data, length);
    entry->data = data;
    entry->next = sSamples;
    sSamples = entry;
    sSampleBytes += length;
    return data;
}

void aramRemoveData(void* data, unsigned long length) {
    for (SampleAllocation** link = &sSamples; *link; link = &(*link)->next) {
        if ((*link)->data == data) {
            SampleAllocation* entry = *link;
            *link = entry->next;
            linearFree(entry->data);
            salFree(entry);
            sSampleBytes -= length;
            return;
        }
    }
}

u8 aramAllocateStreamBuffer(u32 length) {
    if (!length) return HW_STREAM_BUFFER_INVALID;
    for (u8 i = 0; i < HW_MAX_STREAM_BUFFERS; ++i) {
        if (!sStreams[i].data) {
            sStreams[i].data = linearAlloc(length);
            if (!sStreams[i].data) return HW_STREAM_BUFFER_INVALID;
            memset(sStreams[i].data, 0, length);
            sStreams[i].length = length;
            return i;
        }
    }
    return HW_STREAM_BUFFER_INVALID;
}

size_t aramGetStreamBufferAddress(u8 id, size_t* length) {
    if (length) *length = id < HW_MAX_STREAM_BUFFERS ? sStreams[id].length : 0;
    return id < HW_MAX_STREAM_BUFFERS ? (size_t)sStreams[id].data : 0;
}

void aramFreeStreamBuffer(u8 id) {
    if (id < HW_MAX_STREAM_BUFFERS && sStreams[id].data) {
        linearFree(sStreams[id].data);
        sStreams[id].data = NULL;
        sStreams[id].length = 0;
    }
}

// Debug log line: voices in use and DSP channels actually playing
void port_musyx_log_stats(void) {
    u32 voices = 0, channels = 0;
    for (u32 i = 0; i < salNumVoices; ++i) {
        voices += sHw[i].playing;
        if (sNdsp) channels += ndspChnIsPlaying(i);
    }
    static u64 lastTicks, lastTime;
    u64 ticks = sTicksDone - lastTicks, time = sTickTime - lastTime;
    lastTicks = sTicksDone;
    lastTime = sTickTime;
    LOG_DEBUG("musyx", "%lu voices, %lu DSP channels playing, %lu us per 5 ms tick, %lu KB samples, %lu KB linear free",
              (unsigned long)voices, (unsigned long)channels,
              (unsigned long)(ticks ? time * 1000000 / SYSCLOCK_ARM11 / ticks : 0),
              (unsigned long)(sSampleBytes / 1024), (unsigned long)(linearSpaceFree() / 1024));
}

// --- MusyX debugger hooks (foxhollow's debugger UI is not part of the port) ----------

bool8 musyxDebuggerIsPaused(void) { return FALSE; }
bool8 musyxDebuggerConsumeStepFrame(void) { return FALSE; }
void musyxDebuggerSetPaused(bool8 paused) { (void)paused; }
void musyxDebuggerRuntimeFrame(u32 deltaTime) { (void)deltaTime; }
void musyxDebuggerRuntimeMacroStep(SYNTH_VOICE* voice, const MSTEP* step) {
    (void)voice;
    (void)step;
}
void musyxDebuggerRuntimeVoiceStart(SYNTH_VOICE* voice) { (void)voice; }
void musyxDebuggerRuntimeVoiceStop(SYNTH_VOICE* voice) { (void)voice; }

// --- Save states ---------------------------------------------------------------------

// Identifies the sample set in linear memory, which a save state does not carry
// (the game loads all its samples at boot, so it matches within one build)
u32 port_musyx_sample_fingerprint(void) {
    u32 h = 2166136261u;
#define MIX(v) (h = (h ^ (u32)(v)) * 16777619u)
    MIX(sSampleBytes);
    for (SampleAllocation* e = sSamples; e; e = e->next) {
        MIX((uintptr_t)e);
        MIX((uintptr_t)e->data);
    }
    for (u32 i = 0; i < HW_MAX_STREAM_BUFFERS; ++i) {
        MIX((uintptr_t)sStreams[i].data);
        MIX(sStreams[i].length);
    }
#undef MIX
    return h;
}

// After game memory was replaced (MusyX lock held): restart the channels of the
// voices that were playing. Looping sounds start over; one-shots end at the
// next tick instead of replaying.
void port_musyx_after_restore(void) {
    for (u32 i = 0; i < PORT_NDSP_MUSYX_CHANNELS; ++i) {
        if (sNdsp) ndspChnWaveBufClear(i);
    }
    memset(sHw, 0, sizeof(sHw));
    if (!dspVoice) return;
    for (u32 i = 0; i < salNumVoices; ++i) {
        DSPvoice* vp = &dspVoice[i];
        if (vp->state != DSP_VOICE_STATE_PLAYING) continue;
        const SAMPLE_INFO* s = &vp->smp_info;
        bool loop = s->loopLength != 0 && s->loop + s->loopLength <= s->length;
        if (loop) {
            hw_start(i, vp);
        } else {
            sHw[i].playing = true;
            sHw[i].wb[0].status = NDSP_WBUF_DONE;
            sHw[i].silentPos = (u64)s->length << 16;
        }
    }
}
