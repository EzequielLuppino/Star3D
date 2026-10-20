# Input traces advance once per video frame. The game also calls padUpdate in
# its DVD wait loops; with asynchronous reads those spin thousands of times per
# frame and used to eat the whole trace during the first loads.
s|^static int fhInputTraceReplay(PADStatus\* statuses) {$|static int fhInputTraceReplayRead(PADStatus* statuses);\
u32 VIGetRetraceCount(void);\
static int fhInputTraceReplay(PADStatus* statuses) {\
    static PADStatus last[4];\
    static u32 lastRetrace = 0xFFFFFFFFu;\
    static int lastResult;\
    u32 retrace = VIGetRetraceCount();\
    if (gFhInputReplayFile != NULL \&\& retrace == lastRetrace) {\
        memcpy(statuses, last, sizeof(last));\
        return lastResult;\
    }\
    lastRetrace = retrace;\
    lastResult = fhInputTraceReplayRead(statuses);\
    memcpy(last, statuses, sizeof(last));\
    return lastResult;\
}\
static int fhInputTraceReplayRead(PADStatus* statuses) {|
