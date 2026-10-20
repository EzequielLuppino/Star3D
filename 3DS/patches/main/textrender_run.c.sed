# Bottom screen HUD: text in the minimap's box is drawn on the bottom screen
# (port/hud_3ds.c)
1i\
void port_text_render_by_id(int textId, int x, int y);\
void port_text_render_strs(char* str, int boxIdx);
s|\bgameTextRenderById(|port_text_render_by_id(|g
s|\bgameTextRenderStrs(|port_text_render_strs(|g

# PAL language menu (README "Languages"). The US code this port compiles has
# no language switch; the PAL disc has every language's text. This is the
# PAL gameTextSetLanguage without its disc-error message tables (there is no
# disc on the 3DS). Loading the HUD directory first leaves gGameTextLastDir
# different from the current directory, so the dialogue text reloads too.
# The save's language index: 0 English, 1 French, 2 Italian, 3 Spanish,
# 4 German (the PAL gSaveGameLanguageMap). FOXHOLLOW_LANGUAGE (env.txt, read
# at boot) overrides the save.
$a\
\
u8 OSGetLanguage(void);\
\
static const u8 sGameTextSaveLanguageIds[5] = {0, 1, 3, 5, 2};\
\
void gameTextSetLanguage(int language)\
{\
    int dirId;\
\
    curLanguage = language;\
    if (language == gGameTextLastLanguage)\
    {\
        return;\
    }\
    dirId = curGameTextDir;\
    gameTextLoadDir(0x1c);\
    gameTextLoadDir(dirId);\
}\
\
void gameTextApplySaveLanguage(int index)\
{\
    if (fhConfigLanguage() >= 0 || gGameTextFontIsSjis != 0)\
    {\
        return;\
    }\
    if (index < 0 || index >= 5)\
    {\
        index = 0;\
    }\
    gameTextSetLanguage(sGameTextSaveLanguageIds[index]);\
}\
\
/* A new save's language: the 3DS system language (OSGetLanguage, os_3ds.c) */\
u8 gameTextDefaultSaveLanguage(void)\
{\
    switch (OSGetLanguage())\
    {\
    case 1: /* OS_LANGUAGE_GERMAN */\
        return 4;\
    case 2: /* OS_LANGUAGE_FRENCH */\
        return 1;\
    case 3: /* OS_LANGUAGE_SPANISH */\
        return 3;\
    case 4: /* OS_LANGUAGE_ITALIAN */\
        return 2;\
    }\
    return 0;\
}
