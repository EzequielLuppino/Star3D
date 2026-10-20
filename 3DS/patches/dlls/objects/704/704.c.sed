/^void titleScreenShowCopyright(u8 arg)/i\
void gameTextShowStr(char* text, int box, int cursorX, int cursorY);
/^        gameTextShow(FRONT_TEXT_COPYRIGHT);/a\
        /* Star 3D: port title, centred in text box 0 (560 wide at 40,40) */\
        gameTextSetColor(0xff, 0xd8, 0x40, (s32)(255.0f * gTitleScreenCursorX));\
        gameTextShowStr("STAR 3D", 0, 0, 0);\n        gameTextSetColor(0xff, 0xff, 0xff, 0xff);
