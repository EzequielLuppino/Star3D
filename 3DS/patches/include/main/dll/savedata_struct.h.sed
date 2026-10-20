# PAL language menu (README "Languages"): the PAL options keep the text
# language in byte 1 of the options block, which the US layout calls pad01.
# 0 English, 1 French, 2 Italian, 3 Spanish, 4 German (gametext ids through
# gameTextApplySaveLanguage).
s|^    u8 pad01;$|    u8 languageIndex;|
