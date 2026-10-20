# Bottom screen HUD: the minimap is drawn on the 3DS bottom screen
# (gx/gx_init.c gx_hud_begin; its text goes through port/hud_3ds.c)
1i\
int gx_hud_begin(void);\
void gx_hud_end(void);\
void port_savestate_point(void);
# Save states are captured and restored between frames (port/savestate_snap.c)
s|^static void gameLoop(void) {$|static void gameLoop(void) { port_savestate_point();|
s|^\( *\)gMinimapInterface->vtable->update();$|\1{ int hudRedirect = gx_hud_begin(); gMinimapInterface->vtable->update(); if (hudRedirect) gx_hud_end(); }|
