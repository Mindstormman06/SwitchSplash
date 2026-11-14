#include "focus_watch.h"
#include "log.h"

#include <stdatomic.h>

static atomic_ullong g_home_tick = 0;
static atomic_ullong g_power_tick = 0;

static Thread g_thread;
alignas(0x1000) static u8 g_stack[0x4000];

u64 focusLastHomeTick(void) {
    return atomic_load(&g_home_tick);
}

u64 focusLastPowerTick(void) {
    return atomic_load(&g_power_tick);
}

static void buttonThread(void *arg) {
    /* We start at boot2; hid:sys may not be ready yet, so retry. */
    Result rc;
    for (int attempt = 0;; attempt++) {
        rc = hidsysInitialize();
        if (R_SUCCEEDED(rc))
            break;
        if (attempt == 0)
            logWrite("INPUT  hidsysInitialize failed: 0x%x, retrying", rc);
        svcSleepThread(1000000000ULL);
    }

    Event home = {0}, power = {0};
    const Result rc_home = hidsysAcquireHomeButtonEventHandle(&home, false);
    const Result rc_power = hidsysAcquireSleepButtonEventHandle(&power, false);
    logWrite("INPUT  button events acquired: home=0x%x power=0x%x", rc_home, rc_power);
    if (R_FAILED(rc_home) && R_FAILED(rc_power))
        return;
    if (R_SUCCEEDED(rc_home)) eventClear(&home);
    if (R_SUCCEEDED(rc_power)) eventClear(&power);

    Waiter waiters[2];
    s32 n = 0;
    Event *events[2];
    if (R_SUCCEEDED(rc_home))  { events[n] = &home;  waiters[n++] = waiterForEvent(&home); }
    if (R_SUCCEEDED(rc_power)) { events[n] = &power; waiters[n++] = waiterForEvent(&power); }

    for (;;) {
        s32 idx = -1;
        rc = waitObjects(&idx, waiters, n, UINT64_MAX);
        if (R_FAILED(rc) || idx < 0 || idx >= n) {
            svcSleepThread(10000000ULL);
            continue;
        }
        eventClear(events[idx]);
        const u64 now = armGetSystemTick();
        if (events[idx] == &home) {
            atomic_store(&g_home_tick, now);
            logWrite("INPUT  HOME pressed");
        } else {
            atomic_store(&g_power_tick, now);
            logWrite("INPUT  power button pressed");
        }
    }
}

void focusWatchStart(void) {
    /* Same priority as the splash thread so a press is noticed promptly. */
    Result rc = threadCreate(&g_thread, buttonThread, NULL, g_stack, sizeof(g_stack), 0x2C, -2);
    if (R_SUCCEEDED(rc))
        rc = threadStart(&g_thread);
    if (R_FAILED(rc))
        logWrite("INPUT  button thread start failed: 0x%x", rc);
}
