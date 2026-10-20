#include "config.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

#define MAX_SWITCHES 192

typedef struct {
    const char* name;
    int value;
    bool is_bool;
    bool from_env;
} Switch;

// The game and render threads both look switches up (the render thread
// overlaps the game's frame boundary, where the touch UI sets them). Entries
// below s_count are complete; new ones are added under s_lock.
static Switch s_switches[MAX_SWITCHES];
static volatile int s_count;
static LightLock s_lock = 1;  // Unlocked (LightLock_Init stores 1)

static Switch* find(const char* name, int from, int to) {
    for (int i = from; i < to; i++) {
        if (strcmp(s_switches[i].name, name) == 0) return &s_switches[i];
    }
    return NULL;
}

static Switch* lookup(const char* name, int def, bool is_bool) {
    int n = s_count;
    Switch* found = find(name, 0, n);
    if (found) return found;
    LightLock_Lock(&s_lock);
    found = find(name, n, s_count);
    if (found) {
        LightLock_Unlock(&s_lock);
        return found;
    }
    const char* env = getenv(name);
    int value = def;
    if (env && *env) {
        if (is_bool) {
            value = !(env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F');
        } else {
            value = (int)strtol(env, NULL, 0);
        }
    }
    if (s_count == MAX_SWITCHES) {
        // Shared by every name past the table: such settings can't be changed live
        static Switch overflow;
        static bool warned;
        if (!warned) {
            warned = true;
            LOG_WARN("config", "more than %d settings, '%s' and later ones can't be changed (raise MAX_SWITCHES)",
                     MAX_SWITCHES, name);
        }
        overflow.value = value;
        LightLock_Unlock(&s_lock);
        return &overflow;
    }
    Switch* s = &s_switches[s_count];
    s->name = name;
    s->value = value;
    s->is_bool = is_bool;
    s->from_env = env && *env;
    __dmb();  // Complete before other threads can see it
    s_count++;
    LightLock_Unlock(&s_lock);
    return s;
}

bool cfg_bool(const char* name, bool def) {
    return lookup(name, def, true)->value != 0;
}

int cfg_int(const char* name, int def) {
    return lookup(name, def, false)->value;
}

static unsigned s_gen = 1;

unsigned cfg_gen(void) { return s_gen; }

// Every change is logged ("config" tag): touch UI, remote commands, the GPU
// sweep, so a log shows which setting was active when
void cfg_set(const char* name, int value) {
    Switch* s = lookup(name, value, false);
    if (s->value == value) return;
    LOG_INFO("config", "set %s %d -> %d", name, s->value, value);
    s->value = value;
    s_gen++;
}

void cfg_log_summary(void) {
    char line[400];
    int n = 0;
    line[0] = 0;
    for (int i = 0; i < s_count; i++) {
        const Switch* s = &s_switches[i];
        int w = snprintf(line + n, sizeof(line) - n, "%s%s=%d%s", n ? " " : "", s->name, s->value,
                         s->from_env ? "*" : "");
        if (w < 0 || n + w >= (int)sizeof(line) - 1) {
            LOG_INFO("config", "%s", line);
            n = 0;
            line[0] = 0;
            i--;
            continue;
        }
        n += w;
    }
    if (n) LOG_INFO("config", "%s", line);
    LOG_INFO("config", "(* = set in env.txt; others are defaults)");
}
