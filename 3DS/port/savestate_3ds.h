#ifndef SAVESTATE_3DS_H
#define SAVESTATE_3DS_H

#include <stdbool.h>

// Reload block of a save state (savestate_3ds.c): the savegame plus the player
// position as text, applied by reloading the map
bool savestate_in_game(void);
int savestate_format(char* out, int cap);       // Text length, 0 outside the game
bool savestate_apply_text(const char* text);    // Queues the reload for savestate_frame
void savestate_cancel_pending(void);            // After a snapshot restore
void savestate_frame(void);
bool savestate_warp(float x, float y, float z);  // debug teleport

// Snapshot save states (savestate_snap.c), in sdmc:/3ds/star3d/savestates.
// Slots are the newest states, slot 0 = newest. Requests run at the top of
// the next game frame; savestate_snap_message returns each new status once.
#define SAVESTATE_SLOTS 3
void savestate_snap_request_save(void);
void savestate_snap_request_load(int slot);
int savestate_snap_slots(char labels[SAVESTATE_SLOTS][24]);  // Filled slots
int savestate_snap_old_count(void);                           // States beyond the slots
int savestate_snap_clean(void);                               // Deletes those, returns how many
const char* savestate_snap_message(void);

// Cheats poking the player status (scale: 0 normal, 2 giant, -1 tiny)
void savestate_cheats(int health, int magic, int money, int scale);
bool savestate_cheat_heal(void);
void savestate_moonjump(bool held);          // Rises while held
bool savestate_log_where(const char* tag);  // player position, false outside the game

#endif
