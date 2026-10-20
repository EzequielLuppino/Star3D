#ifndef TOUCHUI_3DS_H
#define TOUCHUI_3DS_H

// Bottom screen touch menu (settings, minimap, cheats), see touchui_3ds.c
void touchui_load_settings(void);
void touchui_input(void);
void touchui_frame(void);
void touchui_draw(void);
void touchui_remote_tap(int x, int y);  // Remote "tap X Y" (bottom screen pixels)
void touchui_remote_flick(int dx, int dy);  // Remote "flick DX DY" on the item pad
void touchui_pad_state(signed char* cx, signed char* cy, unsigned short* buttons);  // For PADRead

#endif
