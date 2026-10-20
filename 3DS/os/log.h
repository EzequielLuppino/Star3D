#ifndef LOG_H
#define LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>
#include <stdbool.h>

// Log destinations:
//  - sdmc:/3ds/star3d/Star3D.log (on Azahar:
//    %APPDATA%\Azahar\sdmc\3ds\star3d\Star3D.log). Errors, the
//    crash handler's register dump, benchmarks and the periodic performance
//    reports all go here. The previous run's log is kept as
//    sdmc:/3ds/star3d/Star3D.prev.log.
//  - svcOutputDebugString (shows in the Azahar log as Debug.Emulated)
//  - the bottom screen console, for LOG_LEVEL_INFO and above
#define LOG_FILE_DIR  "sdmc:/3ds/star3d"
#define LOG_FILE_PATH LOG_FILE_DIR "/Star3D.log"
// Where builds before 2026-10-11 wrote the log; removed at startup
#define LOG_FILE_LEGACY "sdmc:/Star3D.log"
#define LOG_FILE_PREV LOG_FILE_DIR "/Star3D.prev.log"
// When the log passes LOG_FILE_CAP bytes it is moved to LOG_FILE_OLD (replacing
// the older one) and a fresh file is started, so long sessions keep the last
// 4-8 MB instead of filling the SD card
#define LOG_FILE_OLD  LOG_FILE_DIR "/Star3D.1.log"
#define LOG_FILE_CAP  (4u * 1024 * 1024)

typedef enum {
    LOG_LEVEL_TRACE,
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} LogLevel;

void log_init(void);
void log_shutdown(void);
void log_flush(void);  // Writes everything logged so far to the card before it returns
void log_kick(void);   // Lets the background writer write what is pending (does not wait)
void log_set_console_level(LogLevel level);
void log_set_verbose(bool on);  // false: keep only warnings and errors
bool log_get_verbose(void);
void log_capture_stderr(void);  // Route stderr output into the log
void log_write_unlocked(const char* text);  // Crash handler only
void log_flush_unlocked(void);

// Frame counter shown in every log line
void log_set_frame(unsigned int frame);

void log_write(LogLevel level, const char* tag, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
void log_vwrite(LogLevel level, const char* tag, const char* fmt, va_list args);
// Debug (render capture): while set, every line, verbose or not, goes to tee
// instead of the log file; warnings and errors go to both. NULL ends it
void log_set_tee(void* file);

#define LOG_TRACE(tag, ...) log_write(LOG_LEVEL_TRACE, tag, __VA_ARGS__)
#define LOG_DEBUG(tag, ...) log_write(LOG_LEVEL_DEBUG, tag, __VA_ARGS__)
#define LOG_INFO(tag, ...)  log_write(LOG_LEVEL_INFO, tag, __VA_ARGS__)
#define LOG_WARN(tag, ...)  log_write(LOG_LEVEL_WARN, tag, __VA_ARGS__)
#define LOG_ERROR(tag, ...) log_write(LOG_LEVEL_ERROR, tag, __VA_ARGS__)

// Log only the first time this line is reached (e.g. for unimplemented calls).
// Later hits are counted: log_once_report() logs (tag "once") the sites that
// start firing again after a quiet report period, so a warning that starts
// firing every frame after a while of play still shows up in the log.
typedef struct LogOnceSite {
    struct LogOnceSite* next;
    const char* tag;
    const char* file;
    int line;
    volatile unsigned int hits;
    unsigned int reported;  // hits at the last report
    bool active;            // grew since the report before that
    char msg[96];           // first message
} LogOnceSite;

void log_once_first(LogOnceSite* site, LogLevel level, const char* tag, const char* file, int line,
                    const char* fmt, ...) __attribute__((format(printf, 6, 7)));
// all: every site (mark snapshot); otherwise only the ones that started firing
void log_once_report(bool all);

#define LOG_ONCE(level, tag, ...) do { \
        static LogOnceSite log_once_site_; \
        if (log_once_site_.hits++ == 0) \
            log_once_first(&log_once_site_, level, tag, __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif // LOG_H
