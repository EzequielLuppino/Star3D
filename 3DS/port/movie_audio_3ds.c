// Attract movie audio on the DSP, from the build-time DSP-ADPCM track (.a3d
// layout in tools/movie_audio_convert.py).
//
// The game's player is unchanged: it decodes each frame's audio into one of
// its three audio buffers and mixes them into the AI DMA output, and the movie
// follows the audio frame being mixed. Here THPAudioDecode returns silence of
// the right length (identifying the frame by a hash of its audio header, and
// noting which track sample the buffer starts at), and movie_audio_pump() plays
// the track on the stream channels from the sample the game is mixing, less
// the AI output latency. The two play at the AI DMA rate, so they stay in step;
// a drift beyond RESYNC_SAMPLES (a skipped frame, the movie looping) restarts
// the track at the game's position.
//
// The track is read in chunks into a ring of linear buffers on the worker, and
// the ndsp thread queues them on both channels in the same DSP frame.
//
// Without a .a3d, ndsp or with S3D_MOVIE=0, aurora's decoder runs instead and
// the game mixes the audio itself; also while a disc stream holds the channels.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <3ds.h>
#include "config.h"
#include "log.h"
#include "main/dll/FRONT/attract_movie.h"
#include "port_3ds.h"
#include "profiler.h"
#include "worker.h"

#define A3D_PATH       "romfs:/files/starfox.a3d"  // named after the disc's .thp
#define A3D_MAGIC      0x31443341u  // "A3D1"
#define PLAY_RATE      32028        // AI DMA rate, the game's movie clock
#define CHUNK_BLOCKS   512          // 7168 samples per chunk and channel
#define CHUNK_BYTES    (CHUNK_BLOCKS * 8)
#define SLOTS          4
#define RESYNC_SAMPLES (PLAY_RATE / 5)
#define MAP_ENTRIES    8
#define HASH_BYTES     144
#define CH_LEFT        PORT_NDSP_STREAM_LEFT
#define CH_RIGHT       PORT_NDSP_STREAM_RIGHT

typedef struct {
    u32 magic, header_size, rate, frames, samples, channels;
    u32 frame_off, seek_off, seek_blocks, seek_count, data_off, channel_bytes;
    s16 coefs[2][16];
} A3dHeader;

typedef struct {
    u32 hash, first;
} A3dFrame;

enum { SLOT_FREE, SLOT_READING, SLOT_READY, SLOT_QUEUED };

typedef struct {
    volatile u32 state;
    u32 block;    // First block of the chunk
    u32 samples;  // Samples per channel
    u8* data;     // Linear memory, left chunk then right chunk
    ndspWaveBuf wb[2];
    WorkerJob job;
} Slot;

typedef struct {
    const s16* buffer;
    u32 first;  // Track sample of buffer[0], ~0u = not in the track
} BufferMap;

extern u16 gAttractMovieVolumeScale[128];
u32 THPAudioDecode_sw(s16* output, u8* frame, s32 flag);

static int s_state;  // 0 not tried, 1 open, -1 unavailable
static FILE* s_file;
static A3dHeader s_hdr;
static A3dFrame* s_frames;
static s16 (*s_seek)[4];
static u32 s_blocks;
static int s_last_frame = -1;
static BufferMap s_map[MAP_ENTRIES];
static int s_map_next;

static Slot s_slots[SLOTS];
static LightLock s_lock;  // Slot ring and channel queue (game thread / ndsp thread)
static bool s_playing;
static volatile bool s_first_chunk;
static volatile bool s_want_pause, s_paused;
static u32 s_fill_slot, s_queue_slot;
static u32 s_fill_block;
static u32 s_play_start;  // Track sample at the start of the first chunk
static u32 s_done_samples;
static ndspAdpcmData s_adpcm[2];
static int s_volume = -1;

static bool a3d_open(void) {
    if (s_state) return s_state > 0;
    s_state = -1;
    if (!cfg_bool("S3D_MOVIE", true)) return false;
    s_file = fopen(A3D_PATH, "rb");
    if (!s_file) {
        LOG_WARN("movie", "%s not found: movie audio decoded in software", A3D_PATH);
        return false;
    }
    setvbuf(s_file, NULL, _IONBF, 0);
    const A3dHeader* h = &s_hdr;
    if (fread(&s_hdr, sizeof(s_hdr), 1, s_file) != 1 || h->magic != A3D_MAGIC || h->channels != 2 ||
        !h->frames || !h->seek_blocks || h->seek_blocks > CHUNK_BLOCKS || CHUNK_BLOCKS % h->seek_blocks ||
        h->channel_bytes % 8) {
        LOG_ERROR("movie", "%s: bad header", A3D_PATH);
        goto fail;
    }
    s_blocks = h->channel_bytes / 8;
    s_frames = malloc(h->frames * sizeof(A3dFrame));
    s_seek = malloc(h->seek_count * sizeof(*s_seek));
    if (!s_frames || !s_seek || fseek(s_file, (long)h->frame_off, SEEK_SET) != 0 ||
        fread(s_frames, sizeof(A3dFrame), h->frames, s_file) != h->frames ||
        fseek(s_file, (long)h->seek_off, SEEK_SET) != 0 ||
        fread(s_seek, sizeof(*s_seek), h->seek_count, s_file) != h->seek_count) {
        LOG_ERROR("movie", "%s: cannot read the frame and seek tables", A3D_PATH);
        goto fail;
    }
    u8* data = linearAlloc(SLOTS * 2 * CHUNK_BYTES);
    if (!data) {
        LOG_ERROR("movie", "no linear memory for movie audio");
        goto fail;
    }
    for (int i = 0; i < SLOTS; i++) s_slots[i].data = data + i * 2 * CHUNK_BYTES;
    LightLock_Init(&s_lock);
    LOG_INFO("movie", "%s: %.1f s DSP-ADPCM stereo, %lu frames", A3D_PATH, h->samples / (float)h->rate,
             (unsigned long)h->frames);
    s_state = 1;
    return true;

fail:
    fclose(s_file);
    s_file = NULL;
    free(s_frames);
    free(s_seek);
    s_frames = NULL;
    s_seek = NULL;
    return false;
}

// Must match frame_hash() in tools/movie_audio_convert.py
static u32 frame_hash(const u8* audio) {
    u32 h = 2166136261u;
    for (int i = 0; i < HASH_BYTES; i++) h = (h ^ audio[i]) * 16777619u;
    return h;
}

static int find_frame(u32 hash) {
    int n = (int)s_hdr.frames;
    int guess[2] = { (s_last_frame + 1) % n, s_last_frame < 0 ? 0 : s_last_frame };
    for (int i = 0; i < 2; i++)
        if (s_frames[guess[i]].hash == hash) return guess[i];
    for (int i = 0; i < n; i++)
        if (s_frames[i].hash == hash) return i;
    return -1;
}

static void map_buffer(const s16* buffer, u32 first) {
    for (int i = 0; i < MAP_ENTRIES; i++) {
        if (s_map[i].buffer == buffer) {
            s_map[i].first = first;
            return;
        }
    }
    s_map[s_map_next] = (BufferMap){ buffer, first };
    s_map_next = (s_map_next + 1) % MAP_ENTRIES;
}

static bool map_lookup(const s16* buffer, u32* first) {
    for (int i = 0; i < MAP_ENTRIES; i++) {
        if (s_map[i].buffer == buffer) {
            *first = s_map[i].first;
            return *first != ~0u;
        }
    }
    return false;
}

static bool dsp_available(void) {
    return a3d_open() && port_audio_ndsp_ready() && !fhAIGetStreamPlayAddr();
}

u32 THPAudioDecode(s16* output, u8* frame, s32 flag) {
    if (!dsp_available()) {
        if (s_state > 0) map_buffer(output, ~0u);
        return THPAudioDecode_sw(output, frame, flag);
    }
    u32 count = ((u32)frame[4] << 24) | ((u32)frame[5] << 16) | ((u32)frame[6] << 8) | frame[7];
    int index = find_frame(frame_hash(frame));
    if (index < 0) {
        LOG_ONCE(LOG_LEVEL_WARN, "movie", "movie audio frame not found in %s (decoded in software)", A3D_PATH);
        map_buffer(output, ~0u);
        return THPAudioDecode_sw(output, frame, flag);
    }
    s_last_frame = index;
    memset(output, 0, count * 2 * sizeof(s16));
    map_buffer(output, s_frames[index].first);
    return count;
}

// --- Playback --------------------------------------------------------------------

static void read_chunk(void* arg) {
    Slot* slot = arg;
    u32 bytes = slot->samples / 14 * 8 + (slot->samples % 14 ? 8 : 0);
    bool ok = true;
    for (int ch = 0; ch < 2 && ok; ch++) {
        long off = (long)(s_hdr.data_off + ch * s_hdr.channel_bytes + slot->block * 8);
        ok = fseek(s_file, off, SEEK_SET) == 0 && fread(slot->data + ch * CHUNK_BYTES, 1, bytes, s_file) == bytes;
    }
    if (!ok) {
        LOG_ONCE(LOG_LEVEL_ERROR, "movie", "read error in %s", A3D_PATH);
        memset(slot->data, 0, 2 * CHUNK_BYTES);
    }
    DSP_FlushDataCache(slot->data, 2 * CHUNK_BYTES);
    memset(slot->wb, 0, sizeof(slot->wb));
    for (int ch = 0; ch < 2; ch++) {
        slot->wb[ch].data_vaddr = slot->data + ch * CHUNK_BYTES;
        slot->wb[ch].nsamples = slot->samples;
    }
    __dmb();
    slot->state = SLOT_READY;
}

// Queue ready chunks on both channels (ndsp thread, before channel updates, so
// the two channels start and pause in the same DSP frame)
void movie_audio_frame(void) {
    if (!s_playing || LightLock_TryLock(&s_lock) != 0) return;
    if (s_want_pause != s_paused) {
        s_paused = s_want_pause;
        ndspChnSetPaused(CH_LEFT, s_paused);
        ndspChnSetPaused(CH_RIGHT, s_paused);
    }
    while (s_playing && s_slots[s_queue_slot].state == SLOT_READY) {
        Slot* slot = &s_slots[s_queue_slot];
        for (int ch = 0; ch < 2; ch++) {
            if (s_first_chunk) {
                const s16* seek = s_seek[slot->block / s_hdr.seek_blocks];
                s_adpcm[ch] = (ndspAdpcmData){ slot->data[ch * CHUNK_BYTES], seek[ch * 2], seek[ch * 2 + 1] };
                slot->wb[ch].adpcm_data = &s_adpcm[ch];
            }
            ndspChnWaveBufAdd(ch ? CH_RIGHT : CH_LEFT, &slot->wb[ch]);
        }
        s_first_chunk = false;
        slot->state = SLOT_QUEUED;
        s_queue_slot = (s_queue_slot + 1) % SLOTS;
    }
    LightLock_Unlock(&s_lock);
}

static void stop(void) {
    if (!s_playing) return;
    LightLock_Lock(&s_lock);
    s_playing = false;
    LightLock_Unlock(&s_lock);
    for (int i = 0; i < SLOTS; i++)
        if (s_slots[i].job.state) worker_wait(&s_slots[i].job);
    // A disc stream that took the channels has reset them already
    if (!fhAIGetStreamPlayAddr()) {
        ndspChnWaveBufClear(CH_LEFT);
        ndspChnWaveBufClear(CH_RIGHT);
    }
    for (int i = 0; i < SLOTS; i++) s_slots[i].state = SLOT_FREE;
}

static void start(u32 sample) {
    stop();
    u32 block = sample / 14 / s_hdr.seek_blocks * s_hdr.seek_blocks;
    for (int ch = 0; ch < 2; ch++) {
        int id = ch ? CH_RIGHT : CH_LEFT;
        ndspChnReset(id);
        ndspChnSetFormat(id, NDSP_FORMAT_MONO_ADPCM);
        ndspChnSetRate(id, PLAY_RATE);
        ndspChnSetInterp(id, NDSP_INTERP_POLYPHASE);
        ndspChnSetAdpcmCoefs(id, (u16*)s_hdr.coefs[ch]);
    }
    s_volume = -1;
    s_fill_slot = s_queue_slot = 0;
    s_fill_block = block;
    s_play_start = block * 14;
    s_done_samples = 0;
    s_first_chunk = true;
    s_want_pause = s_paused = false;
    s_playing = true;
    PROF_COUNT("movie.audio_syncs", 1);
}

static u32 played_position(void) {
    u32 pos = s_play_start + s_done_samples;
    for (int i = 0; i < SLOTS; i++) {
        if (s_slots[i].state == SLOT_QUEUED && s_slots[i].wb[0].status == NDSP_WBUF_PLAYING) {
            pos += ndspChnGetSamplePos(CH_LEFT);
            break;
        }
    }
    return pos;
}

static void refill(void) {
    LightLock_Lock(&s_lock);
    for (int i = 0; i < SLOTS; i++) {
        Slot* slot = &s_slots[i];
        if (slot->state == SLOT_QUEUED && slot->wb[0].status == NDSP_WBUF_DONE &&
            slot->wb[1].status == NDSP_WBUF_DONE) {
            s_done_samples += slot->samples;
            slot->state = SLOT_FREE;
        }
    }
    LightLock_Unlock(&s_lock);
    while (s_fill_block < s_blocks && s_slots[s_fill_slot].state == SLOT_FREE) {
        Slot* slot = &s_slots[s_fill_slot];
        if (slot->job.state) worker_wait(&slot->job);
        u32 blocks = s_blocks - s_fill_block < CHUNK_BLOCKS ? s_blocks - s_fill_block : CHUNK_BLOCKS;
        u32 first = s_fill_block * 14;
        slot->block = s_fill_block;
        slot->samples = blocks * 14;
        if (first + slot->samples > s_hdr.samples) slot->samples = s_hdr.samples - first;
        slot->state = SLOT_READING;
        worker_submit(&slot->job, read_chunk, slot);
        s_fill_block += blocks;
        s_fill_slot = (s_fill_slot + 1) % SLOTS;
    }
}

void movie_audio_pump(void) {
    if (s_state <= 0) return;
    AttractMoviePlayer* p = &gAttractMoviePlayer;
    if (!p->isOpen || !p->audioExists || !port_audio_ndsp_ready() || fhAIGetStreamPlayAddr()) {
        stop();
        return;
    }
    PROF_ZONE("movie.audio");
    bool running = p->internalState == 2;
    s_want_pause = s_playing && !running;

    // Track sample the game mixed last, less what is still between it and the speaker
    AttractMovieAudioBuffer* buf = p->curAudioBuffer;
    u32 first;
    if (running && buf && buf->buffer && map_lookup(buf->buffer, &first)) {
        u32 mixed = first + (u32)(buf->curPtr - buf->buffer) / 2;
        u32 latency = port_audio_ai_latency();
        u32 target = mixed > latency ? mixed - latency : 0;
        if (target < s_hdr.samples) {
            u32 played = played_position();
            s32 drift = (s32)(played - target);
            if (!s_playing || drift > RESYNC_SAMPLES || drift < -RESYNC_SAMPLES) start(target);
        }
    }
    if (!s_playing) return;
    refill();

    int volume = (int)p->curVolume;
    volume = volume < 0 ? 0 : volume > 127 ? 127 : volume;
    if (volume != s_volume) {
        s_volume = volume;
        float v = gAttractMovieVolumeScale[volume] / 32768.0f;
        float left[12] = { v, 0 };
        float right[12] = { 0, v };
        ndspChnSetMix(CH_LEFT, left);
        ndspChnSetMix(CH_RIGHT, right);
    }
}
