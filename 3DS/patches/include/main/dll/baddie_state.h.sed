# Restore the GameCube layout (retail sizeof 0x35C). foxhollow padded the
# CurvesCollisionState overlay for 64-bit builds and added a contactObj field
# after the union, which pushed every later field (controlMode, timers, input,
# hitPoints...) 0x14 bytes past its retail offset. Structs that embed
# BaddieState (PlayerState, GroundBaddieState, ...) and code using raw offsets
# into it then disagreed. With curves_collision_state.h.sed the union is the
# retail 0x268 bytes again.
s|^            u8 padC5\[0x118 - 0xC5 + 12\];$|            u8 padC5[0x118 - 0xC5];|
s|^            u8 unk124\[0x19C - 0x124 + 4\];$|            u8 unk124[0x19C - 0x124];|
/^    void\* contactObj; \/\* GameObject\*;/d
/^STATIC_ASSERT(sizeof(BaddieState) == 0x35C);$/i\
_Static_assert(offsetof(BaddieState, unk118) == 4 + offsetof(CurvesCollisionState, localPointTarget), "");\
_Static_assert(offsetof(BaddieState, spawnRotY) == 4 + offsetof(CurvesCollisionState, tiltPitch), "");\
_Static_assert(offsetof(BaddieState, unk1B0) == 4 + offsetof(CurvesCollisionState, resultFloorGap), "");\
_Static_assert(offsetof(BaddieState, waterSurfaceY) == 4 + offsetof(CurvesCollisionState, resultWaterY), "");\
_Static_assert(offsetof(BaddieState, physicsActive) == 4 + offsetof(CurvesCollisionState, subtype), "");\
_Static_assert(offsetof(BaddieState, surfaceFlags) == 4 + offsetof(CurvesCollisionState, surfaceFlags), "");\
_Static_assert(offsetof(BaddieState, controlMode) == 0x274, "");\
_Static_assert(offsetof(BaddieState, orientationAxesOut) == 0x27C, "");\
_Static_assert(sizeof(BaddieState) == 0x35C, "");
