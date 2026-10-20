# Indirect fallback counters: GXSetTevIndirect calls are counted per calling
# function (gx_tev.c port_tev_indirect)
1i\
#include "dolphin/gx/GXBump.h"\
void port_tev_indirect(const char* func, GXTevStageID tev_stage, GXIndTexStageID ind_stage, GXIndTexFormat format,\
                       GXIndTexBiasSel bias_sel, GXIndTexMtxID matrix_sel, GXIndTexWrap wrap_s, GXIndTexWrap wrap_t,\
                       GXBool add_prev, GXBool ind_lod, GXIndTexAlphaSel alpha_sel);\
void port_fur_wind(float m[3][4], float fz, float sx, float sy);
s|\bGXSetTevIndirect(|port_tev_indirect(__func__, |g
# Fur wind replaces the noise warp (port/fur_3ds.c)
s|GXLoadTexMtxImm(mtx4, GX_PTTEXMTX0, GX_MTX3x4);|port_fur_wind(mtx4, fz, sx, sy); &|
# Bug 27: renderOpMatrix reads the joint texture matrix IDs at
# gObjGxPosMtxIdTable + 0xc, where the GameCube's .data had
# gObjGxTexMtxIdTable (TEXMTX0-9). This build places that table before it, so
# the IDs came from the next variables (0xFF, 0xFD, ...) and gx dropped every
# per-joint GXLoadTexMtxImm: sphere-map lighting (Fox's fur and body, other
# characters) used a stale TEXMTX0. S3D_JOINT_TEXMTX=0 (live) restores the
# old IDs for A/B. port_joint_texmtx_ids is defined before renderOpMatrix below
s|texMtxIds = posMtxIds\[0\] + 0xc;|texMtxIds = port_joint_texmtx_ids();|
# Fur shell budget and per-layer shade (port/fur_3ds.c port_fur_step,
# port_fur_shade): objRenderFuzz steps over layers, scaling gObjFuzzStep so
# the shells still span the full height; inner shells get a darker konst
1a\
int port_fur_step(int n, int cnt);\
unsigned char port_fur_shade(int layer);
s|^        for (gObjFuzzLayerIndex = 0; gObjFuzzLayerIndex < n; gObjFuzzLayerIndex++) {$|        int fhStep = port_fur_step(n, cnt);\
        gObjFuzzStep *= fhStep;\
        for (gObjFuzzLayerIndex = 0; fhStep > 0 \&\& gObjFuzzLayerIndex < n; gObjFuzzLayerIndex += fhStep) {|
/^int objFuzzRenderCb(/,/^}/{
/^    if (lbl_803DCC36 == 0) {$/{
n
s|^        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);$|        {\
            GXColor fhShade;\
            fhShade.r = fhShade.g = fhShade.b = fhShade.a = port_fur_shade(gObjFuzzLayerIndex);\
            GXSetTevKColor(GX_KCOLOR1, fhShade);\
            GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K1);\
        }\
        GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_ZERO);|
}
}
# Fur noise over projected light (GRAPHICS_PLAN.md 4.4, port/fur_3ds.c
# port_fur_proj_light): with the light texture the shell needs four textures
# and the noise mask is the one dropped, so the shells turn solid
1a\
int port_fur_proj_light(void);
/^int objFuzzRenderCb(/,/^}/s|^        fancy = 1;$|        fancy = port_fur_proj_light();|
# Fur sphere map read as white on stage 1 (port/fur_3ds.c port_fur_sphere)
1a\
int port_fur_sphere(void);
/^int objFuzzRenderCb(/,/^}/s|GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C2, GX_CC_RASC);|GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, (GXTevColorArg)port_fur_sphere(), GX_CC_C2, GX_CC_RASC);|
# Fur noise stage without the bump mask (port/fur_3ds.c port_fur_bump): the
# game's no-mask branch frees map2's unit for the sphere map. The Docker
# decomp spells the test rop->indTexture != NULL, so both forms match
1a\
int port_fur_bump(void);
/^int objFuzzRenderCb(/,/^}/{
/^    if (rop->indTexture\(Reference != 0\| != NULL\)) {$/{
N
s|^    if (\(rop->indTexture[^)]*\)) {\n        GXSetTevOrder(stage + 1, GX_TEXCOORD4, GX_TEXMAP3, GX_ALPHA_BUMPN);|    if (\1 \&\& port_fur_bump()) {\n        GXSetTevOrder(stage + 1, GX_TEXCOORD4, GX_TEXMAP3, GX_ALPHA_BUMPN);|
}
}
# Gameplay fur for the player (port/fur_3ds.c port_fur_gameplay)
1a\
int port_fur_gameplay(int strong, int classId, float dist, float scale, int slot, unsigned flags, int* cnt, int n);
/^void objRenderFuzz(/,/^}/s|^    n = 16 - cnt;$|    n = port_fur_gameplay(strong, obj->anim.classId, dist, obj->anim.hitboxScale * obj->anim.rootMotionScale,\
                          obj->anim.mapEventSlot, obj->objectFlags, \&cnt, 16 - cnt);\
    port_fur_mark(obj);|
# Animation LOD and frozen plants (port/lod_3ds.c, S3D_ANIM_LOD, S3D_PLANTS).
# port_skin_skip: a model that blends vertices keeps last frame's vertices (no
# toggle, no re-blend) while its joint matrices, hit spheres and callbacks
# still update. port_anim_skip: a model that doesn't keeps last frame's joint
# matrices (far, standing still, no bone callback, no root motion, no fur;
# port_fur_mark records fur from objRenderFuzz)
1a\
int port_skin_skip(const void* obj, int dllId, const void* am);\
int port_anim_skip(const void* obj, int dllId, const void* am, const float* wm);\
void port_fur_mark(const void* obj);
/^    did = 0;$/,/bufferFlags |= 8;$/{
s|^    did = 0;$|    did = 0;\
    int fhSkinSkip = 0;|
s|^        ObjModel_ToggleVertexBuffer((ObjModel\*)am);$|        fhSkinSkip = ((ModelFileHeader*)m)->vertexAnimEntries != NULL \&\& ((ModelFileHeader*)m)->morphTargetCount == 0 \&\&\
                     port_skin_skip(obj, obj->anim.modelInstance ? obj->anim.modelInstance->dllId : -1, am);\
        if (!fhSkinSkip) ObjModel_ToggleVertexBuffer((ObjModel*)am);|
s|^            if (did != 0) {$|            if (did != 0 \&\& !fhSkinSkip) {|
s|^                ObjModel_UpdateAnimMatrices((ObjModel\*)am, (ModelFileHeader\*)m, obj, wm);$|                if (obj->afterBonesCallback != NULL \|\| (((ObjAnimState*)((ObjModel*)am)->animStateA)->moveControlFlags \& 4) \|\|\
                    !port_anim_skip(obj, obj->anim.modelInstance ? obj->anim.modelInstance->dllId : -1, am, wm))\
                    ObjModel_UpdateAnimMatrices((ObjModel*)am, (ModelFileHeader*)m, obj, wm);|
}
# Model path census (port/lod_3ds.c port_model_path, DRAWCALL_PLAN.md "Plant
# batching"): batchable path (flag 0x8000) vs the full path, plants vs others
1a\
void port_model_path(int dllId, int alt);
/^void objRenderModel(GameObject\* obj) {$/,/^}/s|^        if (m0->flags \& 0x8000) {$|        port_model_path(obj->anim.modelInstance ? obj->anim.modelInstance->dllId : -1, (m0->flags \& 0x8000) != 0);\
&|
# Fast fur shells (port/fur_3ds.c port_fur_fast, FUR_PLAN.md "Fast shells").
# In objRenderFuzz's loop a render op without the fur flag (0x200) draws
# nothing, so its material setup, vertex format and matrix loads are deferred
# and applied only before the next fur op / fur draw or at the loop end.
# Projection, lights, fog and arrays are set once per object.
# modelDoRenderInstrs becomes a wrapper that sets fhCur for passMask 4 only.
/^static void renderOpMatrix(ModelFileHeader\* hdr, /i\
extern u8 gObjGxTexMtxIdTable[12];\
int cfg_int(const char* name, int def);\
unsigned cfg_gen(void);\
static u8* port_joint_texmtx_ids(void) {\
    static u8 dropped[10] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };\
    static int on = -1;\
    static unsigned gen;\
    if (on < 0 || gen != cfg_gen()) {\
        gen = cfg_gen();\
        on = cfg_int("S3D_JOINT_TEXMTX", 1);\
    }\
    return on ? gObjGxTexMtxIdTable : dropped;\
}\
int port_fur_fast(void);\
void port_fur_mtx(int kind, const void* m, u32 id, int type, int defer);\
u32 port_fur_mtx_mark(void);\
void port_fur_mtx_flush(u32 below);\
static int fhFast;\
static int fhShell;\
static int fhMute;\
static int fhMatPending;\
static u32 fhMatGen;\
static MtxBitStream fhMatBs;\
static GameObject* fhMatObj;\
static u8* fhMatM;\
static int* fhMatAm;\
static int fhDescPending;\
static MtxBitStream fhDescBs;\
static u8* fhDescM;\
static u8* fhDescOp;\
static ModelRenderOpTextureRefs* fhDescRefs;\
static void furGXLoadPosMtxImm(const void* mt, u32 id) {\
    if (fhCur & 1) port_fur_mtx(0, mt, id, 0, !gObjFuzzPassActive);\
    else GXLoadPosMtxImm(mt, id);\
}\
static void furGXLoadNrmMtxImm(const void* mt, u32 id) {\
    if (fhCur & 1) port_fur_mtx(1, mt, id, 0, !gObjFuzzPassActive);\
    else GXLoadNrmMtxImm(mt, id);\
}\
static void furGXLoadTexMtxImm(const void* mt, u32 id, GXTexMtxType type) {\
    if (fhCur & 1) port_fur_mtx(2, mt, id, type, !gObjFuzzPassActive);\
    else GXLoadTexMtxImm(mt, id, type);\
}\
static void furGXClearVtxDesc(void) {\
    if (!fhMute) GXClearVtxDesc();\
}\
static void furGXSetVtxDesc(GXAttr attr, GXAttrType type) {\
    if (!fhMute) GXSetVtxDesc(attr, type);\
}\
static void furGXSetCurrentMtx(u32 id) {\
    if (!fhMute) GXSetCurrentMtx(id);\
}\

/^static void renderOpMatrix(/,/^}/s/\bGXLoad\(Pos\|Nrm\|Tex\)MtxImm(/furGXLoad\1MtxImm(/g
/^static void modelRenderFn_setVtxDescr(/,/^}/{
s/\bGXClearVtxDesc(/furGXClearVtxDesc(/g
s/\bGXSetVtxDesc(/furGXSetVtxDesc(/g
s/\bGXSetCurrentMtx(/furGXSetCurrentMtx(/g
}
/^void modelDoRenderInstrs(GameObject\* obj, GameObject\* obj2, u8\* m, u8 passMask) {$/i\
static void fhMaterialize(void) {\
    void* cb;\
    MtxBitStream b;\
    if (!fhMatPending) return;\
    fhMatPending = 0;\
    fhFurValid = 0;\
    port_fur_mtx_flush(fhMatGen);\
    cb = ObjModel_GetRenderCallback((ObjModel*)fhMatAm);\
    b = fhMatBs;\
    ObjModel_SetRenderCallback((u8*)fhMatAm, NULL);\
    objSetupRenderOpGxState(fhMatObj, fhMatM, fhMatAm, &b);\
    ObjModel_SetRenderCallback((u8*)fhMatAm, cb);\
    port_fur_fast_note(1);\
}\
static void fhDescMaterialize(void) {\
    u8 nrm;\
    u8 tex;\
    MtxBitStream b;\
    if (!fhDescPending) return;\
    fhDescPending = 0;\
    b = fhDescBs;\
    modelRenderFn_setVtxDescr(fhDescM, fhDescOp, fhDescRefs, &b, 4, &nrm, &tex);\
    port_fur_fast_note(3);\
}\
static u32 fhSetupOp(GameObject* obj, u8* m, int* am, MtxBitStream* bs) {\
    u32 idx;\
    u8* p;\
    if (!(fhCur & 2)) {\
        port_fur_mtx_flush(~0u);\
        return objSetupRenderOpGxState(obj, m, am, bs);\
    }\
    p = bs->data + (bs->pos >> 3);\
    idx = ((p[0] | (p[1] << 8) | (p[2] << 16)) >> (bs->pos & 7)) & 0x3f;\
    if (ObjModel_GetRenderOp((ModelFileHeader*)m, idx)->flags & 0x200) {\
        fhMaterialize();\
        return objSetupRenderOpGxState(obj, m, am, bs);\
    }\
    fhMatBs = *bs;\
    fhMatObj = obj;\
    fhMatM = m;\
    fhMatAm = am;\
    fhMatPending = 1;\
    fhMatGen = port_fur_mtx_mark();\
    bs->pos += 6;\
    gObjFuzzPassActive = 0;\
    port_fur_fast_note(0);\
    return idx;\
}\
static void fhLoopBegin(void) {\
    fhFast = port_fur_fast();\
    fhShell = 0;\
    fhMatPending = 0;\
    fhDescPending = 0;\
}\
static void fhLoopEnd(void) {\
    if (fhFast) {\
        fhMaterialize();\
        fhDescMaterialize();\
        port_fur_mtx_flush(~0u);\
    }\
    fhFast = 0;\
    fhFurValid = 0;\
}\
static void fhDoRenderInstrs(GameObject* obj, GameObject* obj2, u8* m, u8 passMask);\
void modelDoRenderInstrs(GameObject* obj, GameObject* obj2, u8* m, u8 passMask) {\
    int saved = fhCur;\
    fhCur = passMask == 4 ? fhFast : 0;\
    fhFurValid = 0;\
    fhDoRenderInstrs(obj, obj2, m, passMask);\
    fhFurValid = 0;\
    fhCur = saved;\
}
/^void modelDoRenderInstrs(GameObject\* obj, GameObject\* obj2, u8\* m, u8 passMask) {$/,/^}$/{
s/\bGXLoad\(Pos\|Nrm\|Tex\)MtxImm(/furGXLoad\1MtxImm(/g
s/^\( *\)idx = objSetupRenderOpGxState(obj, m, am, &bs);$/\1idx = fhCur ? fhSetupOp(obj, m, am, \&bs) : objSetupRenderOpGxState(obj, m, am, \&bs);/
/^            modelRenderFn_setVtxDescr(m, (u8\*)op, refs, &bs, passMask, &o9, &o8);$/c\
            if ((fhCur & 4) && !gObjFuzzPassActive) {\
                fhDescBs = bs;\
                fhDescM = m;\
                fhDescOp = (u8*)op;\
                fhDescRefs = refs;\
                fhDescPending = 1;\
                fhMute = 1;\
                modelRenderFn_setVtxDescr(m, (u8*)op, refs, &bs, passMask, &o9, &o8);\
                fhMute = 0;\
                port_fur_fast_note(2);\
            } else {\
                if (fhCur) fhDescPending = 0;\
                modelRenderFn_setVtxDescr(m, (u8*)op, refs, &bs, passMask, &o9, &o8);\
            }
/^                GXCallDisplayList(dl->dlist, dl->dlistSize);$/i\
                if (fhCur) {\
                    fhDescMaterialize();\
                    port_fur_mtx_flush(~0u);\
                }
/^        objFuzzSetupGxState(obj);$/{
n
s/^    } else {$/    } else if (!((fhCur \& 8) \&\& fhShell > 0)) {/
}
/^    GXSetArray(GX_VA_POS, /i\
    if (!((fhCur & 8) && fhShell > 0)) {
/^    GXSetArray(GX_VA_TEX1, /a\
    }
}
s/^void modelDoRenderInstrs(GameObject\* obj, GameObject\* obj2, u8\* m, u8 passMask) {$/static void fhDoRenderInstrs(GameObject* obj, GameObject* obj2, u8* m, u8 passMask) {/
/^void objRenderFuzz(/,/^}/{
/^        ObjModel_SetRenderCallback((u8\*)model, objFuzzRenderCb);$/a\
        fhLoopBegin();
/^            modelDoRenderInstrs(.*, 4);$/a\
            fhShell++;
/^        curObjMtx = 0;$/i\
        fhLoopEnd();
}
# Fur shading once per layer (port/fur_3ds.c S3D_FUR_FAST bit 16, FUR_PLAN.md
# "Shading once per layer"). objFuzzRenderCb sets ~60 TEV, texture and pixel
# commands for every fur op, and in one shell they are the same for all fur
# ops of the model except the base, sphere-map and indirect textures. While
# nothing but matrix loads and vertex formats ran since the last fur op's full
# setup (fhFurValid: cleared at each shell, by a non-fur op's material and by
# a deferred one run late), the next fur op of the same kind only binds the
# textures that differ. The key holds what picks the setup's branches:
# indirect texture, bump mask and the model's fog flag
/^int objFuzzRenderCb(/i\
void port_fur_fast_note(int what);\
static int fhCur;\
static int fhFurValid;\
static void* fhFurTex[3];\
static int fhFurLite(ObjModel* model, Shader* rop, int ropIdx) {\
    void* t[3];\
    int key;\
    int i;\
    if (!fhCur) return 0;\
    t[0] = textureIdxToPtr((uintptr_t)((ShaderLayer*)Shader_getLayer(rop, 0))->texture);\
    t[1] = ObjModel_GetRenderOpTextureRefs(model, ropIdx)->texture0;\
    t[2] = rop->indTexture != NULL ? textureIdxToPtr((uintptr_t)rop->indTexture) : NULL;\
    key = 1 | (rop->indTexture != NULL ? 2 : 0) | (rop->indTexture != NULL && port_fur_bump() ? 4 : 0) |\
          ((model->file->flags & 0x100) ? 8 : 0);\
    if ((fhCur & 16) && fhFurValid == key) {\
        for (i = 0; i < 3; i++) {\
            if (t[i] != NULL && t[i] != fhFurTex[i]) {\
                selectTexture((Texture*)t[i], i);\
                fhFurTex[i] = t[i];\
            }\
        }\
        port_fur_fast_note(5);\
        return 1;\
    }\
    for (i = 0; i < 3; i++) fhFurTex[i] = t[i];\
    fhFurValid = key;\
    port_fur_fast_note(4);\
    return 0;\
}
/^int objFuzzRenderCb(/,/^}/{
s|^        gObjFuzzPassActive = 0;$|&\
        fhFurValid = 0;|
s|^    gObjFuzzPassActive = 1;$|&\
    if (fhFurLite(model, rop, ropIdx)) return 1;|
}
