# Bug 29 fix: EXPGFX_STATIC_DATA casts gExpgfxStaticData (the 0x30-byte plane
# offset table) to the GameCube DLL's whole .data blob and reads fields at
# fixed offsets past it. Only the plane offsets exist at that address here; the
# rest of the blob was split into separate globals (or never defined), so the
# reads hit whatever the linker placed next (179B5EE9: the ShadowHunter move
# tables). Every particle's quad corners come from the templates at +0x150 /
# +0x168: zeros = invisible (spore cloud, torch sparkles), other data = huge
# spinning needles. expgfx_resetAllPools also wrote -1 / 0 over the 240 bytes
# after it. Point every blob field at real data (templates from the original
# DOL, SFA-Decomp gExpgfxQuadTemplateA/B)
/^s16 gExpgfxStaticPoolSlotTypeIds\[80\] = {$/i\
static Vec3s sPortQuadTemplateA[4] = { { -1000, 0, 1000 }, { 1000, 0, 1000 }, { 1000, 0, -1000 }, { -1000, 0, -1000 } };\
static Vec3s sPortQuadTemplateB[4] = { { -1000, -1000, 0 }, { 1000, -1000, 0 }, { 1000, 1000, 0 }, { -1000, 1000, 0 } };\
extern char sExpgfxMismatchInAddRemove[];\
extern char sExpgfxNoTexture[11];
s|staticData->quadTemplateA;|sPortQuadTemplateA;|
s|staticData->quadTemplateB;|sPortQuadTemplateB;|
s|staticData->poolSlotTypeIds;|gExpgfxStaticPoolSlotTypeIds;|
s|staticData->poolFrameFlags;|gExpgfxStaticPoolFrameFlags;|
s|debugPrintf(staticData->noTextureString);|debugPrintf(sExpgfxNoTexture);|
s|debugPrintf(staticData->mismatchInAddRemoveString);|debugPrintf(sExpgfxMismatchInAddRemove);|
