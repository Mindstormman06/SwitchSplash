#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/*
 * Lines are formatted into a memory buffer (timestamped at the call) and written to SD by
 * a low-priority thread, so logging never sits on the launch -> first-frame path.
 * logHold() pauses the SD writes while a splash is being put on screen.
 */

#define LOG_MAX_SIZE   (256 * 1024)
#define LOG_BUF_SIZE   (16 * 1024)
#define FLUSH_INTERVAL 250000000ULL /* 250 ms */

static Mutex g_log_mutex;
static CondVar g_log_cond;
static char g_buf[LOG_BUF_SIZE];
static size_t g_len = 0;
static u32 g_dropped = 0;
static int g_hold = 0;

static char g_out[LOG_BUF_SIZE]; /* writer-thread copy */
static Thread g_thread;
alignas(0x1000) static u8 g_stack[0x3000];

void logInit(void) {
    mutexInit(&g_log_mutex);
    condvarInit(&g_log_cond);

    mkdir("sdmc:/config", 0777);
    mkdir(BOOTSPLASH_DIR, 0777);

    struct stat st;
    if (stat(BOOTSPLASH_LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_SIZE) {
        /* Keep one previous log around for comparison. */
        remove(BOOTSPLASH_LOG_PATH ".old");
        rename(BOOTSPLASH_LOG_PATH, BOOTSPLASH_LOG_PATH ".old");
    }
}

static void writeOut(const char *data, size_t len) {
    FILE *f = fopen(BOOTSPLASH_LOG_PATH, "a");
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

static void writerThread(void *arg) {
    for (;;) {
        mutexLock(&g_log_mutex);
        condvarWaitTimeout(&g_log_cond, &g_log_mutex, FLUSH_INTERVAL);
        size_t len = 0;
        u32 dropped = 0;
        if (g_hold == 0 && g_len > 0) {
            memcpy(g_out, g_buf, g_len);
            len = g_len;
            dropped = g_dropped;
            g_len = 0;
            g_dropped = 0;
        }
        mutexUnlock(&g_log_mutex);

        if (len) {
            writeOut(g_out, len);
            if (dropped) {
                char note[64];
                const int n = snprintf(note, sizeof(note), "[log] %u lines dropped (buffer full)\n", dropped);
                writeOut(note, (size_t)n);
            }
        }
    }
}

void logStart(void) {
    /* Lowest priority we're allowed (npdm: 24-63): SD writes yield to everything else. */
    if (R_SUCCEEDED(threadCreate(&g_thread, writerThread, NULL, g_stack, sizeof(g_stack), 0x3F, -2)))
        threadStart(&g_thread);
}

void logHold(bool hold) {
    mutexLock(&g_log_mutex);
    g_hold += hold ? 1 : -1;
    if (g_hold < 0)
        g_hold = 0;
    if (g_hold == 0)
        condvarWakeOne(&g_log_cond);
    mutexUnlock(&g_log_mutex);
}

void logWrite(const char *fmt, ...) {
    char line[256];
    /* Milliseconds since boot: comparable across lines, and against a video of the screen. */
    const u64 ms = armTicksToNs(armGetSystemTick()) / 1000000ULL;
    int n = snprintf(line, sizeof(line), "[%8lu.%03lu] ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));

    va_list args;
    va_start(args, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, args);
    va_end(args);
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';

    mutexLock(&g_log_mutex);
    if (g_len + (size_t)n <= sizeof(g_buf)) {
        memcpy(g_buf + g_len, line, (size_t)n);
        g_len += (size_t)n;
    } else {
        g_dropped++;
    }
    mutexUnlock(&g_log_mutex);
}
