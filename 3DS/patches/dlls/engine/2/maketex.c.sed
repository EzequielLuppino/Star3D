# loadMemCardImages reads the save banner/icon file names at fixed offsets
# from sMemoryCardFileNameString, which in the original executable starts a
# packed block of strings. The decompiled source declares only the 20-byte
# title, so those offsets pointed into unrelated data. Rebuild the block with
# the names at the offsets the code uses (US layout).
/^void loadMemCardImages(void) {$/i\
static char* fhMemCardNameBlock(void) {\
    static char block[0x150];\
    if (block[0] == 0) {\
        strcpy(block, sMemoryCardFileNameString);\
        strcpy(block + 0xc4, "opening.bnr");\
        strcpy(block + 0xd0, "card/memcardicon0.img");\
        strcpy(block + 0xe8, "card/memcardicon1.img");\
        strcpy(block + 0x100, "card/memcardicon2.img");\
        strcpy(block + 0x118, "card/memcardicon3.img");\
        strcpy(block + 0x130, "card/memcardicon0.pal");\
    }\
    return block;\
}\

s|^    char\* names = sMemoryCardFileNameString;$|    char* names = fhMemCardNameBlock();|
