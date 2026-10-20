# Stereo convergence (GRAPHICS_PLAN.md step 11): report the distance from
# the camera to Fox each time the view matrices are rebuilt; gx_render.c
# smooths it into the zero-parallax distance.
s|^void Camera_UpdateViewMatrices(void) {$|GameObject* Obj_GetPlayerObject(void);\
void port_stereo_focus(f32 x, f32 y, f32 z);\
void Camera_UpdateViewMatrices(void) {|
/^    gCameraInverseViewRotationMatrix\[11\] = gCameraInverseViewRotationMatrix\[7\] = gCameraInverseViewRotationMatrix\[3\] =$/{
n
s|^        0\.0f;$|        0.0f;\
    {\
        GameObject* fhPlayer = Obj_GetPlayerObject();\
        if (fhPlayer != NULL) {\
            port_stereo_focus(fhPlayer->anim.worldPosX - camera->x, fhPlayer->anim.worldPosY - camera->y,\
                              fhPlayer->anim.worldPosZ - camera->z);\
        }\
    }|
}
