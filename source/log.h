#pragma once
#include <switch.h>

#define BOOTSPLASH_DIR      "sdmc:/config/bootsplash"
#define BOOTSPLASH_LOG_PATH BOOTSPLASH_DIR "/bootsplash.log"

/* Creates the config dir, truncates the log if it has grown too large. */
void logInit(void);

/* Appends one line: "[  12345.678] <message>\n". Thread-safe.
 * The file is opened and closed per line so nothing is lost if we crash. */
void logWrite(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
