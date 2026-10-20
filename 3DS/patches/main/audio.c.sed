# The 3DS DSP has 24 channels: 21 play MusyX voices (port/musyx_ndsp.c), the
# others the disc stream (2) and the AI DMA output (1). The GameCube version
# allows 48 voices, at most 24 of them for sound effects; keep that ratio.
s|sndInit(0x30, 0x30, 0x18, 1, 1, 0x1000000);|sndInit(PORT_MUSYX_VOICES, PORT_MUSYX_VOICES, PORT_MUSYX_VOICES / 2, 1, 1, 0x1000000);|
s|sndSetMaxVoices(0x30, 0x18);|sndSetMaxVoices(PORT_MUSYX_VOICES, PORT_MUSYX_VOICES / 2);|
# MusyX allocations go to a pool inside the game's bss (port/snapmem_3ds.c),
# so a save state snapshot carries them
s|const SND_HOOKS gAudioMemHooks = {malloc, free};|const SND_HOOKS gAudioMemHooks = {port_musyx_alloc, port_musyx_free};|
1i\
#include <stddef.h>\
void* port_musyx_alloc(size_t size);\
void port_musyx_free(void* p);\
#define PORT_MUSYX_VOICES 21
