#ifndef PORT_3DS_H
#define PORT_3DS_H

// Internal interface of the 3DS platform layer (replacement for foxhollow's
// Aurora/SDL backend).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Emulated GameCube main RAM (arena for OSAlloc/mmAlloc). The game reserves
// 0xD80000 for its fixed mm regions and gives the rest to region 0: about
// 6.5 MB on the GameCube, 14.5 MB here (foxhollow keeps some data unpacked
// next to the raw copy). It shares the 3DS heap with MusyX's sample data and
// zlib, so it is not made larger than needed.
#define PORT_MEM1_SIZE (28 * 1024 * 1024)
// Emulated ARAM (the game reserves it for MusyX, whose 3DS backend keeps
// samples in linear memory instead, see port/musyx_ndsp.c)
#define PORT_ARAM_SIZE (16 * 1024 * 1024)

// os_3ds.c
void port_os_init(void);
void* port_mem1_base(void);  // PORT_MEM1_SIZE bytes

// ar_3ds.c
void* ARGetStorageAddress(void);
uint32_t port_aram_used(void);
void port_shutdown_and_exit(int code) __attribute__((noreturn));
void padrec_flush(void);   // pad_3ds.c: write the recorded input at shutdown
void padrec_frame(void);   // pad_3ds.c: once per game frame (record/playback slot)

// vi_3ds.c
void port_frame_pump_init(void);
void port_gx_request_reset(void);  // Rebuild the GX caches at the next frame (save state load)

// dvd_3ds.c: completes queued async DVD reads (called with interrupts enabled)
void aurora_dvd_process_callbacks(void);
// Path in the RomFS of an open file
struct DVDFileInfo;
bool port_dvd_get_path(struct DVDFileInfo* fileInfo, char* out, size_t out_size);
// Save states: no reads in flight; the files open in each handle slot (entry
// numbers, -1 for free) are saved and reopened after a restore
#define PORT_DVD_MAX_OPEN 128
bool port_dvd_idle(void);
void port_dvd_save_handles(int32_t out[], int n);
bool port_dvd_restore_handles(const int32_t in[], int n);

// ndsp channels: MusyX voices first, then the AI DMA output and the disc stream
#define PORT_NDSP_MUSYX_CHANNELS 21
#define PORT_NDSP_AI_CHANNEL     21
#define PORT_NDSP_STREAM_LEFT    22
#define PORT_NDSP_STREAM_RIGHT   23

// audio_3ds.c
bool port_audio_ndsp_ready(void);   // Initializes ndsp on first use; false without DSP firmware
uint32_t fhAIGetStreamPlayAddr(void);  // Nonzero while a disc stream is playing
uint32_t port_audio_ai_latency(void);  // Samples from the AI DMA callback's output to the speaker
void port_audio_stream_reset(void);    // Stops the disc stream (save state load)

// movie_audio_3ds.c
void movie_audio_frame(void);  // ndsp frame callback
void movie_audio_pump(void);   // Once per game frame, after the AI DMA pump

// musyx_ndsp.c
void port_musyx_frame(void);        // ndsp frame callback
void port_musyx_pump_silent(void);  // Once per game frame, drives MusyX when ndsp is unavailable
void port_musyx_log_stats(void);
uint32_t port_musyx_sample_fingerprint(void);  // Save states: the sample data a snapshot relies on
void port_musyx_after_restore(void);           // Resyncs the ndsp channels with the restored voices

// savestate_snap.c: called at the top of each game frame
void port_savestate_point(void);

// Implemented by the game / foxhollow port layer
int gameMain(int argc, char** argv);
void fhAIPump(void);

#endif // PORT_3DS_H
