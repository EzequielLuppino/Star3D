#pragma once
#include <dolphin/pad.h>

// Buttons held by a remote "press" command, OR'd into the pad by PADRead
extern u16 remote_pad_buttons;
// Stick positions held by a remote "stick"/"cstick" command (0 = released)
extern s8 remote_stick[2], remote_cstick[2];
void remote_frame(void);
