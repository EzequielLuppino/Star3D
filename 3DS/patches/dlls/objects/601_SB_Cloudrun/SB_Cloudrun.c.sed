# SBCloudRunnerRideState is a second view of SBCloudRunnerState (the padding
# names give the intended offsets: rotorAngle 0x6A, stickX 0x70, liftBase 0x7C).
# The padding after the 4-byte targetObj pointer was 0x24 instead of 0x18, so
# every later field sat 12 bytes too far: the ride state machine and steering
# read the wrong fields, and bankAmount/liftBase were written past the end of
# the object's state, overwriting the next heap object (its anim.eventTable
# became 12.0f = 0x41400000) and stalling the ShipBattle prologue.
s|^    u8 pad18\[0x24\];$|    u8 pad14[0x2C - 0x14];|
/^#define SB_CLOUDRUNNER_SPAWN_OBJECT_ID  0x119$/i\
STATIC_ASSERT(offsetof(SBCloudRunnerRideState, cloudYawDrift) == 0x2C);\
STATIC_ASSERT(offsetof(SBCloudRunnerRideState, liftAmount) == 0x60);\
STATIC_ASSERT(offsetof(SBCloudRunnerRideState, rideState) == 0x65);\
STATIC_ASSERT(offsetof(SBCloudRunnerRideState, rotorAngle) == 0x6A);\
STATIC_ASSERT(offsetof(SBCloudRunnerRideState, stickX) == 0x70);\
STATIC_ASSERT(offsetof(SBCloudRunnerRideState, liftBase) == 0x7C);\

