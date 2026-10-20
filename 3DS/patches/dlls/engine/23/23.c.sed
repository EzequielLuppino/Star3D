# PAL language menu (README "Languages"): new options start in the 3DS system
# language (the PAL code reads the GameCube's language setting here).
1i\
unsigned char gameTextDefaultSaveLanguage(void);
s|^        ((SaveData\*)saveData)->speechVolume = SAVEGAME_DEFAULT_VOLUME;$|&\
        ((SaveData*)saveData)->languageIndex = gameTextDefaultSaveLanguage();|
s|^    options->speechVolume = SAVEGAME_DEFAULT_VOLUME;$|&\
    options->languageIndex = gameTextDefaultSaveLanguage();|
