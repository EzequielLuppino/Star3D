
/* Map material dedup (S3D_MAP_DEDUP, port/mapsort_3ds.c, DRAWCALL_PLAN.md
 * "Map material dedup"). Appended to tex_dolphin.c by
 * patches/main/tex_dolphin.c.sed, after the map sort.
 *
 * mapBlockRender_setShader records the whole material setup for every map
 * shader op, even when the GX state it sets is already in place. The HW map
 * sort test (C8DC5F71) showed that the render thread pays for replaying those
 * setter commands even when nothing changes (~9 us per setup), so here the
 * game's own draw order is kept and only the redundant parts are not recorded:
 * - bit 1: fog, blend / z / alpha compare, channel and cull, each only when
 *   its value differs from what the last map setShader of the pass set. No
 *   other code touches these between two map shader ops of pass 0 (the TEV
 *   helpers only set TEV, texture and indirect state).
 * - bit 2: the TEV / texture setup (Rcp reset, layer stages, cloud, warp and
 *   heavy fog stages) when the shader has the same content as the last one
 *   set up and no piece added stages on top since (point lights, flags
 *   0x800 / 0x1000 / 0x2000). Heat shimmer (0x80), inheriting (0x80000000)
 *   and attract movie (0x20000) shaders are never skipped.
 * Only armed inside renderSceneGeometry(0) (mapDedupBegin / mapDedupEnd in
 * lightmap.c.sed), where only map blocks draw; it starts from "unknown" each
 * pass, so state set by other code before the pass is never assumed.
 */
int port_map_dedup(void);
void port_map_dedup_note(int what, int n);
void port_map_dedup_mismatch(int what, unsigned flags, int cached, int now);
extern u32 g_gx_change_seq; /* gx_state.c, bumped by every GX state change */

static int sMdArmed;
static int sMdMode;
static s8 sMdFog;
static s8 sMdPe;
static s8 sMdCull;
static s8 sMdChan;     /* -1 unknown, 0 shader ambient + channel off, 1 shader
                          ambient + on, 2 sun colour + on */
static u32 sMdChanAmb; /* ambient colour set with it */
static Shader* sMdTev; /* last TEV setup that may be reused, NULL = none */
int sMdLit;            /* a piece added stages since (tex_dolphin.c.sed) */
/* Verify mode (bit 4): the group that would have been skipped, checked by
 * mapDedupCheck once its setters ran */
static int sMdPending = -1;
static u32 sMdPendingFlags;
static int sMdPendingVal;
static u32 sMdSeq;

/* Called right after each group's setters (tex_dolphin.c.sed) */
void mapDedupCheck(void) {
    if (sMdPending < 0) return;
    if (g_gx_change_seq != sMdSeq)
        port_map_dedup_mismatch(sMdPending, sMdPendingFlags, sMdPendingVal, sMdPendingVal);
    sMdPending = -1;
}

/* 1 = skip; in verify mode remembers the group and runs it */
static int mdSkip(int what, u32 flags, int val) {
    port_map_dedup_note(what, 1);
    if (!(sMdMode & 4)) return 1;
    sMdPending = what;
    sMdPendingFlags = flags;
    sMdPendingVal = val;
    sMdSeq = g_gx_change_seq;
    return 0;
}

void mapDedupReset(void) {
    sMdPending = -1;
    sMdFog = -1;
    sMdPe = -1;
    sMdCull = -1;
    sMdChan = -1;
    sMdTev = NULL;
    sMdLit = 0;
}

void mapDedupBegin(int renderType) {
    sMdMode = renderType == 0 ? port_map_dedup() : 0;
    sMdArmed = sMdMode != 0;
    mapDedupReset();
}

void mapDedupEnd(void) {
    sMdArmed = 0;
    mapDedupReset();
}

/* 1 = the slot already holds v (skip the setters), else remembers v */
static int mdSame(s8* slot, int v, int what, Shader* s) {
    if (!sMdArmed || !(sMdMode & 5)) return 0;
    if (*slot == v) return mdSkip(what, SHADER_FLAGS(s), v);
    *slot = (s8)v;
    return 0;
}

int mapDedupFog(Shader* s) {
    mapDedupCheck();
    return mdSame(&sMdFog, (SHADER_FLAGS(s) & 4) != 0, 0, s);
}

int mapDedupPe(Shader* s) {
    u32 f = SHADER_FLAGS(s);
    int v;
    if (f & (0x40000000u | 0x20000000u)) v = 0;
    else if ((f & 0x400u) && !(f & 0x80u)) v = 1;
    else v = 2;
    mapDedupCheck();
    return mdSame(&sMdPe, v, 1, s);
}

int mapDedupCull(Shader* s) {
    mapDedupCheck();
    return mdSame(&sMdCull, (SHADER_FLAGS(s) & 8) != 0, 3, s);
}

/* Same choice of ambient colour and channel switch as setShader */
int mapDedupChan(Shader* s) {
    u32 f = SHADER_FLAGS(s);
    u32 amb;
    int on;
    mapDedupCheck();
    if (!sMdArmed || !(sMdMode & 5)) return 0;
    if (f & (1u | 0x40000u | 0x800u | 0x1000u)) {
        amb = ((u32)gTexShaderAmbColor.r << 24) | ((u32)gTexShaderAmbColor.g << 16) |
              ((u32)gTexShaderAmbColor.b << 8) | gTexShaderAmbColor.a;
        on = (f & 0x40000u) == 0;
    } else {
        /* setShader passes 3 bytes (the alpha is whatever follows them) */
        u8 c[3];
        objGetSunColor(0, &c[0], &c[1], &c[2]);
        amb = ((u32)c[0] << 24) | ((u32)c[1] << 16) | ((u32)c[2] << 8);
        on = 2;
    }
    if (sMdChan == on && sMdChanAmb == amb) return mdSkip(2, f, on);
    sMdChan = (s8)on;
    sMdChanAmb = amb;
    return 0;
}

/* 1 = the TEV / texture setup of s is already in place */
int mapDedupTev(Shader* s) {
    u32 f = SHADER_FLAGS(s);
    int ok = sMdArmed && (sMdMode & 2) && !(sMdMode & 4) &&
             !(f & (0x80000000u | 0x20000u | 0x80u | 0x800u | 0x1000u | 0x2000u));
    port_map_dedup_note(5, 1);
    if (ok && sMdTev != NULL && !sMdLit && msSameShader(s, sMdTev)) {
        port_map_dedup_note(4, 1);
        return 1;
    }
    sMdTev = ok ? s : NULL;
    sMdLit = 0;
    return 0;
}
