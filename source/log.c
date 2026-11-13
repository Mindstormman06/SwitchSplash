#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#define LOG_MAX_SIZE (256 * 1024)

static Mutex g_log_mutex;

void logInit(void) {
    mutexInit(&g_log_mutex);

    mkdir("sdmc:/config", 0777);
    mkdir(BOOTSPLASH_DIR, 0777);

    struct stat st;
    if (stat(BOOTSPLASH_LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_SIZE) {
        /* Keep one previous log around for comparison. */
        remove(BOOTSPLASH_LOG_PATH ".old");
        rename(BOOTSPLASH_LOG_PATH, BOOTSPLASH_LOG_PATH ".old");
    }
}

void logWrite(const char *fmt, ...) {
    mutexLock(&g_log_mutex);

    FILE *f = fopen(BOOTSPLASH_LOG_PATH, "a");
    if (f) {
        /* Milliseconds since boot: comparable across lines, and against a video of the screen. */
        const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
        fprintf(f, "[%8lu.%03lu] ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));

        va_list args;
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);

        fputc('\n', f);
        fclose(f);
    }

    mutexUnlock(&g_log_mutex);
}
