/*
 * SwitchSplash — Wii U–style per-game boot splash sysmodule.
 *
 * Milestone 1: boot, poll pm for application launches, log each launch/exit with its
 * program ID to sdmc:/config/bootsplash/bootsplash.log.
 * Milestone 2: on launch, show a solid-color full-screen layer for N ms.
 * Milestone 3: per-title PNG from sdmc:/config/bootsplash/<TID>/splash.png (or default/).
 * Milestone 4: splash.wav next to the image, played through audout with a fade-out.
 * Milestone 5: visual fade-out (vi layer alpha, or a pixel fade fallback).
 */
#include <switch.h>

#include "app_monitor.h"
#include "config.h"
#include "focus_watch.h"
#include "log.h"
#include "splash.h"

#define POLL_INTERVAL_NS 10000000ULL /* 10 ms, same as sys-tune */

/* ---------------------------------------------------------------------------------------
 * libnx runtime configuration
 * ------------------------------------------------------------------------------------- */

/* We are a plain sysmodule, not an applet. */
u32 __nx_applet_type = AppletType_None;

/* We only need one fs session (for sdmc). */
u32 __nx_fs_num_sessions = 1;

/* nvdrv transfer memory. libnx's default is 8 MiB, which we can't afford. Ultrahand's
 * libtesla runs a 2-buffer layer with 0x15000. Allocated from the inner heap. */
u32 __nx_nv_transfermem_size = 0x15000;

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
    logWrite("----- bootsplash started (milestone 5, build %s %s), HOS %u.%u.%u -----",
             __DATE__, __TIME__, HOSVER_MAJOR(hos), HOSVER_MINOR(hos), HOSVER_MICRO(hos));

    Result rc = appMonitorInitialize();
    if (R_FAILED(rc)) {
        logWrite("FATAL appMonitorInitialize failed: 0x%x", rc);
        return 0; /* Exit quietly rather than crash the console. */
    }

    focusWatchStart();

    while (true) {
        AppEvent ev;
        if (appMonitorPoll(&ev)) {
            logEvent(&ev);

            if (ev.type == AppEvent_Launched) {
                Config cfg;
                configLoad(&cfg);
                const bool hbl = ev.has_ams_info && (ev.status.flags & AMS_OVERRIDE_FLAG_HBL);

                if (!cfg.enabled) {
                    logWrite("SPLASH  disabled in config");
                } else if (hbl && cfg.skip_hbl) {
                    logWrite("SPLASH  skipped (hbmenu title takeover)");
                } else if (cfg.test_pattern) {
                    splashStart(ev.pid, ev.program_id, ev.tick, &cfg, NULL);
                } else {
                    char path[128];
                    const SplashImageSource src = splashFindImage(ev.program_id, path, sizeof(path));
                    if (src == SplashImage_None)
                        logWrite("SPLASH  no image for %016lX and no default -> no splash", ev.program_id);
                    else
                        splashStart(ev.pid, ev.program_id, ev.tick, &cfg, path);
                }
            } else if (ev.type == AppEvent_Exited) {
                splashCancel(ev.pid);
            }
        }

        splashReap();
        svcSleepThread(POLL_INTERVAL_NS);
    }

    appMonitorExit();
    return 0;
}
