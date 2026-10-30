// In-game network server (netsrv_3ds.c): a tiny HTTP server for the console
// test loop, so a PC can pull the log/captures and push new cmd.txt / env.txt /
// settings.txt / a new .3dsx / romfs.bin without the user switching to ftpd.
//
// Enabled with S3D_NET_SERVER=1 in env.txt (default off), port S3D_NET_PORT
// (default 8080). The server thread lives on core 3 (core 0 = game, core 1 =
// audio/helper, core 2 = render thread) and blocks in accept(), so it costs
// nothing while idle; transfers only happen while the game idles between tests.
//
// Endpoints:
//   GET  /                     status text
//   GET  /log                  Star3D.log (flushed first)
//   GET  /log.1                the rotated earlier part (Star3D.1.log)
//   GET  /log.prev             the previous boot's log (Star3D.prev.log)
//   GET  /cmd.txt /env.txt /settings.txt   current files
//   GET  /captures             capture folder names, one per line
//   GET  /captures/<dir>       file names in a capture folder
//   GET  /captures/<dir>/<f>   one capture file (binary)
//   PUT  /cmd.txt /env.txt /settings.txt   write the file (Content-Length)
//   PUT  /app.3dsx             write sdmc:/3ds/star3d.3dsx (new build, visible
//                              to the Homebrew Launcher)
//   PUT  /romfs.bin            write sdmc:/3ds/star3d/romfs.bin (game data)
//   GET  /restart              relaunch the app (Rosalina hb:ldr target =
//                              the /app.3dsx build; falls back to a plain exit
//                              back to the Homebrew Launcher)

#include <3ds.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "netsrv_3ds.h"
#include "remote_3ds.h"

#define DATA_DIR "sdmc:/3ds/star3d"
#define APP_PATH "sdmc:/3ds/star3d.3dsx"
#define MAX_REQ 4096
#define MAX_PUT_SMALL 16384
#define CHUNK 65536

static volatile bool s_restart;
static volatile bool s_on;
static int s_port;

bool netsrv_restart_requested(void) { return s_restart; }

// Startup trace: written to the SD at every step, so a boot where the server
// never comes up can be diagnosed from the file alone (via ftpd)
static FILE* s_trace;
static void trace(const char* fmt, ...) {
    if (!s_trace) {
        s_trace = fopen("sdmc:/3ds/star3d/netsrv.txt", "w");
        if (!s_trace) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_trace, fmt, ap);
    va_end(ap);
    fputc('\n', s_trace);
    fflush(s_trace);
}

// Release the soc session (listener included) on every exit path, so the next
// boot's bind() never has to wait out a lingering port
void netsrv_shutdown(void) {
    if (!s_on) return;
    s_on = false;
    socExit();
}

// ---- small helpers ----------------------------------------------------------

static bool name_ok(const char* s) {
    if (!s || !s[0] || strstr(s, "..") || strchr(s, '/') || strchr(s, '\\')) return false;
    size_t n = strlen(s);
    if (n > 96) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
              c == '-'))
            return false;
    }
    return true;
}

// Case-insensitive header lookup (newlib has no strcasestr here)
static char* find_header(char* s, const char* name) {
    size_t n = strlen(name);
    for (char* p = s; *p; p++) {
        if (strncasecmp(p, name, n) == 0) return p + n;
    }
    return NULL;
}

// Recv/send with a 10 s deadline. The 3DS soc service has no SO_RCVTIMEO /
// SO_SNDTIMEO and no fcntl O_NONBLOCK, so readiness comes from select().
static int recv_to(int fd, void* buf, size_t n) {
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv = { 10, 0 };
        int r = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (r <= 0) return -1;  // Timeout or error
        ssize_t got = recv(fd, buf, n, 0);
        if (got > 0) return (int)got;
        if (got == 0) return 0;
        // EAGAIN: loop (select will wait for readability again)
    }
}

static int send_to(int fd, const void* buf, size_t n) {
    size_t off = 0;
    while (off < n) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { 10, 0 };
        int r = select(fd + 1, NULL, &wfds, NULL, &tv);
        if (r <= 0) return -1;
        // Keep each send small so a blocking socket can't stall on a full
        // buffer (select only guarantees *some* room)
        size_t step = n - off > 16384 ? 16384 : n - off;
        ssize_t w = send(fd, (const char*)buf + off, step, 0);
        if (w > 0) {
            off += (size_t)w;
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return -1;
        // EAGAIN: loop again
    }
    return (int)off;
}

static void send_str(int fd, const char* s) {
    send_to(fd, s, strlen(s));
}

static void header(int fd, const char* status, const char* type, u32 len) {
    char h[160];
    snprintf(h, sizeof(h),
             "HTTP/1.0 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", status, type,
             (unsigned)len);
    send_str(fd, h);
}

static void respond(int fd, const char* status, const char* body) {
    header(fd, status, "text/plain", (u32)strlen(body));
    send_str(fd, body);
}

// Stream a file with Content-Length
static void serve_file(int fd, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        respond(fd, "404 Not Found", "no such file\n");
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        respond(fd, "500 Internal Error", "size failed\n");
        return;
    }
    header(fd, "200 OK", "application/octet-stream", (u32)size);
    static u8 buf[CHUNK];
    long left = size;
    while (left > 0) {
        size_t want = left > (long)sizeof(buf) ? sizeof(buf) : (size_t)left;
        size_t got = fread(buf, 1, want, f);
        if (!got) break;
        int w = send_to(fd, buf, got);
        if (w <= 0) break;
        left -= (long)w;
    }
    fclose(f);
}

// Directory listing, one entry per line (names only)
static void serve_listing(int fd, const char* dir) {
    DIR* d = opendir(dir);
    if (!d) {
        respond(fd, "404 Not Found", "no such directory\n");
        return;
    }
    static char out[8192];
    size_t at = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t n = strlen(e->d_name);
        if (at + n + 2 >= sizeof(out)) break;
        memcpy(out + at, e->d_name, n);
        at += n;
        out[at++] = '\n';
    }
    closedir(d);
    out[at] = 0;
    header(fd, "200 OK", "text/plain", (u32)at);
    send_str(fd, out);
}

// PUT: write the request body to "path" via a temp file + rename
static bool save_body(int fd, const char* path, u32 len, bool first_chunk_pending, const char* pending,
                      u32 pending_len) {
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    if (!f) return false;
    static u8 buf[CHUNK];
    if (first_chunk_pending && pending_len) {
        if (fwrite(pending, 1, pending_len, f) != pending_len) {
            fclose(f);
            remove(tmp);
            return false;
        }
        len -= pending_len;
    }
    while (len > 0) {
        size_t want = len > sizeof(buf) ? sizeof(buf) : len;
        int got = recv_to(fd, buf, want);
        if (got <= 0) {
            fclose(f);
            remove(tmp);
            return false;
        }
        if (fwrite(buf, 1, (size_t)got, f) != (size_t)got) {
            fclose(f);
            remove(tmp);
            return false;
        }
        len -= (u32)got;
    }
    if (fclose(f) != 0) {
        remove(tmp);
        return false;
    }
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

// ---- one connection ---------------------------------------------------------

static void handle(int c) {
    static char req[MAX_REQ];
    u32 at = 0;
    while (at + 1 < sizeof(req)) {
        int got = recv_to(c, req + at, sizeof(req) - 1 - at);
        if (got <= 0) return;
        at += (u32)got;
        req[at] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }

    // "METHOD /path HTTP/1.x"
    char method[8] = { 0 };
    char path[160] = { 0 };
    if (sscanf(req, "%7s %159s", method, path) != 2) {
        respond(c, "400 Bad Request", "bad request\n");
        return;
    }
    // Content-Length for PUT
    u32 clen = 0;
    {
        char* cl = find_header(req, "Content-Length:");
        if (cl) clen = (u32)atoi(cl);
    }
    // Body bytes that arrived in the same packet as the headers
    char* body = strstr(req, "\r\n\r\n");
    u32 pending_len = 0;
    if (body) {
        body += 4;
        pending_len = (u32)((req + at) - body);
        if (pending_len > clen) pending_len = clen;
    }

    bool get = !strcmp(method, "GET");
    bool put = !strcmp(method, "PUT");
    if (!get && !put) {
        respond(c, "405 Method Not Allowed", "GET or PUT only\n");
        return;
    }

    if (!strcmp(path, "/")) {
        char st[256];
        snprintf(st, sizeof(st), "Star 3D net server: GET /log /env.txt /settings.txt /cmd.txt /captures[/dir[/file]], "
                                 "PUT /cmd.txt /env.txt /settings.txt /app.3dsx /romfs.bin, GET /restart\n");
        respond(c, "200 OK", st);
        return;
    }
    if (!strcmp(path, "/restart")) {
        s_restart = true;
        LOG_WARN("netsrv", "restart requested over the network");
        respond(c, "200 OK", "restarting\n");
        return;
    }
    if (get && !strcmp(path, "/log")) {
        log_flush();
        serve_file(c, DATA_DIR "/Star3D.log");
        return;
    }
    if (get && !strcmp(path, "/log.1")) {
        log_flush();
        serve_file(c, DATA_DIR "/Star3D.1.log");
        return;
    }
    if (get && !strcmp(path, "/log.prev")) {
        log_flush();
        serve_file(c, DATA_DIR "/Star3D.prev.log");
        return;
    }

    // /captures[/dir[/file]]
    if (!strncmp(path, "/captures", 9) && (path[9] == 0 || path[9] == '/')) {
        if (!get) {
            respond(c, "405 Method Not Allowed", "GET only\n");
            return;
        }
        char rest[160];
        strncpy(rest, path + (path[9] == '/' ? 10 : 9), sizeof(rest) - 1);
        rest[sizeof(rest) - 1] = 0;
        if (!rest[0]) {
            serve_listing(c, DATA_DIR "/captures");
            return;
        }
        char dir[96], file[96];
        dir[0] = file[0] = 0;
        char* slash = strchr(rest, '/');
        if (slash) {
            *slash = 0;
            if (strlen(rest) >= sizeof(dir) || strlen(slash + 1) >= sizeof(file)) {
                respond(c, "403 Forbidden", "bad path\n");
                return;
            }
            snprintf(dir, sizeof(dir), "%s", rest);
            snprintf(file, sizeof(file), "%s", slash + 1);
        } else {
            if (strlen(rest) >= sizeof(dir)) {
                respond(c, "403 Forbidden", "bad path\n");
                return;
            }
            snprintf(dir, sizeof(dir), "%s", rest);
        }
        if (!name_ok(dir) || (file[0] && !name_ok(file))) {
            respond(c, "403 Forbidden", "bad path\n");
            return;
        }
        if (!file[0]) {
            char full[192];
            snprintf(full, sizeof(full), DATA_DIR "/captures/%s", dir);
            serve_listing(c, full);
            return;
        }
        char full[192];
        snprintf(full, sizeof(full), DATA_DIR "/captures/%s/%s", dir, file);
        serve_file(c, full);
        return;
    }

    // Simple names
    char name[96];
    if (strlen(path + 1) >= sizeof(name)) {
        respond(c, "403 Forbidden", "bad path\n");
        return;
    }
    snprintf(name, sizeof(name), "%s", path + 1);
    if (!name_ok(name)) {
        respond(c, "403 Forbidden", "bad path\n");
        return;
    }
    if (get) {
        if (!strcmp(name, "env.txt") || !strcmp(name, "settings.txt") || !strcmp(name, "cmd.txt")) {
            char full[160];
            snprintf(full, sizeof(full), DATA_DIR "/%s", name);
            serve_file(c, full);
            return;
        }
        respond(c, "404 Not Found", "no such file\n");
        return;
    }
    // PUT
    if (!strcmp(name, "cmd.txt") || !strcmp(name, "env.txt") || !strcmp(name, "settings.txt")) {
        if (clen > MAX_PUT_SMALL) {
            respond(c, "413 Payload Too Large", "file too large\n");
            return;
        }
        char full[160];
        snprintf(full, sizeof(full), DATA_DIR "/%s", name);
        bool ok = save_body(c, full, clen, pending_len > 0, body, pending_len);
        LOG_WARN("netsrv", "PUT %s (%u bytes): %s", name, (unsigned)clen, ok ? "ok" : "failed");
        if (ok && !strcmp(name, "cmd.txt")) remote_poll_now();
        respond(c, ok ? "200 OK" : "500 Internal Error", ok ? "saved\n" : "write failed\n");
        return;
    }
    if (!strcmp(name, "app.3dsx")) {
        bool ok = save_body(c, APP_PATH, clen, pending_len > 0, body, pending_len);
        LOG_WARN("netsrv", "PUT app.3dsx (%u bytes): %s", (unsigned)clen, ok ? "ok" : "failed");
        respond(c, ok ? "200 OK" : "500 Internal Error", ok ? "saved\n" : "write failed\n");
        return;
    }
    if (!strcmp(name, "romfs.bin")) {
        char full[160];
        snprintf(full, sizeof(full), DATA_DIR "/romfs.bin");
        bool ok = save_body(c, full, clen, pending_len > 0, body, pending_len);
        LOG_WARN("netsrv", "PUT romfs.bin (%u bytes): %s", (unsigned)clen, ok ? "ok" : "failed");
        respond(c, ok ? "200 OK" : "500 Internal Error", ok ? "saved\n" : "write failed\n");
        return;
    }
    respond(c, "404 Not Found", "no such target\n");
}

// ---- thread + init ----------------------------------------------------------

static void server_main(void* arg) {
    (void)arg;
    trace("server: thread entered");
    LOG_WARN("netsrv", "server thread starting (port %d)", s_port);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    trace("server: socket -> %d", fd);
    if (fd < 0) {
        LOG_ERROR("netsrv", "socket() failed: %d", errno);
        return;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u16)s_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        // After a relaunch the previous process's listening socket can linger
        // in the soc service: keep retrying until it frees up
        int attempt = 0;
        for (;;) {
            trace("server: bind attempt %d failed: %d", attempt + 1, errno);
            if (attempt < 4 || attempt % 20 == 0)
                LOG_WARN("netsrv", "bind(:%d) failed: %d (attempt %d, retrying)", s_port, errno, attempt + 1);
            attempt++;
            svcSleepThread(3 * 1000 * 1000 * 1000);
            if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) break;
        }
        trace("server: bind succeeded after %d retries", attempt);
        if (attempt) LOG_WARN("netsrv", "bind(:%d) succeeded after %d retries", s_port, attempt);
    }
    if (listen(fd, 1) < 0) {
        LOG_ERROR("netsrv", "listen() failed: %d", errno);
        trace("server: listen failed: %d", errno);
        close(fd);
        return;
    }
    LOG_WARN("netsrv", "server on port %d (core 3): http://<console>:%d/", s_port, s_port);
    trace("server: listening on %d", s_port);
    for (;;) {
        struct sockaddr_in cli;
        socklen_t cl = sizeof(cli);
        int c = accept(fd, (struct sockaddr*)&cli, &cl);
        if (c < 0) continue;
        handle(c);
        close(c);
    }
}

void netsrv_init(void) {
    trace("init: entered");
    if (!cfg_int("S3D_NET_SERVER", 0)) {
        trace("init: S3D_NET_SERVER off, exiting");
        return;
    }
    s_port = cfg_int("S3D_NET_PORT", 8080);
    trace("init: port %d", s_port);
    LOG_WARN("netsrv", "net server on: port %d", s_port);
    static u32 soc_buf[0x100000 / sizeof(u32)] __attribute__((aligned(0x1000)));
    Result rc = socInit(soc_buf, sizeof(soc_buf));
    trace("init: socInit -> 0x%08lX", (unsigned long)rc);
    if (R_FAILED(rc)) {
        LOG_ERROR("netsrv", "socInit failed (0x%08lX): no net server", (unsigned long)rc);
        return;
    }
    s_on = true;
    // Core 3 first (game 0, audio/helper 1, render 2); fall back if the OS
    // denies that core in this launch context
    static const int cores[] = { 3, 2, -2 };
    for (int i = 0; i < 3; i++) {
        Thread t = threadCreate(server_main, NULL, 64 * 1024, 0x30, cores[i], false);
        trace("init: threadCreate core %d -> %p", cores[i], (void*)t);
        if (t) {
            LOG_WARN("netsrv", "server thread on core %d", cores[i]);
            return;
        }
        LOG_WARN("netsrv", "threadCreate on core %d failed: trying the next core", cores[i]);
    }
    LOG_ERROR("netsrv", "no server thread: core 3/2/default all failed");
}

// Relaunch the app with the new build (PUT /app.3dsx): Rosalina's hb:ldr
// service — the same mechanism hbmenu uses to launch a 3dsx — gets the new
// path as its target, and the loader launches it when this app exits. Without
// Rosalina the port connect fails and the exit just returns to hbmenu.
void netsrv_relaunch_app(void) {
    // Let the soc service release the listening socket so the relaunched
    // app's bind() does not hit a lingering port
    netsrv_shutdown();
    static Handle hbldr;
    Result rc = svcConnectToPort(&hbldr, "hb:ldr");
    if (R_SUCCEEDED(rc)) {
        const char* path = APP_PATH;
        if (strncmp(path, "sdmc:/", 6) == 0) path += 5;
        u32 pathLen = strlen(path) + 1;
        u32* cmdbuf = getThreadCommandBuffer();
        cmdbuf[0] = IPC_MakeHeader(2, 0, 2);  // HBLDR_SetTarget
        cmdbuf[1] = IPC_Desc_StaticBuffer(pathLen, 0);
        cmdbuf[2] = (u32)path;
        rc = svcSendSyncRequest(hbldr);
        if (R_SUCCEEDED(rc)) rc = cmdbuf[1];
        svcCloseHandle(hbldr);
        LOG_WARN("netsrv", "hb:ldr target %s: 0x%08lX", path, (unsigned long)rc);
    } else {
        LOG_WARN("netsrv", "hb:ldr connect failed (0x%08lX): plain exit", (unsigned long)rc);
    }
}
