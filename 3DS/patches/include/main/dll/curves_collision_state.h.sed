# Restore the GameCube layout. foxhollow moved the four pointers to the end of
# the struct for 64-bit builds, which shifted everything after segmentHits by
# 12 bytes and grew the struct past its retail 0x268. Code that reaches these
# fields through raw offsets or through overlapping views (BaddieState) then
# reads the wrong data. Retail has segmentLocalPoints at 0x04 and the other
# three right after segmentHits.
s|^    u8 pad004\[0x008 - 0x004\];$|    f32* segmentLocalPoints;|
/^    TrackHitResults segmentHits;/a\
    GameObject* contactObj;      /* 0x0D8 */\
    f32* localPointPositions;    /* 0x0DC */\
    f32* localPointRadii;        /* 0x0E0 */
/^    f32\* segmentLocalPoints;$/{
x
s/^/x/
/^xx$/{x;d}
x
}
/^    GameObject\* contactObj;$/d
/^    f32\* localPointPositions;$/d
/^    f32\* localPointRadii;$/d
/^STATIC_ASSERT(offsetof(CurvesCollisionState, segmentHits) == 0x068);$/a\
_Static_assert(sizeof(CurvesCollisionState) == CURVES_COLLISION_STATE_SIZE, "");\
_Static_assert(offsetof(CurvesCollisionState, segmentLocalPoints) == 0x004, "");\
_Static_assert(offsetof(CurvesCollisionState, contactObj) == 0x0D8, "");\
_Static_assert(offsetof(CurvesCollisionState, localPointWorld) == 0x0E4, "");\
_Static_assert(offsetof(CurvesCollisionState, localHit) == 0x144, "");\
_Static_assert(offsetof(CurvesCollisionState, tiltPitch) == 0x198, "");\
_Static_assert(offsetof(CurvesCollisionState, resultFloorGap) == 0x1AC, "");\
_Static_assert(offsetof(CurvesCollisionState, hitBounds) == 0x240, "");\
_Static_assert(offsetof(CurvesCollisionState, subtype) == 0x25B, "");
