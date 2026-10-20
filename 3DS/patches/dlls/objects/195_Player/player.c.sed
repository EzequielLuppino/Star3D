# Staff aiming (CONTROLS.md, port/controls_3ds.c). The game eases the aim
# toward the main stick's position, so letting go of the stick springs the aim
# and the force-behind camera (DLL 82) back to the centre. With S3D_AIM_HOLD
# the stick moves the aim and it stays where it was left; gyro turns are added
# (S3D_GYRO_AIM). Past the screen edge the aim turns Fox instead of the game's
# 0.75 edge spin, which would keep turning while the aim rests there.
s|^int playerStateAimStaff(GameObject\* obj, PlayerState\* state, f32 fv) {$|int port_aim_hold(void);\
f32 port_aim_axis(f32 v, f32 stick, int axis);\
f32 port_aim_take_turn(void);\
int playerStateAimStaff(GameObject* obj, PlayerState* state, f32 fv) {|
s|^        inner->aimInputZ = inner->aimInputZ + interpolate(c - inner->aimInputZ, 0\.1f, timeDelta);$|        inner->aimInputZ = port_aim_hold() ? port_aim_axis(inner->aimInputZ, c, 1)\
                                           : inner->aimInputZ + interpolate(c - inner->aimInputZ, 0.1f, timeDelta);|
s|^        inner->aimInputX = inner->aimInputX + interpolate(c - inner->aimInputX, 0\.1f, timeDelta);$|        inner->aimInputX = port_aim_hold() ? port_aim_axis(inner->aimInputX, c, 0)\
                                           : inner->aimInputX + interpolate(c - inner->aimInputX, 0.1f, timeDelta);|
s|^        inner->targetYaw = -1000\.0f \* spin + (f32)(int)inner->targetYaw;$|        inner->targetYaw = -1000.0f * (port_aim_hold() ? 0.0f : spin) + port_aim_take_turn() +\
                           (f32)(int)inner->targetYaw;|
