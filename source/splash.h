#pragma once
#include <switch.h>

#include "config.h"

typedef enum {
    SplashImage_None = 0,
    SplashImage_Title,    /* /config/bootsplash/<TID>/splash.png */
    SplashImage_Default,  /* /config/bootsplash/default/splash.png */
    SplashImage_Hbl,      /* /config/bootsplash/hbl/splash.png */
} SplashImageSource;

/* Looks for the title's splash image, then the default one. Writes the path to `out`
 * (empty string when none). Titles with neither get no splash at all.
 * For hbmenu title takeover (`hbl`), only hbl/ is used: hbmenu isn't the game, so it
 * never gets the game's art or the default. */
SplashImageSource splashFindImage(u64 program_id, bool hbl, char *out, size_t out_size);

/* Starts the splash for an application on the splash thread. `image_path` NULL means
 * draw the test pattern. Returns false (and logs) if a splash is already running.
 * `launch_tick` is when the launch was observed and is used to log latency. */
bool splashStart(u64 pid, u64 program_id, u64 launch_tick, const Config *cfg, const char *image_path);

/* Asks a running splash for `pid` to tear down early (the app exited). */
void splashCancel(u64 pid);

/* Joins a finished splash thread. Called from the main loop. */
void splashReap(void);
