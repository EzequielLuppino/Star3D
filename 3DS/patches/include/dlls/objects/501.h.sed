# Retail state is 0x140: ObjSeqState plus eight bytes, the first holding the
# light. The 64-bit pointer filled all eight; pad to the retail size on 32-bit.
/^    ModelLightStruct\* light;$/a\
    u8 pad13C[0x140 - 0x13C];
/^STATIC_ASSERT(sizeof(Dll1F5State) == 0x140);$/i\
_Static_assert(offsetof(Dll1F5State, light) == 0x138, "");\
_Static_assert(sizeof(Dll1F5State) == 0x140, "");
