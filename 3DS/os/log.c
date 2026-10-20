#include "log.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/stat.h>

static FILE* log_file = NULL;
static LightLock log_lock;
static bool log_ready = false;
static LogLevel console_level = LOG_LEVEL_INFO;
static unsigned int log_frame = 0;
// Touch UI "Logging" switch: when off, only warnings and errors are written
static volatile bool log_verbose = true;
static u64 start_tick = 0;
static u32 log_bytes = 0;
static LogOnceSite* once_sites = NULL;

static const char* const level_names[] = { "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR" };

// --- Background writer ---------------------------------------------------------
// SD card writes stall for 0.3-1.5 s at times on hardware (frame trace
// 7AED0EF9: the profiler report's ~110 lines, flushed on the game thread).
// Lines go into a memory ring instead; a writer thread on core 0, below the
// game thread's priority, writes them out. It wakes on a timeout, on warnings
// and when the ring fills up. log_flush() still writes everything before it
// returns (debug paths, watchdog); log_kick() only wakes the writer. The file
// is only touched under s_file_lock (writer, log_flush, rotation, shutdown),
// or without locks by the crash handler.
#define RING_SIZE (256 * 1024)
static char s_ring[RING_SIZE];
static volatile u32 s_head, s_tail;  // Bytes written / bytes taken (free-running)
static LightLock s_file_lock;
static LightEvent s_writer_ev;
static Thread s_writer;
static volatile bool s_writer_quit;
static u32 s_dropped;

// Appends to the ring; caller holds log_lock
static void ring_put(const char* p, u32 len) {
    u32 at = s_head % RING_SIZE, first = RING_SIZE - at < len ? RING_SIZE - at : len;
    memcpy(s_ring + at, p, first);
    memcpy(s_ring, p + first, len - first);
    __dmb();  // The bytes before the index that publishes them
    s_head += len;
}

// Writes what is in the ring to the file. Caller holds s_file_lock (or is the
// crash handler). Producers only append past s_head, so the bytes between
// s_tail and s_head stay put until s_tail moves
static void drain_ring(void) {
    for (;;) {
        u32 tail = s_tail, head = s_head;
        __dmb();
        if (tail == head || !log_file) return;
        u32 at = tail % RING_SIZE, len = head - tail;
        if (len > RING_SIZE - at) len = RING_SIZE - at;
        if (log_bytes + len > LOG_FILE_CAP) {
            // Rotate: keep this part as Star3D.1.log and start over
            fclose(log_file);
            remove(LOG_FILE_OLD);
            rename(LOG_FILE_PATH, LOG_FILE_OLD);
            log_file = fopen(LOG_FILE_PATH, "w");
            if (!log_file) return;
            setvbuf(log_file, NULL, _IOFBF, 32 * 1024);
            log_bytes = fprintf(log_file, "[log rotated at frame %u, earlier lines are in %s]%c", log_frame,
                                LOG_FILE_OLD, '\n');
        }
        fwrite(s_ring + at, 1, len, log_file);
        log_bytes += len;
        __dmb();
        s_tail = tail + len;
    }
}

static void writer_main(void* arg) {
    (void)arg;
    while (!s_writer_quit) {
        LightEvent_WaitTimeout(&s_writer_ev, 1000000000LL);  // At least once a second
        LightLock_Lock(&s_file_lock);
        u64 t0 = svcGetSystemTick();
        u32 bytes = s_head - s_tail;
        drain_ring();
        if (log_file) fflush(log_file);
        u64 ms = (svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000);
        LightLock_Unlock(&s_file_lock);
        // Slow SD writes (BFDF5C33 HW: 3 s frame stalls every ~13 s, SD or not?)
        if (ms > 250) LOG_WARN("log", "SD write of %lu bytes took %lu ms", (unsigned long)bytes, (unsigned long)ms);
    }
}

static void log_start_writer(void) {
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    // Below the game thread, on its core: it runs while the game waits, and
    // mostly sleeps in the SD card driver
    s_writer = threadCreate(writer_main, NULL, 16 * 1024, prio < 0x3F ? prio + 1 : prio, 0, false);
    if (!s_writer) LOG_WARN("log", "no log writer thread: writing on the calling threads");
}

void log_init(void) {
    if (log_ready) return;

    LightLock_Init(&log_lock);
    LightLock_Init(&s_file_lock);
    LightEvent_Init(&s_writer_ev, RESET_ONESHOT);
    start_tick = svcGetSystemTick();

    mkdir("sdmc:/3ds", 0777);
    mkdir(LOG_FILE_DIR, 0777);
    // Keep the previous run (e.g. the one that crashed) next to the new log
    remove(LOG_FILE_PREV);
    remove(LOG_FILE_OLD);
    if (rename(LOG_FILE_PATH, LOG_FILE_PREV) != 0) rename(LOG_FILE_LEGACY, LOG_FILE_PREV);
    remove(LOG_FILE_LEGACY);
    log_file = fopen(LOG_FILE_PATH, "w");
    // Large stdio buffer: SD card writes are slow, flushes happen on warnings,
    // every 2 s (watchdog) and in the crash handler
    if (log_file) setvbuf(log_file, NULL, _IOFBF, 32 * 1024);
    log_ready = true;
    log_start_writer();

    LOG_INFO("log", "Star 3D log, build %s %s", __DATE__, __TIME__);
    if (!log_file) {
        LOG_WARN("log", "could not open %s, logging to debug output only", LOG_FILE_PATH);
    }
}

void log_shutdown(void) {
    if (!log_ready) return;

    if (s_dropped) LOG_WARN("log", "%lu lines dropped (log ring full)", (unsigned long)s_dropped);
    LOG_INFO("log", "log closed");
    if (s_writer) {
        s_writer_quit = true;
        LightEvent_Signal(&s_writer_ev);
        threadJoin(s_writer, 1000000000ULL);
        threadFree(s_writer);
        s_writer = NULL;
    }
    LightLock_Lock(&s_file_lock);
    drain_ring();
    LightLock_Lock(&log_lock);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    log_ready = false;
    LightLock_Unlock(&log_lock);
    LightLock_Unlock(&s_file_lock);
}

void log_flush(void) {
    if (!log_ready) return;
    LightLock_Lock(&s_file_lock);
    drain_ring();
    if (log_file) fflush(log_file);
    LightLock_Unlock(&s_file_lock);
}

void log_kick(void) {
    if (log_ready) LightEvent_Signal(&s_writer_ev);
}

// Crash path: the faulting thread may hold the locks, so write without them,
// after whatever is still in the ring
void log_write_unlocked(const char* text) {
    size_t len = strlen(text);
    svcOutputDebugString(text, len);
    drain_ring();
    if (log_file) {
        fputs(text, log_file);
        fputc('\n', log_file);
    }
}

void log_flush_unlocked(void) {
    drain_ring();
    if (log_file) fflush(log_file);
}

void log_set_console_level(LogLevel level) {
    console_level = level;
}

void log_set_frame(unsigned int frame) {
    log_frame = frame;
}

void log_set_verbose(bool on) {
    log_verbose = on;
}

bool log_get_verbose(void) {
    return log_verbose;
}

static FILE* log_tee;

void log_set_tee(void* file) {
    LightLock_Lock(&log_lock);
    log_tee = (FILE*)file;
    LightLock_Unlock(&log_lock);
}

void log_vwrite(LogLevel level, const char* tag, const char* fmt, va_list args) {
    if (!log_verbose && level < LOG_LEVEL_WARN && !log_tee) return;
    char line[512];
    u64 elapsed_ms = (svcGetSystemTick() - start_tick) / (u64)CPU_TICKS_PER_MSEC;

    int prefix = snprintf(line, sizeof(line), "[%6llu.%03llu f%-6u %s %-8s] ",
                          elapsed_ms / 1000, elapsed_ms % 1000, log_frame,
                          level_names[level], tag ? tag : "");
    if (prefix < 0) prefix = 0;
    if (prefix > (int)sizeof(line) - 2) prefix = sizeof(line) - 2;

    vsnprintf(line + prefix, sizeof(line) - prefix - 1, fmt, args);

    // Always end with exactly one newline (OSReport text often has its own)
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    line[len++] = '\n';
    line[len] = '\0';

    if (!log_ready) {
        // Before log_init: debug output only
        svcOutputDebugString(line, len);
        return;
    }

    LightLock_Lock(&log_lock);

    svcOutputDebugString(line, len);

    bool to_log = !log_tee || level >= LOG_LEVEL_WARN;
    if (log_tee) fputs(line, log_tee);
    bool full = false, wake = false;
    if (log_file && to_log) {
        u32 used = s_head - s_tail;
        if (used + len > RING_SIZE) {
            full = true;
        } else {
            ring_put(line, len);
            // Problems go out at once (they may come just before a crash or a hang)
            wake = level >= LOG_LEVEL_WARN || used + len > RING_SIZE / 4;
        }
    }

    if (level >= console_level) {
        fputs(line + prefix, stdout);
    }

    LightLock_Unlock(&log_lock);
    if (wake && s_writer) LightEvent_Signal(&s_writer_ev);
    if (full) {
        // The writer fell behind: write the ring out here
        log_flush();
        LightLock_Lock(&log_lock);
        if (s_head - s_tail + len <= RING_SIZE) ring_put(line, len);
        else s_dropped++;
        LightLock_Unlock(&log_lock);
    }
    if (!s_writer && log_file) {
        // No writer thread (before it starts, or it could not be created):
        // write on this thread, flushing problems at once as before
        LightLock_Lock(&s_file_lock);
        drain_ring();
        if (level >= LOG_LEVEL_WARN) fflush(log_file);
        LightLock_Unlock(&s_file_lock);
    }
}

// --- counted LOG_ONCE --------------------------------------------------------

void log_once_first(LogOnceSite* site, LogLevel level, const char* tag, const char* file, int line,
                    const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(site->msg, sizeof(site->msg), fmt, args);
    va_end(args);
    const char* base = strrchr(file, '/');
    site->tag = tag;
    site->file = base ? base + 1 : file;
    site->line = line;
    site->reported = 1;
    if (log_ready) LightLock_Lock(&log_lock);
    site->next = once_sites;
    once_sites = site;
    if (log_ready) LightLock_Unlock(&log_lock);
    log_write(level, tag, "%s", site->msg);
}

void log_once_report(bool all) {
    if (!log_ready) return;
    // Walk a snapshot of the list head; sites are only ever prepended
    LightLock_Lock(&log_lock);
    LogOnceSite* head = once_sites;
    LightLock_Unlock(&log_lock);
    for (LogOnceSite* s = head; s; s = s->next) {
        unsigned int hits = s->hits;
        bool grew = hits != s->reported;
        // Periodic report: only sites that start (or restart) firing, so the
        // ones hit every frame show up once instead of in every report
        if (all || (grew && !s->active))
            LOG_WARN("once", "%s:%d x%u (+%u) %s: %s", s->file, s->line, hits, hits - s->reported,
                     s->tag ? s->tag : "", s->msg);
        if (all) continue;  // the mark snapshot leaves the periodic state alone
        s->active = grew;
        s->reported = hits;
    }
}

// --- stderr capture ---------------------------------------------------------
// foxhollow's shims report problems with fprintf(stderr, ...); route those
// lines into the log instead of the console.

static char stderr_line[256];
static size_t stderr_len;

static ssize_t stderr_write(struct _reent* r, void* fd, const char* ptr, size_t len) {
    (void)r;
    (void)fd;
    for (size_t i = 0; i < len; i++) {
        char c = ptr[i];
        if (c == '\n' || stderr_len == sizeof(stderr_line) - 1) {
            stderr_line[stderr_len] = '\0';
            if (stderr_len > 0) LOG_WARN("stderr", "%s", stderr_line);
            stderr_len = 0;
            if (c == '\n') continue;
        }
        stderr_line[stderr_len++] = c;
    }
    return len;
}

static const devoptab_t stderr_devoptab = {
    .name = "stderr_log",
    .write_r = stderr_write,
};

void log_capture_stderr(void) {
    devoptab_list[STD_ERR] = &stderr_devoptab;
    setvbuf(stderr, NULL, _IONBF, 0);
}

void log_write(LogLevel level, const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_vwrite(level, tag, fmt, args);
    va_end(args);
}
