/*
 * SwitchSplash — Wii U–style per-game boot splash sysmodule.
 *
 * Milestone 1: boot, poll pm for application launches, log each launch/exit with its
 * program ID to sdmc:/config/bootsplash/bootsplash.log.
 */
#include <switch.h>

#include "app_monitor.h"
#include "log.h"

#define POLL_INTERVAL_NS 10000000ULL /* 10 ms, same as sys-tune */

/* ---------------------------------------------------------------------------------------
 * libnx runtime configuration
 * ------------------------------------------------------------------------------------- */

/* We are a plain sysmodule, not an applet. */
u32 __nx_applet_type = AppletType_None;

/* We only need one fs session (for sdmc). */
u32 __nx_fs_num_sessions = 1;

/* Static inner heap. This keeps the kernel heap region untouched, so later milestones
 * can grow it with svcSetHeapSize only while a splash is showing (DESIGN.md §3). */
#define INNER_HEAP_SIZE (384 * 1024)

void __libnx_initheap(void) {
    static char inner_heap[INNER_HEAP_SIZE];
    extern char *fake_heap_start;
    extern char *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

void __appInit(void) {
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

    rc = fsdevMountSdmc();
    if (R_FAILED(rc))
        diagAbortWithResult(rc);

    /* sm stays open: later milestones need vi:m, audout, hid:sys on demand. */
}

void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    smExit();
}

/* ---------------------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------------------- */

static const char *storageName(u8 storage_id) {
    switch (storage_id) {
        case NcmStorageId_GameCard:  return "gamecard";
        case NcmStorageId_BuiltInUser: return "nand";
        case NcmStorageId_SdCard:    return "sd";
        case NcmStorageId_Host:      return "host";
        case NcmStorageId_BuiltInSystem: return "system";
        default:                     return "?";
    }
}

static void logEvent(const AppEvent *ev) {
    switch (ev->type) {
        case AppEvent_Launched:
            if (ev->has_ams_info) {
                logWrite("LAUNCH tid=%016lX pid=%lu storage=%s hbl=%d keys=0x%lx flags=0x%lx",
                         ev->program_id, ev->pid, storageName(ev->storage_id),
                         (ev->status.flags & AMS_OVERRIDE_FLAG_HBL) ? 1 : 0,
                         ev->status.keys_held, ev->status.flags);
            } else {
                logWrite("LAUNCH tid=%016lX pid=%lu (AtmosphereGetProcessInfo unavailable)",
                         ev->program_id, ev->pid);
            }
            break;
        case AppEvent_Exited:
            logWrite("EXIT   tid=%016lX pid=%lu", ev->program_id, ev->pid);
            break;
        default:
            break;
    }
}

int main(int argc, char **argv) {
    logInit();

    const u32 hos = hosversionGet();
    logWrite("----- bootsplash started (milestone 1), HOS %u.%u.%u -----",
             HOSVER_MAJOR(hos), HOSVER_MINOR(hos), HOSVER_MICRO(hos));

    Result rc = appMonitorInitialize();
    if (R_FAILED(rc)) {
        logWrite("FATAL appMonitorInitialize failed: 0x%x", rc);
        return 0; /* Exit quietly rather than crash the console. */
    }

    while (true) {
        AppEvent ev;
        if (appMonitorPoll(&ev))
            logEvent(&ev);

        svcSleepThread(POLL_INTERVAL_NS);
    }

    appMonitorExit();
    return 0;
}
