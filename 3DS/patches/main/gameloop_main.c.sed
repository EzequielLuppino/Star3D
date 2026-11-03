# Bottom screen HUD: the minimap is drawn on the 3DS bottom screen
# (gx/gx_init.c gx_hud_begin; its text goes through port/hud_3ds.c)
1i\
extern volatile const char* g_wd_phase[3];\
extern __thread int t_thread_slot;\
#define WD_PHASE(name) (g_wd_phase[t_thread_slot] = (name))\
int gx_hud_begin(void);\
void gx_hud_end(void);\
void port_savestate_point(void);
# Save states are captured and restored between frames (port/savestate_snap.c)
s|^static void gameLoop(void) {$|static void gameLoop(void) { port_savestate_point();|
s|^\( *\)gMinimapInterface->vtable->update();$|\1{ int hudRedirect = gx_hud_begin(); gMinimapInterface->vtable->update(); if (hudRedirect) gx_hud_end(); }|
# Watchdog phase markers: name the stage the game thread is stuck in (the
# watchdog logs the phase when frames stop)
s|^\( *\)padUpdate();$|\1WD_PHASE("game: pad"); padUpdate();|
s|^\( *\)voxmaps_updateTimers();$|\1WD_PHASE("game: voxmaps"); voxmaps_updateTimers();|
s|^\( *\)gameUpdate();$|\1WD_PHASE("game: update"); gameUpdate();|
s|^\( *\)Camera_UpdateShakeAndFarPlane();$|\1WD_PHASE("game: camshake"); Camera_UpdateShakeAndFarPlane();|
s|^\( *\)doNothing_startOfFrame();$|\1WD_PHASE("game: startOfFrame"); doNothing_startOfFrame();|
s|^\( *\)loadDataFiles();$|\1WD_PHASE("game: loadDataFiles"); loadDataFiles();|
s|^\( *\)audioUpdate();$|\1WD_PHASE("game: audio"); audioUpdate();|
s|^\( *\)Sfx_UpdateLoopedObjectSounds();$|\1WD_PHASE("game: sfxLooped"); Sfx_UpdateLoopedObjectSounds();|
s|^\( *\)debugPrintDraw(0);$|\1WD_PHASE("game: debugPrintDraw"); debugPrintDraw(0);|
s|^\( *\)(\*gScreenTransitionInterface)->init(0, 0, 0);$|\1WD_PHASE("game: screenTrans"); (*gScreenTransitionInterface)->init(0, 0, 0);|
s|^\( *\)subtitleUpdateAndDraw(0);$|\1WD_PHASE("game: subtitle"); subtitleUpdateAndDraw(0);|
s|^\( *\)doNothing_endOfFrame();$|\1WD_PHASE("game: endOfFrame"); doNothing_endOfFrame();|
s|^\( *\)GXFlush_(1, 1);$|\1WD_PHASE("game: GXFlush"); GXFlush_(1, 1);|
s|^\( *\)Obj_FlushDeferredFreeList();$|\1WD_PHASE("game: deferredFree"); Obj_FlushDeferredFreeList();|
s|^\( *\)mmFreeTick(1);$|\1WD_PHASE("game: mmFreeTick"); mmFreeTick(1);|
s|^\( *\)doQueuedLoads();$|\1WD_PHASE("game: doQueuedLoads"); doQueuedLoads();|
s|^\( *\)mainLoopDoGameText();$|\1WD_PHASE("game: gameTextMain"); mainLoopDoGameText();|
s|^\( *\)uiDll_runFrameStartAndLoadNext();$|\1WD_PHASE("game: uiDllStart"); uiDll_runFrameStartAndLoadNext();|
s|^\( *\)camcontrol_setAButtonIconForTarget();$|\1WD_PHASE("game: abtnIcon"); camcontrol_setAButtonIconForTarget();|
s|^\( *\)getButtonsJustPressed(0);$|\1WD_PHASE("game: justPressed"); getButtonsJustPressed(0);|
s|^\( *\)Obj_UpdateAllObjects(timeStop);$|\1WD_PHASE("game: objects"); Obj_UpdateAllObjects(timeStop);|
s|^\( *\)updateEnvironment(0);$|\1WD_PHASE("game: env"); updateEnvironment(0);|
s|^\( *\)(\*gMapEventInterface)->updateTimes();$|\1WD_PHASE("game: mapTimes"); (*gMapEventInterface)->updateTimes();|
s|^\( *\)waterFxUpdate(timeDelta);$|\1WD_PHASE("game: waterFx"); waterFxUpdate(timeDelta);|
s|^\( *\)uiDll_runFrameEndAndLoadNext();$|\1WD_PHASE("game: uiDllEnd"); uiDll_runFrameEndAndLoadNext();|
s|^\( *\)trackIntersect();$|\1WD_PHASE("game: trackIntersect"); trackIntersect();|
s|^\( *\)mapUpdateCameraPosByTransformSpace();$|\1WD_PHASE("game: mapCamPos"); mapUpdateCameraPosByTransformSpace();|
s|^\( *\)doPendingMapLoads();$|\1WD_PHASE("game: pendingMapLoads"); doPendingMapLoads();|
s|^\( *\)Obj_ApplyPendingParentLinks();$|\1WD_PHASE("game: parentLinks"); Obj_ApplyPendingParentLinks();|
s|^\( *\)(\*gCheckpointInterface)->onGameLoop();$|\1WD_PHASE("game: checkpoint"); (*gCheckpointInterface)->onGameLoop();|
s|^\( *\)resetSomeGxFlags();$|\1WD_PHASE("game: gxFlags"); resetSomeGxFlags();|
s|^\( *\)sceneRender(0, 0, 0, 0, 0, 0);$|\1WD_PHASE("game: scene"); sceneRender(0, 0, 0, 0, 0, 0);|
s|^\( *\)gScreensInterface->vtable->run(0);$|\1WD_PHASE("game: screens"); gScreensInterface->vtable->run(0);|
s|^\( *\)curUiDllDraw(0, 0, 0, 0);$|\1WD_PHASE("game: uiDllDraw"); curUiDllDraw(0, 0, 0, 0);|
s|^\( *\)dvdCheckError();$|\1WD_PHASE("game: dvdCheckError"); dvdCheckError();|
s|^\( *\)gameTextRun();$|\1WD_PHASE("game: gameText"); gameTextRun();|
s|^\( *\)Camera_ApplyCurrentViewport(0);$|\1WD_PHASE("game: viewport"); Camera_ApplyCurrentViewport(0);|
