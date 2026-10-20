// Memory card (CARD API) for 3DS: slot A is a directory on the SD card,
// sdmc:/3ds/star3d/card/, with one file per save file and a ".stat"
// sidecar holding its directory entry (CARDStat: icon, comment, ...).
// Slot B reports no card. All calls complete synchronously.

#include <dirent.h>
#include <dolphin/card.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "log.h"

#define CARD_DIR      "sdmc:/3ds/star3d/card"
#define CARD_SECTOR   0x2000
#define CARD_MEM_MBIT 16         // 2 MB card (251 usable blocks)
#define MAX_FILES     127

static char sGameName[5] = "GSAE";
static char sCompany[3] = "01";
static char sNames[MAX_FILES][CARD_FILENAME_MAX + 1];  // fileNo -> name ("" = free)

// The game's save file is stored under the port's name on the SD card
#define HOST_SAVE_NAME "Star 3D"
extern char* sMemoryCardFileName;  // game save file name (maketex.c)

static bool is_game_save(const char* name) {
    return sMemoryCardFileName && strncmp(name, sMemoryCardFileName, CARD_FILENAME_MAX) == 0;
}

static void path_for(char* out, size_t size, const char* name, const char* ext) {
    if (is_game_save(name)) name = HOST_SAVE_NAME;
    // File names come from the game (ASCII); keep them filesystem-safe
    char safe[CARD_FILENAME_MAX + 1];
    size_t i;
    for (i = 0; name[i] && i < CARD_FILENAME_MAX; i++) {
        char c = name[i];
        safe[i] = (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                   c == '>' || c == '|') ? '_' : c;
    }
    safe[i] = 0;
    snprintf(out, size, "%s/%s%s", CARD_DIR, safe, ext);
}

static bool file_exists(const char* name, long* size) {
    char path[96];
    path_for(path, sizeof(path), name, ".bin");
    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (size) *size = (long)st.st_size;
    return true;
}

static int file_no_for(const char* name, bool add) {
    int free_slot = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (strncmp(sNames[i], name, CARD_FILENAME_MAX) == 0 && sNames[i][0]) return i;
        if (!sNames[i][0] && free_slot < 0) free_slot = i;
    }
    if (!add || free_slot < 0) return -1;
    strncpy(sNames[free_slot], name, CARD_FILENAME_MAX);
    sNames[free_slot][CARD_FILENAME_MAX] = 0;
    return free_slot;
}

// Rebuild the fileNo table from the directory
static void scan_dir(void) {
    memset(sNames, 0, sizeof(sNames));
    DIR* d = opendir(CARD_DIR);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcmp(e->d_name + n - 4, ".bin") == 0 && n - 4 <= CARD_FILENAME_MAX) {
            char name[CARD_FILENAME_MAX + 1];
            memcpy(name, e->d_name, n - 4);
            name[n - 4] = 0;
            file_no_for(strcmp(name, HOST_SAVE_NAME) == 0 && sMemoryCardFileName ? sMemoryCardFileName : name, true);
        }
    }
    closedir(d);
}

void CARDInit(const char* game, const char* maker) {
    if (game) { memcpy(sGameName, game, 4); sGameName[4] = 0; }
    if (maker) { memcpy(sCompany, maker, 2); sCompany[2] = 0; }
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/star3d", 0777);
    mkdir(CARD_DIR, 0777);
    // One-time migration: saves used to be stored under the game's own name
    if (sMemoryCardFileName) {
        static const char* const exts[] = {".bin", ".stat"};
        for (int i = 0; i < 2; i++) {
            char from[96], to[96];
            struct stat st;
            snprintf(from, sizeof(from), "%s/%s%s", CARD_DIR, sMemoryCardFileName, exts[i]);
            path_for(to, sizeof(to), sMemoryCardFileName, exts[i]);
            if (stat(to, &st) != 0 && stat(from, &st) == 0 && rename(from, to) == 0)
                LOG_INFO("card", "migrated %s -> %s", from, to);
        }
    }
    scan_dir();
    LOG_INFO("card", "memory card in %s (game %s, maker %s)", CARD_DIR, sGameName, sCompany);
}

void CARDSetLoadType(CARDFileType type) { (void)type; }
void CARDSetCardImagePath(const char* path, s32 chan) { (void)path; (void)chan; }

s32 CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (memSize) *memSize = CARD_MEM_MBIT;
    if (sectorSize) *sectorSize = CARD_SECTOR;
    return CARD_RESULT_READY;
}

s32 CARDMount(s32 chan, void* workArea, CARDCallback detachCallback) {
    (void)workArea; (void)detachCallback;
    if (chan != 0) return CARD_RESULT_NOCARD;
    scan_dir();
    return CARD_RESULT_READY;
}

s32 CARDUnmount(s32 chan) { return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }
s32 CARDCheck(s32 chan) { return chan == 0 ? CARD_RESULT_READY : CARD_RESULT_NOCARD; }

s32 CARDFormat(s32 chan) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    for (int i = 0; i < MAX_FILES; i++) {
        if (!sNames[i][0]) continue;
        char path[96];
        path_for(path, sizeof(path), sNames[i], ".bin");
        remove(path);
        path_for(path, sizeof(path), sNames[i], ".stat");
        remove(path);
    }
    memset(sNames, 0, sizeof(sNames));
    LOG_INFO("card", "formatted");
    return CARD_RESULT_READY;
}

s32 CARDGetSerialNo(s32 chan, u64* serialNo) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (serialNo) *serialNo = 0x3D5F0A7C12345678ull;
    return CARD_RESULT_READY;
}

static void fill_info(CARDFileInfo* fi, s32 chan, int fileNo, long size) {
    if (!fi) return;
    memset(fi, 0, sizeof(*fi));
    fi->chan = chan;
    fi->fileNo = fileNo;
    fi->length = (s32)size;
}

s32 CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    long size;
    if (!file_exists(fileName, &size)) return CARD_RESULT_NOFILE;
    fill_info(fileInfo, chan, file_no_for(fileName, true), size);
    return CARD_RESULT_READY;
}

s32 CARDCreate(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (strlen(fileName) > CARD_FILENAME_MAX) return CARD_RESULT_NAMETOOLONG;
    if (file_exists(fileName, NULL)) return CARD_RESULT_EXIST;
    int fileNo = file_no_for(fileName, true);
    if (fileNo < 0) return CARD_RESULT_LIMIT;

    size = (size + CARD_SECTOR - 1) & ~(CARD_SECTOR - 1);
    char path[96];
    path_for(path, sizeof(path), fileName, ".bin");
    FILE* f = fopen(path, "wb");
    if (!f) {
        sNames[fileNo][0] = 0;
        LOG_ERROR("card", "cannot create %s (errno %d)", path, errno);
        return CARD_RESULT_IOERROR;
    }
    static const u8 zeros[512];
    for (u32 done = 0; done < size; done += sizeof(zeros)) fwrite(zeros, 1, sizeof(zeros), f);
    fclose(f);

    CARDStat st;
    memset(&st, 0, sizeof(st));
    strncpy(st.fileName, fileName, CARD_FILENAME_MAX);
    st.length = size;
    memcpy(st.gameName, sGameName, 4);
    memcpy(st.company, sCompany, 2);
    path_for(path, sizeof(path), fileName, ".stat");
    f = fopen(path, "wb");
    if (f) {
        fwrite(&st, sizeof(st), 1, f);
        fclose(f);
    }
    fill_info(fileInfo, chan, fileNo, size);
    LOG_INFO("card", "created %s (%lu bytes)", fileName, (unsigned long)size);
    return CARD_RESULT_READY;
}

s32 CARDClose(CARDFileInfo* fileInfo) {
    (void)fileInfo;
    return CARD_RESULT_READY;
}

s32 CARDDelete(s32 chan, const char* fileName) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    char path[96];
    path_for(path, sizeof(path), fileName, ".bin");
    if (remove(path) != 0) return CARD_RESULT_NOFILE;
    path_for(path, sizeof(path), fileName, ".stat");
    remove(path);
    int fileNo = file_no_for(fileName, false);
    if (fileNo >= 0) sNames[fileNo][0] = 0;
    LOG_INFO("card", "deleted %s", fileName);
    return CARD_RESULT_READY;
}

static s32 transfer(const CARDFileInfo* fi, void* buf, s32 length, s32 offset, bool write) {
    if (!fi || fi->chan != 0 || fi->fileNo < 0 || fi->fileNo >= MAX_FILES || !sNames[fi->fileNo][0])
        return CARD_RESULT_NOFILE;
    char path[96];
    path_for(path, sizeof(path), sNames[fi->fileNo], ".bin");
    FILE* f = fopen(path, write ? "r+b" : "rb");
    if (!f) return CARD_RESULT_NOFILE;
    s32 result = CARD_RESULT_READY;
    if (fseek(f, offset, SEEK_SET) != 0) {
        result = CARD_RESULT_IOERROR;
    } else if (write) {
        if (fwrite(buf, 1, length, f) != (size_t)length) result = CARD_RESULT_IOERROR;
    } else {
        size_t got = fread(buf, 1, length, f);
        if (got < (size_t)length) memset((u8*)buf + got, 0, length - got);
    }
    fclose(f);
    return result;
}

s32 CARDRead(const CARDFileInfo* fileInfo, void* buf, s32 length, s32 offset) {
    return transfer(fileInfo, buf, length, offset, false);
}

s32 CARDWrite(const CARDFileInfo* fileInfo, const void* buf, s32 length, s32 offset) {
    return transfer(fileInfo, (void*)buf, length, offset, true);
}

s32 CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (fileNo < 0 || fileNo >= MAX_FILES || !sNames[fileNo][0] || !stat) return CARD_RESULT_NOFILE;
    char path[96];
    path_for(path, sizeof(path), sNames[fileNo], ".stat");
    memset(stat, 0, sizeof(*stat));
    FILE* f = fopen(path, "rb");
    if (f) {
        fread(stat, sizeof(*stat), 1, f);
        fclose(f);
    }
    long size = 0;
    file_exists(sNames[fileNo], &size);
    strncpy(stat->fileName, sNames[fileNo], CARD_FILENAME_MAX);
    stat->length = (u32)size;
    memcpy(stat->gameName, sGameName, 4);
    memcpy(stat->company, sCompany, 2);
    return CARD_RESULT_READY;
}

s32 CARDSetStatus(s32 chan, s32 fileNo, const CARDStat* stat) {
    if (chan != 0) return CARD_RESULT_NOCARD;
    if (fileNo < 0 || fileNo >= MAX_FILES || !sNames[fileNo][0] || !stat) return CARD_RESULT_NOFILE;
    char path[96];
    path_for(path, sizeof(path), sNames[fileNo], ".stat");
    FILE* f = fopen(path, "wb");
    if (!f) return CARD_RESULT_IOERROR;
    fwrite(stat, sizeof(*stat), 1, f);
    fclose(f);
    return CARD_RESULT_READY;
}
