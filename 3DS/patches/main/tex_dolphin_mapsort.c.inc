
/* Map sort (S3D_MAP_SORT, port/mapsort_3ds.c, DRAWCALL_PLAN.md "Map sort").
 * Appended to tex_dolphin.c by patches/main/tex_dolphin.c.sed.
 *
 * renderSceneGeometry(0) draws the opaque map block by block, and every block
 * draws its pieces material by material, so consecutive draws almost never
 * share a material (Azahar hub: 205 calls, 69 materials, ~1 repeat). Every
 * switch makes the render thread set up its state again. Here the visible
 * blocks are collected (mapSortAdd) and drawn together at the end of the pass
 * (mapSortFlush), grouped by material:
 * - A block's main stream is split into units at its shader ops. A unit is
 *   one shader op and the ops up to the next one, and it is replayed exactly
 *   like renderMapBlock would (setShader, vertex formats, display lists). A
 *   shader op whose setup returns early (flags 0x80000000, 0x20000) inherits
 *   the state before it, so it stays glued to the unit before it.
 * - Order rules inside a block: an "ordered" unit (no key: blended, heat
 *   shimmer, inheriting; alpha-tested, flag 0x400; or with a glued part)
 *   keeps its place against every other unit of its block. Plain opaque
 *   units may swap among themselves only with bit 4; without it the whole
 *   block keeps its order. Across blocks the order is free (opaque, depth
 *   tested; blocks are drawn front to back only for early z).
 * - Greedy merge: units without a key are drawn as soon as they may be; then
 *   the material with the most units that may be drawn now is drawn, all of
 *   them, in block order. Repeat.
 * - A unit whose block differs from the last unit's gets the block setup
 *   (matrices and vertex arrays) again. A unit that needs the vertex format of
 *   an earlier unit of its block replays that op first.
 * - Bit 2 also skips a unit's setShader when the unit drawn just before used
 *   a shader with the same content and added nothing to it (no point-light
 *   stages, no glued unit, no flags 0x800/0x1000/0x2000): the GX state is then
 *   already what setShader would set.
 */
int port_map_sort(void);
void port_map_sort_note(int what, int n);
extern f32 lbl_803DCE58;
extern f32 lbl_803DCE54;

#define MS_BLOCKS 96
#define MS_UNITS 1024
#define MS_KEYS 512

typedef struct MsBlock {
    MapBlockData* block;
    f32 rowF;
    f32 colF;
    f32 m[16];
    u16 first;
    u16 count;
    u16 head;     /* first undrawn unit */
    u16 barrier;  /* first undrawn ordered unit (first + count = none) */
} MsBlock;

typedef struct MsUnit {
    s32 bit;          /* first op of the unit */
    s32 endBit;       /* first op of the next unit, -1 = runs to the end op */
    s32 shaderBit;    /* the unit's own shader op, -1 = none */
    s32 vtxBit;       /* vertex format op to replay first, -1 = none */
    Shader* vtxShader;
    Shader* key;      /* NULL = never grouped */
    u32 hash;
    u8 clean;
    u8 ordered;
    u8 drawn;
} MsUnit;

static MsBlock sMsBlocks[MS_BLOCKS];
static MsUnit sMsUnits[MS_UNITS];
static int sMsBlockCount;
static int sMsUnitCount;
static MsBlock* sMsCurBlock;
static Shader* sMsLastKey;
static u8 sMsLastClean;
static struct {
    u32 hash;
    Shader* key;
    u16 n;
    u16 stamp;
} sMsKeys[MS_KEYS];
static u16 sMsStamp;

static u32 msShaderHash(Shader* s) {
    u32 h = 2166136261u;
    const u8* p;
    int i;
    int n;
    h = (h ^ s->flags) * 16777619u;
    h = (h ^ s->layerCount) * 16777619u;
    p = (const u8*)s->layers;
    n = s->layerCount * (int)sizeof(ShaderLayer);
    for (i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}

static int msSameShader(Shader* a, Shader* b) {
    if (a == b) return 1;
    if (a->flags != b->flags || a->layerCount != b->layerCount) return 0;
    return memcmp(a->layers, b->layers, a->layerCount * sizeof(ShaderLayer)) == 0;
}

/* Key of a unit led by shader s, NULL if it must not be grouped */
static Shader* msKey(Shader* s) {
    u32 f = SHADER_FLAGS(s);
    if (s->layerCount > 2) return NULL;
    if (f & (0x80000000u | 0x40000000u | 0x20000000u | 0x20000u | 0x80u)) return NULL;
    return s;
}

static int msCleanFlags(Shader* s) {
    return (SHADER_FLAGS(s) & (0x800u | 0x1000u | 0x2000u)) == 0;
}

static u32 msRead(ModelRenderInstrsState* st, int bits) {
    int pos = st->bit;
    u8* p = st->instrs + (pos >> 3);
    u32 w = p[0] | (p[1] << 8) | (p[2] << 16);
    st->bit = pos + bits;
    return (w >> (pos & 7)) & ((1u << bits) - 1);
}

static void msUnitStart(MsUnit* u, s32 bit, Shader* s) {
    u->bit = bit;
    u->endBit = -1;
    u->shaderBit = s ? bit : -1;
    u->vtxBit = -1;
    u->vtxShader = NULL;
    u->key = s ? msKey(s) : NULL;
    u->hash = u->key ? msShaderHash(s) : 0;
    u->clean = u->key != NULL && msCleanFlags(s);
    u->ordered = u->key == NULL || (SHADER_FLAGS(s) & 0x400u) != 0;
    u->drawn = 0;
}

/* Splits the block's main stream into units. Returns 0 if they don't fit */
static int msParse(MsBlock* b) {
    ModelRenderInstrsState st;
    MapBlockData* block = b->block;
    MsUnit* u;
    Shader* cur = NULL;
    s32 lastVtx = -1;
    Shader* lastVtxShader = NULL;
    int sawShader = 0;
    int vtxInUnit = 0;
    int drew = 0;
    int start = sMsUnitCount;
    int guard = 0;
    int nbits = block->nRenderInstrsMain << 3;
    int i;

    if (sMsUnitCount >= MS_UNITS) return 0;
    modelRenderInstrsState_init(&st, block->renderInstrsMain, nbits, nbits);
    u = &sMsUnits[sMsUnitCount++];
    msUnitStart(u, 0, NULL);
    u->ordered = 1;
    for (;;) {
        s32 pos = st.bit;
        u32 op;
        if (++guard > 4096 || pos >= nbits) break;
        op = msRead(&st, 4);
        if (op == 5) break;
        if (op == 1) {
            Shader* s = &block->shaders[msRead(&st, 6)];
            u32 f = SHADER_FLAGS(s);
            if (sawShader && !(f & (0x80000000u | 0x20000u))) {
                if (sMsUnitCount >= MS_UNITS) {
                    sMsUnitCount = start;
                    return 0;
                }
                u->endBit = pos;
                u = &sMsUnits[sMsUnitCount++];
                msUnitStart(u, pos, s);
                u->vtxBit = lastVtx;
                u->vtxShader = lastVtxShader;
                vtxInUnit = 0;
            } else if (!sawShader) {
                /* The first unit starts at the stream start (vertex format
                 * ops may come before its shader op) */
                if (!drew && !(f & (0x80000000u | 0x20000u))) {
                    msUnitStart(u, 0, s);
                    u->shaderBit = pos;
                }
            } else {
                /* Glued: draws with the state the unit's own shader left */
                u->clean = 0;
                u->ordered = 1;
                if (f & 0x80u) u->key = NULL;
            }
            sawShader = 1;
            cur = s;
        } else if (op == 3) {
            lastVtx = pos;
            lastVtxShader = cur;
            vtxInUnit = 1;
            st.bit += 3;
        } else if (op == 2) {
            /* A unit drawing before its own vertex format op needs the last one */
            if (!vtxInUnit && u != &sMsUnits[start] && u->vtxBit >= 0) u->vtxBit |= 0x40000000;
            vtxInUnit = 1;
            drew = 1;
            st.bit += 8;
        } else if (op == 4) {
            st.bit += 8 * msRead(&st, 4);
        }
    }
    /* Keep the replayed vertex format only where it is needed (bit 30 set) */
    for (i = start; i < sMsUnitCount; i++) {
        MsUnit* v = &sMsUnits[i];
        v->vtxBit = (v->vtxBit >= 0 && (v->vtxBit & 0x40000000)) ? (v->vtxBit & ~0x40000000) : -1;
    }
    b->first = (u16)start;
    b->count = (u16)(sMsUnitCount - start);
    b->head = (u16)start;
    return 1;
}

extern int sMsLit;

static void msDrawUnit(MsBlock* b, MsUnit* u, int mode) {
    ModelRenderInstrsState st;
    MapBlockData* block = b->block;
    Shader* shader = NULL;
    int skip;
    int nbits = block->nRenderInstrsMain << 3;

    if (sMsCurBlock != b) {
        lbl_803DCE58 = b->rowF;
        lbl_803DCE54 = b->colF;
        setupToRenderMapBlock(block, b->m);
        sMsCurBlock = b;
        port_map_sort_note(2, 1);
    }
    modelRenderInstrsState_init(&st, block->renderInstrsMain, nbits, nbits);
    if (u->vtxBit >= 0) {
        st.bit = u->vtxBit + 4;
        mapBlockRender_setVtxDcrs(1, block, u->vtxShader, &st);
        port_map_sort_note(4, 1);
    }
    skip = (mode & 2) && u->key != NULL && sMsLastKey != NULL && sMsLastClean && u->clean &&
           msSameShader(u->key, sMsLastKey);
    sMsLit = 0;
    st.bit = u->bit;
    for (;;) {
        s32 pos = st.bit;
        u32 op;
        if (u->endBit >= 0 && pos >= u->endBit) break;
        if (pos >= nbits) break;
        op = msRead(&st, 4);
        if (op == 5) break;
        switch (op) {
        case 3:
            mapBlockRender_setVtxDcrs(1, block, shader, &st);
            break;
        case 1:
            if (skip && pos == u->shaderBit) {
                shader = &block->shaders[msRead(&st, 6)];
                port_map_sort_note(3, 1);
            } else {
                shader = mapBlockRender_setShader(1, block, &st);
            }
            break;
        case 2:
            mapBlockRender_callList(1, 0, block, shader, &st, b->m);
            break;
        case 4:
            st.bit += 8 * msRead(&st, 4);
            break;
        }
    }
    u->drawn = 1;
    sMsLastKey = u->key;
    sMsLastClean = u->clean && !sMsLit;
}

/* Moves head and barrier past drawn units */
static void msAdvance(MsBlock* b) {
    int end = b->first + b->count;
    while (b->head < end && sMsUnits[b->head].drawn)
        b->head++;
    if (b->barrier < b->head) b->barrier = b->head;
    while (b->barrier < end && (sMsUnits[b->barrier].drawn || !sMsUnits[b->barrier].ordered))
        b->barrier++;
}

/* May unit i of block b be drawn now? */
static int msReady(MsBlock* b, int i, int relaxed) {
    MsUnit* u = &sMsUnits[i];
    if (u->drawn) return 0;
    if (!relaxed || u->ordered) return i == b->head;
    return i < b->barrier;
}

/* Called for each visible block of pass 0 instead of renderMapBlock */
void mapSortAdd(MapBlockData* block, f32 rowF, f32 colF) {
    MsBlock* b;
    if (block->nRenderInstrsMain == 0) return;
    if (sMsBlockCount >= MS_BLOCKS) {
        port_map_sort_note(5, 1);
        renderMapBlock(block, 0);
        return;
    }
    b = &sMsBlocks[sMsBlockCount];
    b->block = block;
    b->rowF = rowF;
    b->colF = colF;
    PSMTXConcat((MtxPtr)Camera_GetViewMatrix(), block->transform, (MtxPtr)b->m);
    if (!msParse(b)) {
        port_map_sort_note(5, 1);
        renderMapBlock(block, 0);
        return;
    }
    sMsBlockCount++;
}

void mapSortFlush(void) {
    int mode = port_map_sort();
    int relaxed = (mode & 4) != 0;
    int remaining = sMsUnitCount;
    int rounds = 0;
    int i;
    int j;

    sMsCurBlock = NULL;
    sMsLastKey = NULL;
    sMsLastClean = 0;
    port_map_sort_note(0, sMsUnitCount);
    port_map_sort_note(6, sMsBlockCount);
    for (i = 0; i < sMsBlockCount; i++) {
        sMsBlocks[i].barrier = sMsBlocks[i].first;
        msAdvance(&sMsBlocks[i]);
    }
    /* Census: distinct shader contents among the keyed units, units without
     * a key, ordered units. Groups close to the distinct count = the order
     * rules don't hold the merge back */
    sMsStamp++;
    for (i = 0; i < sMsUnitCount; i++) {
        MsUnit* u = &sMsUnits[i];
        if (u->ordered) port_map_sort_note(9, 1);
        if (u->key == NULL) {
            port_map_sort_note(7, 1);
            continue;
        }
        for (j = u->hash % MS_KEYS;; j = (j + 1) % MS_KEYS) {
            if (sMsKeys[j].stamp != sMsStamp) {
                sMsKeys[j].stamp = sMsStamp;
                sMsKeys[j].hash = u->hash;
                sMsKeys[j].key = u->key;
                port_map_sort_note(8, 1);
                break;
            }
            if (sMsKeys[j].hash == u->hash && msSameShader(sMsKeys[j].key, u->key)) break;
        }
    }
    while (remaining > 0) {
        Shader* best = NULL;
        u32 bestHash = 0;
        int bestN = 0;
        int progress;
        /* Units without a key go as soon as they may */
        do {
            progress = 0;
            for (i = 0; i < sMsBlockCount; i++) {
                MsBlock* b = &sMsBlocks[i];
                for (j = b->head; j < b->first + b->count; j++) {
                    if (sMsUnits[j].key != NULL || !msReady(b, j, relaxed)) continue;
                    msDrawUnit(b, &sMsUnits[j], mode);
                    remaining--;
                    progress = 1;
                    msAdvance(b);
                }
            }
        } while (progress);
        /* The material with the most units that may be drawn now (front
         * blocks win ties) */
        sMsStamp++;
        for (i = 0; i < sMsBlockCount; i++) {
            MsBlock* b = &sMsBlocks[i];
            for (j = b->head; j < b->first + b->count; j++) {
                MsUnit* u = &sMsUnits[j];
                int k;
                if (u->key == NULL || !msReady(b, j, relaxed)) continue;
                for (k = u->hash % MS_KEYS;; k = (k + 1) % MS_KEYS) {
                    if (sMsKeys[k].stamp != sMsStamp) {
                        sMsKeys[k].stamp = sMsStamp;
                        sMsKeys[k].hash = u->hash;
                        sMsKeys[k].key = u->key;
                        sMsKeys[k].n = 0;
                    } else if (sMsKeys[k].hash != u->hash || !msSameShader(sMsKeys[k].key, u->key)) {
                        continue;
                    }
                    if (++sMsKeys[k].n > bestN) {
                        bestN = sMsKeys[k].n;
                        best = sMsKeys[k].key;
                        bestHash = u->hash;
                    }
                    break;
                }
            }
        }
        if (best == NULL) break;
        rounds++;
        /* Draw them all; drawing one can free the next of the same material */
        do {
            progress = 0;
            for (i = 0; i < sMsBlockCount; i++) {
                MsBlock* b = &sMsBlocks[i];
                for (j = b->head; j < b->first + b->count; j++) {
                    MsUnit* u = &sMsUnits[j];
                    if (u->key == NULL || u->hash != bestHash || !msReady(b, j, relaxed) ||
                        !msSameShader(u->key, best))
                        continue;
                    msDrawUnit(b, u, mode);
                    remaining--;
                    progress = 1;
                    msAdvance(b);
                }
            }
        } while (progress);
    }
    if (remaining > 0) port_map_sort_note(10, remaining);
    port_map_sort_note(1, rounds);
    sMsBlockCount = 0;
    sMsUnitCount = 0;
    sMsCurBlock = NULL;
}
