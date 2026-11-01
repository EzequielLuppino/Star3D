#pragma once
#include <stddef.h>

// Guarded heap for the DL cache's side tables (gx_dlcache.c): every block
// gets a magic head (magic, size) and a magic tail word. dl_free verifies
// both before freeing and svcBreaks with the caller's description when they
// are wrong - a double-free, a stale pointer or a write past a table's end,
// bug 39's corrupted-heap crash mode (free_entry -> _free_r with a garbage
// block header). The diagnostics name the table and the entry, and the crash
// handler's [CRASH] block preserves them in the log.
void* dl_alloc(size_t n);
void* dl_calloc(size_t n, size_t sz);
void* dl_realloc(void* p, size_t n);
void dl_free(void* p, const char* what, const void* entry);
