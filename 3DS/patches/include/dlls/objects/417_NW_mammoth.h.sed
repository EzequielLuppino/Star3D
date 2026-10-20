# Retail reserves 0x110 bytes for the mammoth's curve walker (0x5C..0x16C),
# but RomCurveWalker is only 0x108, so on 32-bit pathState and every later
# field sat 8 bytes before its retail offset (the 64-bit build happened to
# line up because Curve's pointers are 8 bytes there).
/^    RomCurveWalker curveState;$/a\
    u8 curveStatePad[0x110 - 0x108];
/^STATIC_ASSERT(sizeof(NwMammothState) == 0x48C);$/i\
_Static_assert(sizeof(RomCurveWalker) == 0x108, "");\
_Static_assert(offsetof(NwMammothState, pathState) == 0x16C, "");\
_Static_assert(offsetof(NwMammothState, stateIndex) == 0x408, "");\
_Static_assert(offsetof(NwMammothState, pathPoints) == 0x45C, "");\
_Static_assert(sizeof(NwMammothState) == 0x48C, "");
