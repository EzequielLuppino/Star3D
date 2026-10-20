# Bug 34 (C12 floor switch losing its texture): references are a 1-based slot
# handle or a Texture pointer. The GameCube tells them apart by the sign bit
# (0x80000000+ = pointer); 3DS game memory is at 0x30000000+, so a pointer
# (textureLoad returns one on a cache hit of a texture first loaded without a
# handle, e.g. by the map) was taken for a slot, failed the range check and
# came back NULL: selectTexture(NULL) then left the previous material's texture
# bound. Handles stop at LOADED_TEXTURE_CAPACITY
s|^    if (idx > INT32_MAX)$|    if (idx <= INT32_MAX \&\& idx > LOADED_TEXTURE_CAPACITY) port_tex_ptr_ref((void*)idx);\
    if (idx > INT32_MAX \|\| idx > LOADED_TEXTURE_CAPACITY)|
1i\
void port_tex_ptr_ref(void* tex);
