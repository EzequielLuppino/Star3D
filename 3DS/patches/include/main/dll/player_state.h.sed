# CharacterEyeAnimState is 0x28 bytes (0x364..0x38C), but the pad after it
# started at 0x390, so on 32-bit every field from footPoints on sat 4 bytes
# before its retail offset (the 64-bit build happened to line up because
# playerStatus is 8 bytes there).
s|^    u8 pad390\[0x3C4 - 0x390\];$|    u8 pad38C[0x3C4 - 0x38C];|
/^STATIC_ASSERT(sizeof(PlayerState) == 0x8E0);$/i\
_Static_assert(offsetof(PlayerState, eyeAnimState) == 0x364, "");\
_Static_assert(offsetof(PlayerState, footPoints) == 0x3C4, "");\
_Static_assert(offsetof(PlayerState, contactObject) == 0x67C, "");\
_Static_assert(sizeof(PlayerState) == 0x8E0, "");
