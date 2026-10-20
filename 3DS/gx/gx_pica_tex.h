#ifndef GX_PICA_TEX_H
#define GX_PICA_TEX_H

// Header of textures converted at build time (tools/native/texconv.c, which
// has its own copy of this layout). It sits at the start of the texture's
// pixel data, i.e. at the pointer the game passes to GXInitTexObj.

#include <3ds/types.h>
#include <string.h>

#define PICA_TEX_MAGIC 0x31543350u  // "P3T1"
#define PICA_TEX_KEY   0x51A3F0C9u

#define PICA_TEXF_PADDED    0x01  // Image covers uscale x vscale of the texture
#define PICA_TEXF_RESAMPLED 0x02

typedef struct {
    u32 magic, key;
    u16 width, height;  // Stored (power-of-two) size
    u8 format;          // GPU_TEXCOLOR
    u8 levels;          // Mip levels (1 = none)
    u8 flags;
    u8 swizzle;
    float uscale, vscale;
    u32 data_offset;    // From this header to level 0
    u32 data_size;      // All levels
} PicaTexHeader;

_Static_assert(sizeof(PicaTexHeader) == 32, "PicaTexHeader layout");

static inline const PicaTexHeader* pica_tex_header(const void* data) {
    const PicaTexHeader* h = (const PicaTexHeader*)data;
    if (!data || ((uintptr_t)data & 3)) return NULL;
    if (h->magic != PICA_TEX_MAGIC || h->key != PICA_TEX_KEY) return NULL;
    if (h->width < 8 || h->height < 8 || h->width > 1024 || h->height > 1024 || h->levels == 0 || h->levels > 8)
        return NULL;
    return h;
}

#endif // GX_PICA_TEX_H
