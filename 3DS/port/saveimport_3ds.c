// Save importer (Settings > Import saves): copies a game slot from a save file
// in sdmc:/3ds/star3d/saves into the emulated memory card.
//
// The game's save file is 3 blocks (0x6000 bytes): block 0 holds the card
// icons/banner, blocks 1 and 2 are two copies of the save data (the game
// writes both, reads the first one whose checksum is valid). Each data block
// holds 3 slots of 0x6EC bytes at 0xA50 and ends with a BE u64 checksum.
// Supported containers: .gci (0x40 header), .gcs (GCSAVE, 0x110 + 0x40),
// .sav (DATELGC_SAVE, 0x80 + 0x40), the raw 0x6000 file, and raw memory card
// images (directory + BAT walk to the "Star Fox Adventures" file).

#include <dirent.h>
#include <dolphin/card.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "log.h"
#include "saveimport_3ds.h"

#define CARD_BIN  "sdmc:/3ds/star3d/card/Star 3D.bin"
#define CARD_STAT "sdmc:/3ds/star3d/card/Star 3D.stat"
#define SAVE_SIZE 0x6000
#define BLK       0x2000
#define SLOT_OFF  0xA50
#define SLOT_SIZE 0x6EC

extern char* sMemoryCardFileName;            // "Star Fox Adventures"
extern u32 gSaveCardChecksumHi, gSaveCardChecksumLo;

static u8 s_src[SAVE_SIZE], s_card[SAVE_SIZE];
static u8 s_src_hdr[0x40];
static bool s_src_has_hdr;

static u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
static u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static u64 be64(const u8* p) { return (u64)be32(p) << 32 | be32(p + 4); }
static void put16(u8* p, u16 v) { p[0] = v >> 8; p[1] = (u8)v; }
static void put32(u8* p, u32 v) { put16(p, v >> 16); put16(p + 2, (u16)v); }
static void put64(u8* p, u64 v) { put32(p, (u32)(v >> 32)); put32(p + 4, (u32)v); }

static u64 checksum(const u8* blk) {
    u64 x = 0, a = 1;
    for (int i = 0; i < 0x3FF; i++) {
        u64 v = be64(blk + i * 8);
        x ^= v;
        a += v;
    }
    return x ^ (a + 13);
}

static bool block_ok(const u8* save, int b) {
    const u8* blk = save + b * BLK;
    return checksum(blk) == be64(blk + 0x1FF8);
}

// Data block the game would read (1, else 2), 0 when neither is valid
static int valid_block(const u8* save) {
    return block_ok(save, 1) ? 1 : block_ok(save, 2) ? 2 : 0;
}

static const u8* slot_ptr(const u8* save, int b, int i) { return save + b * BLK + SLOT_OFF + i * SLOT_SIZE; }
static bool slot_used(const u8* s) { return s[0x21] != 0; }

static bool read_at(FILE* f, long off, void* dst, size_t n) {
    return fseek(f, off, SEEK_SET) == 0 && fread(dst, 1, n, f) == n;
}

// Raw memory card image: find the game's file in the directory and follow its
// block chain in the BAT
static bool load_card_image(FILE* f, long size) {
    static u8 dir[BLK], bat[BLK], tmp[BLK];
    if (!read_at(f, 1 * BLK, dir, BLK) || !read_at(f, 2 * BLK, tmp, BLK)) return false;
    if ((s16)be16(tmp + 0x1FFA) > (s16)be16(dir + 0x1FFA)) memcpy(dir, tmp, BLK);
    if (!read_at(f, 3 * BLK, bat, BLK) || !read_at(f, 4 * BLK, tmp, BLK)) return false;
    if ((s16)be16(tmp + 4) > (s16)be16(bat + 4)) memcpy(bat, tmp, BLK);
    long nblocks = size / BLK;
    for (int i = 0; i < 127; i++) {
        const u8* e = dir + i * 0x40;
        if (memcmp(e, "GSA", 3) != 0 || strncmp((const char*)e + 8, sMemoryCardFileName, 32) != 0) continue;
        u16 blk = be16(e + 0x36), count = be16(e + 0x38);
        if (count * BLK < SAVE_SIZE) return false;
        for (int k = 0; k < SAVE_SIZE / BLK; k++) {
            if (blk < 5 || blk >= nblocks || !read_at(f, (long)blk * BLK, s_src + k * BLK, BLK)) return false;
            blk = be16(bat + 0x0A + (blk - 5) * 2);
        }
        memcpy(s_src_hdr, e, 0x40);
        s_src_has_hdr = true;
        return true;
    }
    return false;
}

// Loads a save file into s_src; false when it isn't a valid game save
static bool load_source(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    u8 head[0x10] = {0};
    read_at(f, 0, head, sizeof(head));
    long data = -1, hdr = -1;
    s_src_has_hdr = false;
    bool ok = false;
    if (memcmp(head, "GCSAVE", 6) == 0) { hdr = 0x110; data = 0x150; }
    else if (memcmp(head, "DATELGC_SAVE", 12) == 0) { data = 0xC0; }  // swapped header, skip it
    else if (memcmp(head, "GSA", 3) == 0 && size >= 0x40 + SAVE_SIZE) { hdr = 0; data = 0x40; }
    else if (size == SAVE_SIZE) { data = 0; }
    else if (size >= 0x80000 && size % 0x40000 == 0) ok = load_card_image(f, size);
    if (data >= 0) ok = size >= data + SAVE_SIZE && read_at(f, data, s_src, SAVE_SIZE);
    if (ok && hdr >= 0) s_src_has_hdr = read_at(f, hdr, s_src_hdr, 0x40) && memcmp(s_src_hdr, "GSA", 3) == 0;
    fclose(f);
    return ok && valid_block(s_src) != 0;
}

static bool load_card(void) {
    FILE* f = fopen(CARD_BIN, "rb");
    if (!f) return false;
    bool ok = fread(s_card, 1, SAVE_SIZE, f) == SAVE_SIZE;
    fclose(f);
    return ok;
}

static void copy_name(char* out, const u8* s) {
    for (int i = 0; i < 3; i++) out[i] = (s[0x1C + i] >= 0x20 && s[0x1C + i] < 0x7F) ? s[0x1C + i] : '?';
    out[3] = 0;
}

int saveimport_scan(ImportEntry* out, int max) {
    mkdir(SAVEIMPORT_DIR, 0777);
    DIR* d = opendir(SAVEIMPORT_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent* de;
    while (n < max && (de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || strlen(de->d_name) >= sizeof(out->file)) continue;
        char path[128];
        snprintf(path, sizeof(path), "%s/%s", SAVEIMPORT_DIR, de->d_name);
        if (!load_source(path)) {
            LOG_INFO("import", "skipped %s (not a Star Fox Adventures save)", de->d_name);
            continue;
        }
        int b = valid_block(s_src);
        for (int i = 0; i < 3 && n < max; i++) {
            const u8* s = slot_ptr(s_src, b, i);
            if (!slot_used(s)) continue;
            ImportEntry* e = &out[n++];
            snprintf(e->file, sizeof(e->file), "%s", de->d_name);
            copy_name(e->name, s);
            e->slot = i;
            e->percent = s[0x55D] * 100 / 187;
            u32 t = be32(s + 0x560);
            float frames;
            memcpy(&frames, &t, 4);
            e->minutes = frames > 0 && frames < 1e9f ? (int)(frames / 3600.0f) : 0;
        }
    }
    closedir(d);
    LOG_INFO("import", "found %d importable slots in %s", n, SAVEIMPORT_DIR);
    return n;
}

bool saveimport_current(char names[3][4]) {
    if (!load_card()) return false;
    int b = valid_block(s_card);
    for (int i = 0; i < 3; i++) {
        const u8* s = slot_ptr(s_card, b ? b : 1, i);
        if (b && slot_used(s)) copy_name(names[i], s);
        else strcpy(names[i], "---");
    }
    return true;
}

int saveimport_free_slot(void) {
    if (!load_card()) return 0;
    int b = valid_block(s_card);
    if (!b) return 0;  // corrupt card: every slot is unusable anyway
    for (int i = 0; i < 3; i++)
        if (!slot_used(slot_ptr(s_card, b, i))) return i;
    return -1;
}

bool saveimport_backup(char* out_name, size_t size) {
    if (!load_card()) return false;
    CARDStat st;
    memset(&st, 0, sizeof(st));
    FILE* f = fopen(CARD_STAT, "rb");
    if (f) {
        fread(&st, sizeof(st), 1, f);
        fclose(f);
    }
    // GCI header = the card directory entry
    u8 h[0x40];
    memset(h, 0, sizeof(h));
    memcpy(h, st.gameName[0] ? (const char*)st.gameName : "GSAE", 4);
    memcpy(h + 4, st.company[0] ? (const char*)st.company : "01", 2);
    h[6] = 0xFF;
    h[7] = st.bannerFormat;
    strncpy((char*)h + 8, sMemoryCardFileName, 32);
    time_t now = time(NULL);
    put32(h + 0x28, st.time ? st.time : (u32)(now - 946684800));  // seconds since 2000
    put32(h + 0x2C, st.iconAddr);
    put16(h + 0x30, st.iconFormat);
    put16(h + 0x32, st.iconSpeed);
    h[0x34] = CARD_ATTR_PUBLIC;
    put16(h + 0x38, SAVE_SIZE / BLK);
    put16(h + 0x3A, 0xFFFF);
    put32(h + 0x3C, st.commentAddr);

    struct tm* tm = localtime(&now);
    char name[64], path[128];
    snprintf(name, sizeof(name), "backup_%04d%02d%02d_%02d%02d%02d.gci", tm->tm_year + 1900, tm->tm_mon + 1,
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    mkdir(SAVEIMPORT_DIR, 0777);
    snprintf(path, sizeof(path), "%s/%s", SAVEIMPORT_DIR, name);
    f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h) && fwrite(s_card, 1, SAVE_SIZE, f) == SAVE_SIZE;
    fclose(f);
    if (!ok) { remove(path); return false; }
    snprintf(out_name, size, "%s", name);
    LOG_WARN("import", "backed up the card to %s", path);
    return true;
}

static void write_stat_from_header(void) {
    CARDStat st;
    memset(&st, 0, sizeof(st));
    strncpy(st.fileName, sMemoryCardFileName, CARD_FILENAME_MAX);
    st.length = SAVE_SIZE;
    memcpy(st.gameName, "GSAE", 4);
    memcpy(st.company, "01", 2);
    if (s_src_has_hdr) {
        const u8* h = s_src_hdr;
        memcpy(st.gameName, h, 4);
        memcpy(st.company, h + 4, 2);
        st.bannerFormat = h[7];
        st.time = be32(h + 0x28);
        st.iconAddr = be32(h + 0x2C);
        st.iconFormat = be16(h + 0x30);
        st.iconSpeed = be16(h + 0x32);
        st.commentAddr = be32(h + 0x3C);
    }
    FILE* f = fopen(CARD_STAT, "wb");
    if (f) {
        fwrite(&st, sizeof(st), 1, f);
        fclose(f);
    }
}

bool saveimport_apply(const ImportEntry* e, int dst, char* msg, size_t size) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", SAVEIMPORT_DIR, e->file);
    if (!load_source(path)) {
        snprintf(msg, size, "Cannot read %s", e->file);
        return false;
    }
    const u8* src = slot_ptr(s_src, valid_block(s_src), e->slot);
    bool have_card = load_card() && valid_block(s_card) != 0;
    if (!have_card) {
        // No (valid) card yet: the source file becomes the card
        memcpy(s_card, s_src, SAVE_SIZE);
        write_stat_from_header();
    }
    if (dst < 0 || dst > 2) dst = 0;
    for (int b = 1; b <= 2; b++) {
        u8* blk = s_card + b * BLK;
        memmove(blk + SLOT_OFF + dst * SLOT_SIZE, src, SLOT_SIZE);
        put64(blk + 0x1FF8, checksum(blk));
    }
    FILE* f = fopen(CARD_BIN, "wb");
    bool ok = f && fwrite(s_card, 1, SAVE_SIZE, f) == SAVE_SIZE;
    if (f) fclose(f);
    if (!ok) {
        snprintf(msg, size, "Writing the card failed");
        LOG_ERROR("import", "cannot write %s", CARD_BIN);
        return false;
    }
    // The game refuses to save when the card's checksum differs from the one
    // it cached ("card was changed"); 0 makes it cache the new one
    gSaveCardChecksumHi = gSaveCardChecksumLo = 0;
    snprintf(msg, size, "Imported %s to slot %d", e->name, dst + 1);
    LOG_WARN("import", "imported %s slot %d (%s, %d%%) into card slot %d%s", e->file, e->slot + 1, e->name,
             e->percent, dst + 1, have_card ? "" : " (new card)");
    return true;
}

bool saveimport_run(const ImportEntry* e, int dst, char* msg, size_t size) {
    if (dst < 0) dst = saveimport_free_slot();
    if (dst < 0) {
        snprintf(msg, size, "Card full, pick a slot");
        return false;
    }
    char names[3][4], backup[64] = "";
    if (saveimport_current(names) && strcmp(names[dst], "---") != 0 && !saveimport_backup(backup, sizeof(backup))) {
        snprintf(msg, size, "Backup failed, nothing changed");
        return false;
    }
    if (!saveimport_apply(e, dst, msg, size)) return false;
    if (backup[0]) {
        size_t n = strlen(msg);
        snprintf(msg + n, size - n, " (backup made)");
    }
    return true;
}
