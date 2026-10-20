#ifndef SNAPMEM_3DS_H
#define SNAPMEM_3DS_H

#include <stdbool.h>
#include <stddef.h>

// MusyX allocator hooks (gAudioMemHooks, patched in patches/main/audio.c.sed)
void* port_musyx_alloc(size_t size);
void port_musyx_free(void* p);
bool port_musyx_pool_snapshot_ok(void);  // false while a block lives outside the pool
void* port_musyx_pool_base(void);
void port_musyx_pool_log(void);

#endif
