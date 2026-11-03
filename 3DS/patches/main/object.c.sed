# Watchdog detail inside Obj_UpdateAllObjects: name the sub-stage and the last
# object whose update started, so a stall there reports exactly which object
# (defNo) or phase blocks
1i\
extern volatile const char* g_wd_phase[3];\
extern __thread int t_thread_slot;\
#define WD_PHASE(name) (g_wd_phase[t_thread_slot] = (name))\
extern volatile unsigned int g_wd_obj_defno;
s|^\( *\)trackTickDynamicSlotCooldowns();$|\1WD_PHASE("game: cooldowns"); trackTickDynamicSlotCooldowns();|
s|^\( *\)Obj_UpdateModelBlendStates();$|\1WD_PHASE("game: blendStates"); Obj_UpdateModelBlendStates();|
s|^\( *\)ObjHitReact_ResetActiveObjects(gObjCount);$|\1WD_PHASE("game: hitReactReset"); ObjHitReact_ResetActiveObjects(gObjCount);|
s|^\( *\)Obj_UpdateObject((GameObject\*)obj);$|\1g_wd_obj_defno = ((GameObject*)obj)->anim.romDefNo; Obj_UpdateObject((GameObject*)obj);|
s|^\( *\)ObjHitReact_UpdateResetObjects();$|\1WD_PHASE("game: hitReactUpdate"); ObjHitReact_UpdateResetObjects();|
s|^\( *\)ObjHits_Update(gObjCount);$|\1WD_PHASE("game: objHits"); ObjHits_Update(gObjCount);|
s|^\( *\)cb((GameObject\*)obj3);$|\1g_wd_obj_defno = ((GameObject*)obj3)->anim.romDefNo; cb((GameObject*)obj3);|
s|^\( *\)cb((GameObject\*)child);$|\1g_wd_obj_defno = ((GameObject*)child)->anim.romDefNo; cb((GameObject*)child);|
s|^\( *\)(\*gWaterfxInterface)->runFrame(framesThisStep);$|\1WD_PHASE("game: waterfxRun"); (*gWaterfxInterface)->runFrame(framesThisStep);|
s|^\( *\)(\*gModgfxInterface)->updateActiveEffects();$|\1WD_PHASE("game: modgfx"); (*gModgfxInterface)->updateActiveEffects();|
s|^\( *\)(\*gExpgfxInterface)->updateFrameState(0, framesThisStep);$|\1WD_PHASE("game: expgfx"); (*gExpgfxInterface)->updateFrameState(0, framesThisStep);|
s|^\( *\)ObjHits_TickPriorityHitCooldowns();$|\1WD_PHASE("game: hitCooldowns"); ObjHits_TickPriorityHitCooldowns();|
s|^\( *\)(\*gObjectTriggerInterface)->run();$|\1WD_PHASE("game: triggerRun"); (*gObjectTriggerInterface)->run();|
s|^\( *\)(\*gObjectTriggerInterface)->updateCamera();$|\1WD_PHASE("game: triggerCam"); (*gObjectTriggerInterface)->updateCamera();|
s|^\( *\)(\*gCameraInterface)->update(framesThisStep);$|\1WD_PHASE("game: cameraUpdate"); (*gCameraInterface)->update(framesThisStep);|
