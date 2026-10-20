# Retail CCPedestalState is 8 bytes (gamebit at 0x04, flags at 0x06); the
# padding and asserts were sized for an 8-byte callback pointer.
s|^    u8 pad0B\[5\];$|    u8 pad07[1];|
s|^STATIC_ASSERT(sizeof(CcPedestalState) == 0x10);$|STATIC_ASSERT(sizeof(CcPedestalState) == 0x08);|
s|^STATIC_ASSERT(offsetof(CcPedestalState, activationGameBit) == 0x08);$|STATIC_ASSERT(offsetof(CcPedestalState, activationGameBit) == 0x04);|
s|^STATIC_ASSERT(offsetof(CcPedestalState, pendingGameBitFlags) == 0x0A);$|STATIC_ASSERT(offsetof(CcPedestalState, pendingGameBitFlags) == 0x06);|
/^STATIC_ASSERT(sizeof(CcPedestalState) == 0x08);$/i\
_Static_assert(sizeof(CcPedestalState) == 0x08, "");
