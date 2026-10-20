#ifndef PORT_CONFIG_H
#define PORT_CONFIG_H

// Runtime feature switches. Every optional renderer/port feature reads one of
// these, so a feature can be turned off on hardware without a rebuild: put
// NAME=0 (or 1) in sdmc:/3ds/star3d/env.txt. Values are read with getenv
// (main.c loads env.txt into the environment first). Every switch that was
// queried is listed in the log by cfg_log_summary().

#include <stdbool.h>

bool cfg_bool(const char* name, bool def);
int cfg_int(const char* name, int def);
void cfg_log_summary(void);

// Live settings (touch UI): cfg_set changes a switch and bumps the
// generation, so callers that cache a switch re-read it. CFG_LIVE(var, expr)
// re-evaluates expr into var once per generation.
void cfg_set(const char* name, int value);
unsigned cfg_gen(void);
#define CFG_LIVE(var, expr)                                       do {                                                              static unsigned var##_gen_;                                   if (var##_gen_ != cfg_gen()) {                                    var##_gen_ = cfg_gen();                                       var = (expr);                                             }                                                         } while (0)

#endif // PORT_CONFIG_H
