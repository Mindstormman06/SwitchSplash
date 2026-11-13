#pragma once
#include <switch.h>

/* Atmosphère cfg::OverrideStatus (libraries/libstratosphere/include/stratosphere/cfg/cfg_types.hpp). */
typedef struct {
    u64 keys_held;
    u64 flags;
} AmsOverrideStatus;

#define AMS_OVERRIDE_FLAG_HBL              BIT(0)
#define AMS_OVERRIDE_FLAG_PROGRAM_SPECIFIC BIT(1)
#define AMS_OVERRIDE_FLAG_CHEAT_ENABLED    BIT(2)

typedef enum {
    AppEvent_None = 0,
    AppEvent_Launched,   /* A new application process appeared (cold launch). */
    AppEvent_Exited,     /* The application process we were tracking is gone. */
} AppEventType;

typedef struct {
    AppEventType type;
    u64 pid;
    u64 program_id;
    u64 tick;                 /* armGetSystemTick() when the change was observed. */
    bool has_ams_info;        /* AtmosphereGetProcessInfo succeeded. */
    u8 storage_id;            /* NcmStorageId the program was launched from. */
    AmsOverrideStatus status;
} AppEvent;

Result appMonitorInitialize(void);
void appMonitorExit(void);

/* Polls pm once. Returns true and fills *out if the application process changed.
 * When a game is closed and another is launched between two polls, the next poll
 * reports Exited and the one after that reports Launched. */
bool appMonitorPoll(AppEvent *out);
