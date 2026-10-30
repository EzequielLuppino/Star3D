#pragma once
#include <dolphin/pad.h>

// Port-internal SELECT bit (the GameCube controller has none). The remote
// "press C" command and pad_3ds.c's KEY_SELECT mapping use it; it is never
// set by the game itself, so the extra bit in PADStatus.button is harmless.
#define PAD_BUTTON_SELECT 0x2000

// Buttons held by a remote "press" command, OR'd into the pad by PADRead
extern u16 remote_pad_buttons;
// Stick positions held by a remote "stick"/"cstick" command (0 = released)
extern s8 remote_stick[2], remote_cstick[2];
void remote_frame(void);
// Poll cmd.txt this frame (netsrv_3ds.c calls this after a PUT /cmd.txt)
void remote_poll_now(void);
