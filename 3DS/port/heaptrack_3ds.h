#pragma once
// C heap census (see heaptrack_3ds.c)
#include <3ds.h>

u32 heaptrack_heap_free(void);          // Free C heap in bytes (unclaimed + free chunks)
void heaptrack_report(const char* tag);  // Free heap, largest block and the top call sites by live bytes
