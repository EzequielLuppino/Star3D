# Slow-call tracing for the SnowBike update path (the bike objects stall the
# object loop for seconds per update on hardware; see bug 44)
1i\
extern void port_slow_enter(const char* name);\
extern void port_slow_exit(const char* name);
s|^\( *\)SnowBike_UpdateRouteFollowing(obj, (SnowBikeState\*)state);$|\1port_slow_enter("bike: route"); SnowBike_UpdateRouteFollowing(obj, (SnowBikeState*)state); port_slow_exit("bike: route");|
s|^\( *\)if (s->routeFlags.b02)$|\1port_slow_enter("bike: attached"); if (s->routeFlags.b02)|
s|^\( *\)SnowBike_UpdateExhaustFx(obj, (uintptr_t)state);$|\1port_slow_enter("bike: exhaustFx"); SnowBike_UpdateExhaustFx(obj, (uintptr_t)state); port_slow_exit("bike: exhaustFx");|
s|^\( *\)SnowBike_buildOrientationMatrices(obj, (uintptr_t)state);$|\1port_slow_enter("bike: matrices"); SnowBike_buildOrientationMatrices(obj, (uintptr_t)state); port_slow_exit("bike: matrices");|
s|^\( *\)SnowBike_UpdateLiftSway((uintptr_t)obj, (uintptr_t)state);$|\1port_slow_enter("bike: liftSway"); SnowBike_UpdateLiftSway((uintptr_t)obj, (uintptr_t)state); port_slow_exit("bike: liftSway");|
s|^\( *\)objApplyVelocity(obj);$|\1port_slow_enter("bike: applyVel"); objApplyVelocity(obj); port_slow_exit("bike: applyVel");|
s|^\( *\)SnowBike_UpdateAirMeter(obj, state);$|\1port_slow_exit("bike: attached"); port_slow_enter("bike: airMeter"); SnowBike_UpdateAirMeter(obj, state); port_slow_exit("bike: airMeter");|
s|^\( *\)SnowBike_UpdateEngineFx(obj, state, s->localVelZ,$|\1port_slow_enter("bike: engineFx"); SnowBike_UpdateEngineFx(obj, state, s->localVelZ,|
s|^\( *\)(int)(850.0f \* -s->engineFxLevel), (u8\*)&s->stickY + 1, 7);$|\1(int)(850.0f * -s->engineFxLevel), (u8*)\&s->stickY + 1, 7); port_slow_exit("bike: engineFx");|
s|^\( *\)SnowBike_UpdateCollisionResponse(obj, (uintptr_t)state);$|\1port_slow_enter("bike: collision"); SnowBike_UpdateCollisionResponse(obj, (uintptr_t)state); port_slow_exit("bike: collision");|
s|^\( *\)SnowBike_UpdateSteering((short\*)obj, (uintptr_t)state);$|\1port_slow_enter("bike: steering"); SnowBike_UpdateSteering((short*)obj, (uintptr_t)state); port_slow_exit("bike: steering");|
s|^\( *\)SnowBike_UpdateTrails(obj, (uintptr_t)state);$|\1port_slow_enter("bike: trails"); SnowBike_UpdateTrails(obj, (uintptr_t)state); port_slow_exit("bike: trails");|
s|^\( *\)hitCount = trackGetHeight(obj, endpoint\[0\], endpoint\[1\], endpoint\[2\], &hits, 0, 0x20);$|\1port_slow_enter("bike: trackGetHeight"); hitCount = trackGetHeight(obj, endpoint[0], endpoint[1], endpoint[2], \&hits, 0, 0x20); port_slow_exit("bike: trackGetHeight");|
