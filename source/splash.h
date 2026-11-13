#pragma once
#include <switch.h>

#include "config.h"

/* Starts the splash for an application on the splash thread. Returns false (and logs)
 * if a splash is already running. `launch_tick` is when the launch was observed and is
 * used to log latency. */
bool splashStart(u64 pid, u64 program_id, u64 launch_tick, const Config *cfg);

/* Asks a running splash for `pid` to tear down early (the app exited). */
void splashCancel(u64 pid);

/* Joins a finished splash thread. Called from the main loop. */
void splashReap(void);
