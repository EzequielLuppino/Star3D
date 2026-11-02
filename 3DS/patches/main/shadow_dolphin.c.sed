# Shadow decals (gx_init.c port_shadow_decals): objects with a geometric
# shadow never fill their 512x512 cast texture in decal mode (no caster RTT
# passes), so their flat ground quad would sample an empty texture and
# vanish. Draw the game's small soft disk instead.
1i\
_Bool port_shadow_decals(void);
s|^            selectTexture((Texture\*)(&obj->anim)->modelState->shadowTexture, 0);$|            if (port_shadow_decals() \&\& obj->anim.modelInstance->shadowType == OBJ_SHADOW_TYPE_MODEL_GEOMETRIC) {\
                selectTexture(getNewShadowSmallDiskTexture(), 0);\
            } else {\
                selectTexture((Texture\*)(\&obj->anim)->modelState->shadowTexture, 0);\
            }|
