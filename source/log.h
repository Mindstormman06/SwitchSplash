#pragma once
#include <switch.h>

#define BOOTSPLASH_DIR      "sdmc:/config/bootsplash"
#define BOOTSPLASH_LOG_PATH BOOTSPLASH_DIR "/bootsplash.log"

/* Creates the config dir, rotates the log if it has grown too large. */
void logInit(void);

/* Starts the background writer thread. Lines logged before this are kept and written
 * once it runs. */
void logStart(void);

/* Appends one line: "[  12345.678] <message>\n". Thread-safe and fast: the line is
 * formatted into memory (timestamped now) and written to SD later by the writer thread. */
void logWrite(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* While held (nesting allowed), the writer thread doesn't touch the SD card, so a launch's
 * SD reads and CPU time go to the splash. Lines keep buffering. */
void logHold(bool hold);
