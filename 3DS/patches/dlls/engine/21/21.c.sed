# PAL language menu (README "Languages"): loading the options applies the
# saved text language, as the PAL loadSaveSettings does
# (patches/main/textrender_run.c.sed).
1i\
void gameTextApplySaveLanguage(int index);
s|^    audioSetVolumes(((SaveData\*)saveData)->speechVolume, 10, 0, 0, 1);$|&\
    gameTextApplySaveLanguage(((SaveData*)saveData)->languageIndex);|
