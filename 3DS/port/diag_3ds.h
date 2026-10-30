#pragma once
// Soak diagnostics (see diag_3ds.c)
void diag_frame(void);  // Once per frame from VIWaitForRetrace, after hidScanInput
void diag_stats(void);  // "diag" stats line + grown LOG_ONCE counts
void diag_mark(void);   // "mark" snapshot, flushed to the log
void diag_pmark(void);  // SELECT press: position only (no capture), flushed
void diag_map_loaded(int map_id);  // From the map loader hook (gx_init.c): starts S3D_GPU_SWEEP
