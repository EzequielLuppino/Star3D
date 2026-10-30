// In-game network server (see netsrv_3ds.c): the console test loop's file
// transfer channel, replacing the ftpd-app switch dance.
#pragma once

#include <stdbool.h>

void netsrv_init(void);              // No-op unless S3D_NET_SERVER=1
bool netsrv_restart_requested(void); // True after GET /restart (vi_3ds.c exits)
void netsrv_relaunch_app(void);      // hb:ldr target = /app.3dsx build (call just before exiting)
void netsrv_shutdown(void);          // socExit on every exit path (port cleanup)
