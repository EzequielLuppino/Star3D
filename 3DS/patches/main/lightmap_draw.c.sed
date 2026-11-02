# Shadow decals (gx_init.c port_shadow_decals, S3D_SHADOW_DECALS): the queued
# projected-shadow meshes (queue type 2, one per shadowed object) and the
# transparent objects' cast meshes become flat ground quads
# (objDrawGroundShadow, shadow_dolphin.c): terrain-conforming (a ground
# raycast per frame, so moving objects keep their shadow on slopes), faded
# with the object's renderAlpha. Crash shadows (type 3) already use the same
# path.
1i\
_Bool port_shadow_decals(void);
s|^            objShadowRender(entries\[i\]\.arg0\.object, 0, 0, framesThisStep);$|            if (port_shadow_decals()) {\
                ObjModel\* fhModel = Obj_GetActiveModel(entries[i].arg0.object);\
                \*\(u8\*\)\(\(u8\*\)fhModel->groundShadowVerts \+ 0x18\) = 0;\
                objDrawGroundShadow(entries[i].arg0.object, fhModel);\
            } else {\
                objShadowRender(entries[i].arg0.object, 0, 0, framesThisStep);\
            }|
s|^        if (shadow != NULL && shadow->shadowCastSlot != NULL)$|        if (shadow != NULL \&\& (port_shadow_decals() ? obj->anim.modelInstance->shadowType != OBJ_SHADOW_TYPE_NONE : shadow->shadowCastSlot != NULL))|
s|^            objShadowRender(obj, 0, 0, framesThisStep);$|            if (port_shadow_decals()) {\
                \*\(u8\*\)\(\(u8\*\)model->groundShadowVerts \+ 0x18\) = 0;\
                objDrawGroundShadow(obj, model);\
            } else {\
                objShadowRender(obj, 0, 0, framesThisStep);\
            }|
s|^        else if (obj->anim.modelInstance->shadowType == OBJ_SHADOW_TYPE_CRASH)$|        else if (!port_shadow_decals() \&\& obj->anim.modelInstance->shadowType == OBJ_SHADOW_TYPE_CRASH)|
