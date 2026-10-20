#pragma once
#include <stdbool.h>
#include <stddef.h>

#define SAVEIMPORT_DIR "sdmc:/3ds/star3d/saves"
#define SAVEIMPORT_MAX 48

// One importable game slot found in a file of SAVEIMPORT_DIR
typedef struct {
    char file[64];   // file name inside SAVEIMPORT_DIR
    char name[4];    // player name (3 chars)
    int slot;        // slot index in that file (0..2)
    int percent;     // completion
    int minutes;     // play time
} ImportEntry;

// Lists the occupied slots of every supported save file (.gci, .gcs, .sav,
// raw 0x6000 .bin, raw card images .raw/.gcp/.mci). Returns the count.
int saveimport_scan(ImportEntry* out, int max);
// First free slot of the current card, 0 when there is no card yet, -1 when full
int saveimport_free_slot(void);
// Names of the current card's slots ("---" when free); false without a card
bool saveimport_current(char names[3][4]);
// Writes the current card to SAVEIMPORT_DIR/backup_<date>.gci
bool saveimport_backup(char* out_name, size_t size);
// Copies the entry's slot into slot dst of the current card (creating the card
// from the source file when there is none). msg receives a short result.
bool saveimport_apply(const ImportEntry* e, int dst, char* msg, size_t size);
// Full import as the touch menu does it: dst < 0 picks the first free slot
// (fails when the card is full); an occupied dst is backed up first.
bool saveimport_run(const ImportEntry* e, int dst, char* msg, size_t size);
