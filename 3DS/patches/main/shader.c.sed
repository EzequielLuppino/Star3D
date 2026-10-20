# Object draw distance scale, minimum screen size and census
# (MULTICORE_PLAN.md "Hub: fewer objects", port/lod_3ds.c): objUpdateOpacity's
# per-placement range is scaled before the distance test (the 100-unit fade
# band moves with it), and objects smaller than S3D_OBJ_MINPX are dropped
1i\
float port_obj_range(const void* obj, const char* name, int def_id, int class_id, int dll_id, float range, float d);\
int port_obj_small(const void* obj, float sz);
s|^        if (d > range)$|        range = port_obj_range(obj, obj->anim.modelInstance ? obj->anim.modelInstance->name : 0, obj->anim.defId, obj->anim.classId, obj->anim.modelInstance ? obj->anim.modelInstance->dllId : -1, range, d);\n        if (d > range)|
s|^        sz = sz \* gMapBlockWorldSize;$|        sz = sz * gMapBlockWorldSize;\n        if (port_obj_small(obj, sz))\n        {\n            obj->anim.renderAlpha = 0;\n            return 0;\n        }|
