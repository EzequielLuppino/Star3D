// Audio for 3DS, replacing foxhollow's ai_shim.c and musyx_output.c (SDL).
//
// Everything plays on the DSP through ndsp (channel layout in port_3ds.h):
// - MusyX voices: port/musyx_ndsp.c, driven by the ndsp frame callback below.
// - Disc streams (voice acting, /streams/*.adp): transcoded at build time to
//   DSP-ADPCM in chunks of STREAM_CHUNK_BYTES, left half then right half (see
//   tools/audio_convert.py). A reader thread loads chunks from the RomFS into a
//   ring of linear buffers (so the game never waits for the SD card), and the
//   ndsp thread queues them on two channels that the DSP decodes itself.
// - AI DMA: the game's PCM DMA interface works like the GameCube's: a buffer
//   is played and the DMA callback supplies the next one. MusyX no longer
//   renders into it, but the attract movie mixes its audio into these buffers.
//   fhAIPump(), called once per frame, keeps about AI_QUEUE_MS queued.
//
// ndsp needs the DSP firmware (sdmc:/3ds/dspfirm.cdc). Without it everything
// runs silently at the real-time rate, so whatever depends on audio progress
// (music timing, movies, end of voice lines) keeps working.

#include <3ds.h>
#include "crash.h"
#include <dolphin/ai.h>
#include <dolphin/dvd.h>
#include <musyx/musyx.h>
#include <musyx/pc.h>
#include <stdio.h>
#include <string.h>
#include "log.h"
#include "port_3ds.h"
#include "profiler.h"
#include "worker.h"

#define AI_DMA_RATE    32028   // GameCube AI DMA sample rate (stereo s16)
#define AI_QUEUE_MS    100     // Covers 30 fps frames plus the occasional long one
#define AI_QUEUE_BYTES ((AI_DMA_RATE * 4 * AI_QUEUE_MS) / 1000)
#define WAVEBUF_COUNT  48
#define WAVEBUF_BYTES  4096    // Largest DMA buffer accepted
#define AI_CHANNEL     PORT_NDSP_AI_CHANNEL

static AIDCallback sDmaCallback;
static uintptr_t sDmaStartAddr;
static u32 sDmaLength;
static bool sDmaRunning;
static u32 sDspSampleRate;

static bool sNdsp;
static ndspWaveBuf sWaveBufs[WAVEBUF_COUNT];
static u8* sWaveData;
static u64 sSilentStartTick;   // Silent mode: real-time pacing
static u64 sSilentBytes;

static void stream_frame(void);

static void frame_callback(void* data) {
    (void)data;
    port_musyx_frame();
    stream_frame();
    movie_audio_frame();
}

bool port_audio_ndsp_ready(void) {
    static bool tried;
    if (tried) return sNdsp;
    tried = true;
    Result rc = ndspInit();
    if (R_FAILED(rc)) {
        LOG_WARN("audio", "ndspInit failed (0x%08lX): no DSP firmware? Audio runs silently", rc);
        return false;
    }
    sWaveData = linearAlloc(WAVEBUF_COUNT * WAVEBUF_BYTES);
    if (!sWaveData) {
        LOG_ERROR("audio", "no linear memory for audio buffers");
        ndspExit();
        return false;
    }
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnReset(AI_CHANNEL);
    ndspChnSetInterp(AI_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AI_CHANNEL, AI_DMA_RATE);
    ndspChnSetFormat(AI_CHANNEL, NDSP_FORMAT_STEREO_PCM16);
    float mix[12] = { 1.0f, 1.0f };
    ndspChnSetMix(AI_CHANNEL, mix);
    memset(sWaveBufs, 0, sizeof(sWaveBufs));
    sNdsp = true;
    ndspSetCallback(frame_callback, NULL);
    LOG_INFO("audio", "ndsp ready");
    return true;
}

// --- MusyX output (foxhollow's musyx_output.c interface). MusyX plays on its
// own DSP channels; the AI buffers it used to fill stay silent, since the
// attract movie chains its DMA callback in front of this one and mixes into
// them. ---------------------------------------------------------------------------

#define MUSYX_BUFFER_COUNT 4
#define MUSYX_CHUNK_FRAMES 160
#define MUSYX_CHUNK_BYTES  (MUSYX_CHUNK_FRAMES * 2 * sizeof(s16))

static s16 sMusyxBuffers[MUSYX_BUFFER_COUNT][MUSYX_CHUNK_FRAMES * 2];
static u32 sMusyxBufferIndex;

static void musyx_dma_callback(void) {
    sMusyxBufferIndex = (sMusyxBufferIndex + 1) % MUSYX_BUFFER_COUNT;
    AIInitDMA((uintptr_t)sMusyxBuffers[sMusyxBufferIndex], MUSYX_CHUNK_BYTES);
}

void fhMusyxConfigure(void) {
    SND_PC_CONFIG config = { 32000, 2 };
    sndPCConfigure(&config);
}

void fhMusyxStartOutput(void) {
    memset(sMusyxBuffers, 0, sizeof(sMusyxBuffers));
    sMusyxBufferIndex = 1;
    AIRegisterDMACallback(musyx_dma_callback);
    AIInitDMA((uintptr_t)sMusyxBuffers[sMusyxBufferIndex], MUSYX_CHUNK_BYTES);
    AIStartDMA();
}

void fhMusyxStopOutput(void) {
    AIRegisterDMACallback(NULL);
    AIStopDMA();
}

// --- AI DMA ------------------------------------------------------------------------

void AIInit(u8* stack) { (void)stack; }
void AIReset(void) { AIStopDMA(); }
BOOL AICheckInit(void) { return TRUE; }

AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    AIDCallback previous = sDmaCallback;
    sDmaCallback = callback;
    return previous;
}

void AIInitDMA(uintptr_t startAddr, u32 length) {
    sDmaStartAddr = startAddr;
    sDmaLength = length;
}

void AIStartDMA(void) {
    port_audio_ndsp_ready();
    if (!sDmaRunning) {
        sSilentStartTick = svcGetSystemTick();
        sSilentBytes = 0;
    }
    sDmaRunning = true;
}

void AIStopDMA(void) {
    sDmaRunning = false;
    if (sNdsp) ndspChnWaveBufClear(AI_CHANNEL);
}

static void run_callback(void) {
    AIDCallback cb = sDmaCallback;
    if (cb) cb();
}

static void pump_dma(void) {
    if (!sDmaRunning) return;

    if (sNdsp) {
        u32 queued = 0;
        for (int i = 0; i < WAVEBUF_COUNT; i++) {
            u8 st = sWaveBufs[i].status;
            if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) queued += sWaveBufs[i].nsamples * 4;
        }
        for (int guard = 0; queued < AI_QUEUE_BYTES && guard < WAVEBUF_COUNT; guard++) {
            ndspWaveBuf* wb = NULL;
            for (int i = 0; i < WAVEBUF_COUNT && !wb; i++) {
                u8 st = sWaveBufs[i].status;
                if (st == NDSP_WBUF_FREE || st == NDSP_WBUF_DONE) wb = &sWaveBufs[i];
            }
            if (!wb) break;
            u32 len = sDmaLength > WAVEBUF_BYTES ? WAVEBUF_BYTES : sDmaLength & ~3u;
            if (sDmaStartAddr && len) {
                u8* dst = sWaveData + (wb - sWaveBufs) * WAVEBUF_BYTES;
                memcpy(dst, (const void*)sDmaStartAddr, len);
                DSP_FlushDataCache(dst, len);
                memset(wb, 0, sizeof(*wb));
                wb->data_vaddr = dst;
                wb->nsamples = len / 4;
                ndspChnWaveBufAdd(AI_CHANNEL, wb);
                queued += len;
            }
            run_callback();
            if (!len) break;
        }
        return;
    }

    // Silent: run the callback as often as the buffers would have played
    u64 elapsed = svcGetSystemTick() - sSilentStartTick;
    u64 due = elapsed * (AI_DMA_RATE * 4ull) / SYSCLOCK_ARM11;
    if (due > sSilentBytes + AI_QUEUE_BYTES) sSilentBytes = due - AI_QUEUE_BYTES;  // Don't catch up long stalls
    for (int guard = 0; sSilentBytes < due && guard < 64; guard++) {
        u32 len = sDmaLength ? sDmaLength : MUSYX_CHUNK_BYTES;
        sSilentBytes += len;
        run_callback();
    }
}

// Samples between the last one the DMA callback produced and the one being heard:
// the queued AI buffers (less what has played of the current one) plus the
// buffer the callback just filled, which is copied on the next pump
u32 port_audio_ai_latency(void) {
    if (!sNdsp || !sDmaRunning) return sDmaLength / 4;
    u32 queued = 0;
    for (int i = 0; i < WAVEBUF_COUNT; i++) {
        u8 st = sWaveBufs[i].status;
        if (st == NDSP_WBUF_QUEUED || st == NDSP_WBUF_PLAYING) queued += sWaveBufs[i].nsamples;
    }
    u32 pos = ndspChnGetSamplePos(AI_CHANNEL);
    return (queued > pos ? queued - pos : 0) + sDmaLength / 4;
}

uintptr_t fhAIGetDMAStartAddr(void) { return sDmaStartAddr; }
u32 AIGetDMAStartAddr(void) { return (u32)sDmaStartAddr; }
u32 AIGetDMALength(void) { return sDmaLength; }
AIDCallback AIGetDMACallback(void) { return sDmaCallback; }
BOOL AIGetDMAEnableFlag(void) { return sDmaRunning; }
u32 AIGetDMABytesLeft(void) { return sDmaLength; }
void AISetDSPSampleRate(u32 rate) { sDspSampleRate = rate; }
u32 AIGetDSPSampleRate(void) { return sDspSampleRate; }

// --- Disc streams ----------------------------------------------------------------

#define STREAM_RATE        48043  // GameCube DTK playback rate
#define STREAM_CHUNK_BYTES 16384  // Must match tools/audio_convert.py
#define STREAM_SLOTS       6      // ~1.8 s of buffering, ~0.9 s kept queued
#define STREAM_BLOCK_BYTES 32     // One DTK block: 28 stereo samples
#define STREAM_LEFT        PORT_NDSP_STREAM_LEFT
#define STREAM_RIGHT       PORT_NDSP_STREAM_RIGHT

// DTK's prediction filters as DSP-ADPCM coefficients (tools/audio_convert.py)
static const u16 kStreamCoefs[16] = { 0, 0, 1920, 0, 3680, (u16)-1664, 3136, (u16)-1760 };

enum { SLOT_FREE, SLOT_READY, SLOT_QUEUED };
typedef struct {
    volatile u8 state;
    u32 bytes;
    ndspWaveBuf wb[2];
} StreamSlot;

static AISCallback sStreamCallback;
static u32 sStreamPlayState, sStreamTrigger;
static u8 sStreamVolLeft, sStreamVolRight;

static LightLock sStreamLock;    // Ring state, shared with the ndsp thread
static LightLock sStreamIoLock;  // File and read position, shared with the reader thread
static LightEvent sStreamIoEvent;
static Thread sStreamThread;
static StreamSlot sSlots[STREAM_SLOTS];
static u8* sStreamData;          // STREAM_SLOTS chunks of linear memory
static FILE* sStreamFile;
static u32 sStreamLength, sStreamReadPos;
static u32 sFillSlot, sQueueSlot;
static volatile bool sStreamActive;   // Started and not finished
static volatile bool sStreamFirstChunk;
static ndspAdpcmData sStreamAdpcm[2];
static u32 sStreamSamplesPlayed;
static u64 sStreamSilentStart;

static u32 stream_samples(u32 bytes) { return bytes / STREAM_BLOCK_BYTES * 28; }

static void stream_fill(void);

static void stream_thread(void* arg) {
    (void)arg;
    crash_install_handler();
    t_thread_slot = THREAD_SLOT_HELPER;
    for (;;) {
        LightEvent_Wait(&sStreamIoEvent);
        LightLock_Lock(&sStreamIoLock);
        stream_fill();
        LightLock_Unlock(&sStreamIoLock);
    }
}

static void stream_init(void) {
    static bool done;
    if (done) return;
    done = true;
    LightLock_Init(&sStreamLock);
    LightLock_Init(&sStreamIoLock);
    LightEvent_Init(&sStreamIoEvent, RESET_ONESHOT);
    if (!port_audio_ndsp_ready()) return;
    // On core 1 with the other helpers (worker.h core1_init), else on the game
    // core above the game thread, so a read starts at once; it sleeps during
    // the I/O
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    if (core1_init()) sStreamThread = threadCreate(stream_thread, NULL, 16 * 1024, priority - 1, 1, true);
    if (!sStreamThread) sStreamThread = threadCreate(stream_thread, NULL, 16 * 1024, priority - 1, -2, true);
    if (!sStreamThread) LOG_ERROR("audio", "cannot create the stream reader thread");
}

static void stream_close(void) {
    LightLock_Lock(&sStreamIoLock);
    LightLock_Lock(&sStreamLock);
    sStreamActive = false;
    if (sNdsp) {
        ndspChnWaveBufClear(STREAM_LEFT);
        ndspChnWaveBufClear(STREAM_RIGHT);
    }
    for (int i = 0; i < STREAM_SLOTS; i++) sSlots[i].state = SLOT_FREE;
    LightLock_Unlock(&sStreamLock);
    if (sStreamFile) fclose(sStreamFile);
    sStreamFile = NULL;
    LightLock_Unlock(&sStreamIoLock);
}

static void stream_apply_volume(void) {
    if (!sNdsp) return;
    float left[12] = { sStreamVolLeft / 255.0f, 0 };
    float right[12] = { 0, sStreamVolRight / 255.0f };
    ndspChnSetMix(STREAM_LEFT, left);
    ndspChnSetMix(STREAM_RIGHT, right);
}

// Read chunks into free slots, in ring order (reader thread, sStreamIoLock held)
static void stream_fill(void) {
    while (sStreamFile && sStreamReadPos < sStreamLength && sSlots[sFillSlot].state == SLOT_FREE) {
        StreamSlot* slot = &sSlots[sFillSlot];
        u8* dst = sStreamData + sFillSlot * STREAM_CHUNK_BYTES;
        u32 bytes = sStreamLength - sStreamReadPos;
        if (bytes > STREAM_CHUNK_BYTES) bytes = STREAM_CHUNK_BYTES;
        if (fseek(sStreamFile, sStreamReadPos, SEEK_SET) != 0 || fread(dst, 1, bytes, sStreamFile) != bytes) {
            LOG_ERROR("audio", "stream read failed at %lu", (unsigned long)sStreamReadPos);
            sStreamReadPos = sStreamLength;
            break;
        }
        DSP_FlushDataCache(dst, bytes);
        u32 half = bytes / 2;
        memset(slot->wb, 0, sizeof(slot->wb));
        for (int ch = 0; ch < 2; ch++) {
            slot->wb[ch].data_vaddr = dst + ch * half;
            slot->wb[ch].nsamples = half / 8 * 14;
        }
        slot->bytes = bytes;
        __dmb();
        slot->state = SLOT_READY;
        sStreamReadPos += bytes;  // After the slot: the game thread checks for the end with it
        sFillSlot = (sFillSlot + 1) % STREAM_SLOTS;
    }
}

// Queue ready chunks on both channels (ndsp thread, before channel updates, so
// the two channels always start in the same DSP frame)
static void stream_frame(void) {
    if (!sStreamActive || LightLock_TryLock(&sStreamLock) != 0) return;
    while (sStreamActive && sSlots[sQueueSlot].state == SLOT_READY) {
        StreamSlot* slot = &sSlots[sQueueSlot];
        for (int ch = 0; ch < 2; ch++) {
            if (sStreamFirstChunk) {
                const u8* data = slot->wb[ch].data_vaddr;
                sStreamAdpcm[ch] = (ndspAdpcmData){ data[0], 0, 0 };
                slot->wb[ch].adpcm_data = &sStreamAdpcm[ch];
            }
            ndspChnWaveBufAdd(ch ? STREAM_RIGHT : STREAM_LEFT, &slot->wb[ch]);
        }
        sStreamFirstChunk = false;
        slot->state = SLOT_QUEUED;
        sQueueSlot = (sQueueSlot + 1) % STREAM_SLOTS;
    }
    LightLock_Unlock(&sStreamLock);
}

static void stream_pump(void) {
    if (!sStreamFile) return;
    PROF_ZONE("audio.stream");
    if (!sNdsp) {
        if (sStreamActive) {
            u64 elapsed = svcGetSystemTick() - sStreamSilentStart;
            sStreamSamplesPlayed = (u32)(elapsed * STREAM_RATE / SYSCLOCK_ARM11);
            if (sStreamSamplesPlayed >= stream_samples(sStreamLength)) {
                sStreamActive = false;
                sStreamPlayState = AI_STREAM_STOP;
            }
        }
        return;
    }

    LightLock_Lock(&sStreamLock);
    bool pending = false;
    for (int i = 0; i < STREAM_SLOTS; i++) {
        StreamSlot* slot = &sSlots[i];
        if (slot->state == SLOT_QUEUED && slot->wb[0].status == NDSP_WBUF_DONE &&
            slot->wb[1].status == NDSP_WBUF_DONE) {
            sStreamSamplesPlayed += stream_samples(slot->bytes);
            slot->state = SLOT_FREE;
        }
        if (slot->state != SLOT_FREE) pending = true;
    }
    LightLock_Unlock(&sStreamLock);
    if (sStreamReadPos < sStreamLength) LightEvent_Signal(&sStreamIoEvent);
    if (sStreamActive && !pending && sStreamReadPos >= sStreamLength) {
        sStreamActive = false;
        sStreamPlayState = AI_STREAM_STOP;
    }
    if (sStreamCallback && sStreamTrigger && sStreamSamplesPlayed >= sStreamTrigger) {
        sStreamTrigger = 0;
        sStreamCallback(sStreamSamplesPlayed);
    }
}

int fhAIPrepareStream(void* fileInfoPtr) {
    DVDFileInfo* fileInfo = fileInfoPtr;
    char path[256];
    stream_init();
    stream_close();
    if (!fileInfo || !fileInfo->length || (fileInfo->length % STREAM_BLOCK_BYTES) != 0 ||
        !port_dvd_get_path(fileInfo, path, sizeof(path))) {
        return 0;
    }
    port_audio_ndsp_ready();
    if (sNdsp && !sStreamData) {
        sStreamData = linearAlloc(STREAM_SLOTS * STREAM_CHUNK_BYTES);
        if (!sStreamData) {
            LOG_ERROR("audio", "no linear memory for stream buffers");
            return 0;
        }
    }
    FILE* file = fopen(path, "rb");
    if (!file) {
        LOG_ERROR("audio", "cannot open stream %s", path);
        return 0;
    }
    setvbuf(file, NULL, _IONBF, 0);
    LightLock_Lock(&sStreamIoLock);
    sStreamFile = file;
    sStreamLength = fileInfo->length;
    sStreamReadPos = 0;
    sFillSlot = sQueueSlot = 0;
    LightLock_Unlock(&sStreamIoLock);
    sStreamSamplesPlayed = 0;
    sStreamPlayState = AI_STREAM_STOP;
    sStreamFirstChunk = true;
    LOG_DEBUG("audio", "stream %s (%.1f s)", path, stream_samples(sStreamLength) / (float)STREAM_RATE);

    if (sNdsp) {
        for (int ch = 0; ch < 2; ch++) {
            int id = ch ? STREAM_RIGHT : STREAM_LEFT;
            ndspChnReset(id);
            ndspChnSetFormat(id, NDSP_FORMAT_MONO_ADPCM);
            ndspChnSetRate(id, STREAM_RATE);
            ndspChnSetInterp(id, NDSP_INTERP_POLYPHASE);
            ndspChnSetAdpcmCoefs(id, (u16*)kStreamCoefs);
        }
        stream_apply_volume();
        LightEvent_Signal(&sStreamIoEvent);  // Read ahead before the game starts the stream
    }
    return 1;
}

uint32_t fhAIGetStreamPlayAddr(void) {
    return sStreamActive ? (sStreamReadPos | 0x80) : 0;
}

AISCallback AIRegisterStreamCallback(AISCallback callback) {
    AISCallback previous = sStreamCallback;
    sStreamCallback = callback;
    return previous;
}
u32 AIGetStreamSampleCount(void) { return sStreamSamplesPlayed; }
void AIResetStreamSampleCount(void) { sStreamSamplesPlayed = 0; }
void AISetStreamTrigger(u32 trigger) { sStreamTrigger = trigger; }
u32 AIGetStreamTrigger(void) { return sStreamTrigger; }

void AISetStreamPlayState(u32 state) {
    sStreamPlayState = state;
    if (state == AI_STREAM_START) {
        if (sStreamFile && !sStreamActive) {
            sStreamSilentStart = svcGetSystemTick();
            sStreamActive = true;
        }
    } else if (sStreamFile) {
        stream_close();
    }
}

u32 AIGetStreamPlayState(void) { return sStreamPlayState; }
void AISetStreamSampleRate(u32 rate) { (void)rate; }
u32 AIGetStreamSampleRate(void) { return AI_SAMPLERATE_48KHZ; }
void AISetStreamVolLeft(u8 vol) {
    sStreamVolLeft = vol;
    stream_apply_volume();
}
void AISetStreamVolRight(u8 vol) {
    sStreamVolRight = vol;
    stream_apply_volume();
}
u8 AIGetStreamVolLeft(void) { return sStreamVolLeft; }
u8 AIGetStreamVolRight(void) { return sStreamVolRight; }

// --- Per-frame pump --------------------------------------------------------------

// Called once per frame from VIWaitForRetrace
void fhAIPump(void) {
    PROF_ZONE("audio.pump");
    port_musyx_pump_silent();
    pump_dma();
    stream_pump();
    movie_audio_pump();
    static u32 frames;
    if (++frames % 300 == 0) {
        port_musyx_log_stats();
        if (sStreamActive)
            LOG_DEBUG("audio", "stream at %.1f s, volume %d/%d", sStreamSamplesPlayed / (float)STREAM_RATE,
                      sStreamVolLeft, sStreamVolRight);
    }
}

// After a save state restore: drop the playing stream, the game restarts it
void port_audio_stream_reset(void) {
    stream_init();
    stream_close();
    sStreamPlayState = AI_STREAM_STOP;
    sStreamTrigger = 0;
    sStreamSamplesPlayed = 0;
}
