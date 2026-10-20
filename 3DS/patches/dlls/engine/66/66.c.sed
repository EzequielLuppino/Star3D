# C-stick camera (CONTROLS.md, port/controls_3ds.c). The normal camera is a
# leash: it only corrects its distance to Fox and derives its yaw from where
# it stands, so swinging its position around Fox once per frame turns it and
# nothing pulls it back. The C-stick's up/down moves the camera's height band,
# which the camera eases toward like its own height changes.
s|^#include "main/dll/dll_0042_cameramodenormal.h"$|&\
f32 port_camera_orbit_yaw(void);\
f32 port_camera_orbit_height(void);|
s|^    CameraModeNormal_updateWallAvoidance(camera, target\[0\]);$|&\
    {\
        f32 fhOrbit = port_camera_orbit_yaw();\
        if (fhOrbit != 0.0f) {\
            f32 fhSin = mathSinf(fhOrbit);\
            f32 fhCos = mathCosf(fhOrbit);\
            f32 fhDx = camera->anim.worldPosX - target[0]->anim.worldPosX;\
            f32 fhDz = camera->anim.worldPosZ - target[0]->anim.worldPosZ;\
            camera->anim.worldPosX = target[0]->anim.worldPosX + fhDx * fhCos - fhDz * fhSin;\
            camera->anim.worldPosZ = target[0]->anim.worldPosZ + fhDx * fhSin + fhDz * fhCos;\
            Obj_TransformWorldPointToLocal(camera->anim.worldPosX, camera->anim.worldPosY, camera->anim.worldPosZ,\
                                           \&camera->anim.localPosX, \&camera->anim.localPosY,\
                                           \&camera->anim.localPosZ, camera->anim.parent);\
        }\
    }|
s|^    upperY += slideOffset;$|&\
    lowerY += port_camera_orbit_height();\
    upperY += port_camera_orbit_height();|
