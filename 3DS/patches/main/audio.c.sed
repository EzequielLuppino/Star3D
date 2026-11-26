# The GameCube version allows 48 voices, at most 24 of them for sound effects.
# The software mixer (MUSYX_MIX=soft, musyx_mix.h) keeps that. With one 3DS DSP
# channel per voice, 21 of the 24 channels play voices, the others the disc
# stream (2) and the AI DMA output (1); keep the ratio.
s|sndInit(0x30, 0x30, 0x18, 1, 1, 0x1000000);|sndInit(PORT_MUSYX_VOICES, PORT_MUSYX_VOICES, PORT_MUSYX_VOICES / 2, 1, 1, 0x1000000);|
s|sndSetMaxVoices(0x30, 0x18);|sndSetMaxVoices(PORT_MUSYX_VOICES, PORT_MUSYX_VOICES / 2);|
# MusyX allocations go to a pool inside the game's bss (port/snapmem_3ds.c),
# so a save state snapshot carries them
s|const SND_HOOKS gAudioMemHooks = {malloc, free};|const SND_HOOKS gAudioMemHooks = {port_musyx_alloc, port_musyx_free};|
1i\
#include <stddef.h>\
void* port_musyx_alloc(size_t size);\
void port_musyx_free(void* p);\
#include "musyx_mix.h"\
#ifdef PORT_MUSYX_SOFT\
#define PORT_MUSYX_VOICES 48\
#else\
#define PORT_MUSYX_VOICES 21\
#endif
