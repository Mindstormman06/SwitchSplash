/*
 * Splash layer. Milestone 2: a full-screen solid color (plus alpha test stripes) in a
 * managed vi layer above everything, held for N ms, then destroyed.
 *
 * Layer setup follows libtesla (tesla.hpp, Renderer::init) — see DESIGN.md §4.
 */
#include "splash.h"
#include "dynheap.h"
#include "log.h"

#include <stdatomic.h>
#include <string.h>

/* Framebuffer resolution. */
#define SCREEN_W 1280
#define SCREEN_H 720
#define BYTES_PER_PIXEL 2 /* RGBA4444 */

/* vi layer coordinates are in 1920x1080 display space (libtesla cfg::ScreenWidth/
 * ScreenHeight). Confirmed on hardware: a 1280x720 layer covered only the top-left 2/3.
 * FitToLayer scales the 1280x720 framebuffer up to fill it. Size comes from config
 * (layer_w/layer_h, default 1920x1080) while we diagnose load-time visibility. */

/* Must match libnx framebufferCreate's alignment (verified by disassembly):
 * stride 64-byte aligned, height 128-row aligned, block-linear with 16-GOB blocks. */
#define FB_STRIDE     ((SCREEN_W * BYTES_PER_PIXEL + 63) & ~63)
#define FB_HEIGHT_AL  ((SCREEN_H + 127) & ~127)
#define FB_SIZE       (FB_STRIDE * FB_HEIGHT_AL)
#define GOBS_PER_ROW  (FB_STRIDE / 64)

extern u64 __nx_vi_layer_id;

static Thread g_thread;
static bool g_thread_created = false;
static atomic_bool g_running = false;
static atomic_bool g_cancel = false;
static atomic_ullong g_pid = 0;

static u64 g_program_id;
static u64 g_launch_tick;
static Config g_cfg;

alignas(0x1000) static u8 g_thread_stack[0x8000];

/* ---- helpers ---- */

static inline u64 msSince(u64 tick) {
    return armTicksToNs(armGetSystemTick() - tick) / 1000000ULL;
}

/* PIXEL_FORMAT_RGBA_4444 as libtesla uses it: r in bits 0-3, g 4-7, b 8-11, a 12-15. */
static inline u16 rgba4444(u32 r8, u32 g8, u32 b8, u32 a4) {
    return (u16)((r8 >> 4) | ((g8 >> 4) << 4) | ((b8 >> 4) << 8) | ((a4 & 0xF) << 12));
}

/* Offset in u16 units of pixel (x, y) in a block-linear surface (GOB = 64 B x 8 rows,
 * block = 1 GOB wide x 16 GOBs tall). Same math as libtesla's getPixelOffset, generalized. */
static inline u32 pixelOffset(u32 x, u32 y) {
    const u32 xb = x * BYTES_PER_PIXEL;
    u32 off = (y / 128) * (GOBS_PER_ROW * 8192)
            + (xb / 64) * 8192
            + ((y % 128) / 8) * 512
            + ((xb % 64) / 32) * 256
            + ((y % 8) / 2) * 64
            + ((xb % 32) / 16) * 32
            + (y % 2) * 16
            + (xb % 16);
    return off / BYTES_PER_PIXEL;
}

/* IManagerDisplayService#6000 AddToLayerStack — not wrapped by libnx (from libtesla). */
static Result viAddToLayerStack(ViLayer *layer, ViLayerStack stack) {
    const struct {
        u32 stack;
        u64 layer_id;
    } in = { stack, layer->layer_id };
    return serviceDispatchIn(viGetSession_IManagerDisplayService(), 6000, in);
}

static void drawTestScreen(u16 *fb) {
    const u16 bg = rgba4444((g_cfg.color >> 16) & 0xFF, (g_cfg.color >> 8) & 0xFF, g_cfg.color & 0xFF, 0xF);

    /* Alpha test (DESIGN.md risk R4). First hardware run (2026-11-13): stripe A was solid
     * white, so the compositor is NOT straight-alpha. Box C tells the remaining two apart.
     *  A (rows 260-339): white, alpha 0.  Seen as solid white.
     *  B (rows 380-459): red, alpha 8/15. Seen as solid bright red (over a black background).
     *  C (bottom-right box, over the stock "NINTENDO SWITCH" logo): black, alpha 0.
     *      Logo visible through it -> premultiplied alpha (transparency works).
     *      Solid black            -> alpha is ignored, the layer is opaque. */
    const u16 stripe_a = rgba4444(0xFF, 0xFF, 0xFF, 0x0);
    const u16 stripe_b = rgba4444(0xFF, 0x00, 0x00, 0x8);
    const u16 box_c    = rgba4444(0x00, 0x00, 0x00, 0x0);

    for (u32 y = 0; y < SCREEN_H; y++) {
        u16 row = bg;
        if (g_cfg.alpha_test) {
            if (y >= 260 && y < 340) row = stripe_a;
            else if (y >= 380 && y < 460) row = stripe_b;
        }
        for (u32 x = 0; x < SCREEN_W; x++) {
            u16 c = row;
            if (g_cfg.alpha_test && x >= 900 && y >= 560)
                c = box_c;
            fb[pixelOffset(x, y)] = c;
        }
    }
}

typedef enum {
    Hold_Done = 0,
    Hold_AppExited,
    Hold_HomePressed,
    Hold_PowerPressed,
} HoldResult;

/* HOME/power button events (libtesla backgroundEventPoller pattern). The splash must
 * never cover the HOME menu, so either press ends it immediately. */
static Event g_home_event, g_power_event;
static bool g_hidsys_init = false, g_home_valid = false, g_power_valid = false;

static void buttonEventsOpen(void) {
    if (R_FAILED(hidsysInitialize())) {
        logWrite("SPLASH  hidsysInitialize failed: HOME won't cancel the splash");
        return;
    }
    g_hidsys_init = true;
    g_home_valid = R_SUCCEEDED(hidsysAcquireHomeButtonEventHandle(&g_home_event, false));
    g_power_valid = R_SUCCEEDED(hidsysAcquireSleepButtonEventHandle(&g_power_event, false));
    /* Drop any press that happened before the splash started. */
    if (g_home_valid) eventClear(&g_home_event);
    if (g_power_valid) eventClear(&g_power_event);
    if (!g_home_valid || !g_power_valid)
        logWrite("SPLASH  button events: home=%d power=%d", g_home_valid, g_power_valid);
}

static void buttonEventsClose(void) {
    if (g_home_valid) eventClose(&g_home_event);
    if (g_power_valid) eventClose(&g_power_event);
    if (g_hidsys_init)
        hidsysExit();
    g_hidsys_init = g_home_valid = g_power_valid = false;
}

/* Waits up to `ms`, waking early on app exit or HOME/power press. */
static HoldResult holdFor(u32 ms) {
    const u64 start = armGetSystemTick();
    while (msSince(start) < ms) {
        if (atomic_load(&g_cancel))
            return Hold_AppExited;
        if (g_home_valid && R_SUCCEEDED(eventWait(&g_home_event, 0)))
            return Hold_HomePressed;
        if (g_power_valid && R_SUCCEEDED(eventWait(&g_power_event, 0)))
            return Hold_PowerPressed;

        /* Sleep on whichever event is available, so a press wakes us instantly. */
        if (g_home_valid)
            eventWait(&g_home_event, 10000000ULL);
        else
            svcSleepThread(10000000ULL);
    }
    return Hold_Done;
}

static const char *holdResultName(HoldResult r) {
    switch (r) {
        case Hold_AppExited:    return "application exited";
        case Hold_HomePressed:  return "HOME pressed";
        case Hold_PowerPressed: return "power button pressed";
        default:                return "done";
    }
}

#define STEP(call)                                                        \
    do {                                                                  \
        rc = (call);                                                      \
        if (R_FAILED(rc)) {                                               \
            logWrite("SPLASH  %s failed: 0x%x", #call, rc);               \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

static void splashThread(void *arg) {
    Result rc = 0;
    ViDisplay display = {0};
    ViLayer layer = {0};
    NWindow win = {0};
    Framebuffer fb = {0};
    bool vi_init = false, display_open = false, layer_created = false, win_created = false, fb_created = false;

    const u64 t0 = armGetSystemTick();
    const u32 nbuf = g_cfg.buffers;
    const size_t fb_total = ((size_t)FB_SIZE * nbuf + 0xFFF) & ~0xFFFUL;

    logWrite("SPLASH  begin tid=%016lX buffers=%u fb=%u bytes (%lu total) detect->begin=%lums",
             g_program_id, nbuf, (u32)FB_SIZE, (unsigned long)fb_total, msSince(g_launch_tick));

    logWrite("SPLASH  cfg: duration=%ums layer=%ux%u home_cancel=%d last_frame_stack=%d alpha_test=%d",
             g_cfg.duration_ms, g_cfg.layer_w, g_cfg.layer_h, g_cfg.home_cancel,
             g_cfg.last_frame_stack, g_cfg.alpha_test);

    /* 0. Listen for HOME/power before anything is on screen. */
    if (g_cfg.home_cancel)
        buttonEventsOpen();

    /* 1. Transient memory. */
    rc = dynheapAcquire(fb_total);
    if (R_FAILED(rc)) {
        logWrite("SPLASH  svcSetHeapSize(%lu) failed: 0x%x -> skipping splash (system pool exhausted?)",
                 (unsigned long)fb_total, rc);
        goto cleanup;
    }
    logWrite("SPLASH  heap acquired: %lu KiB in %lums", (unsigned long)(dynheapSize() / 1024), msSince(t0));

    /* 2. Layer (libtesla sequence). */
    STEP(viInitialize(ViServiceType_Manager));
    vi_init = true;
    STEP(viOpenDefaultDisplay(&display));
    display_open = true;
    STEP(viCreateManagedLayer(&display, (ViLayerFlags)0, 0, &__nx_vi_layer_id));
    STEP(viCreateLayer(&display, &layer));
    layer_created = true;
    STEP(viSetLayerScalingMode(&layer, ViScalingMode_FitToLayer));

    {
        s32 zmin = -1, zmax = -1, rw = 0, rh = 0, lw = 0, lh = 0;
        const Result rc_min = viGetZOrderCountMin(&display, &zmin);
        const Result rc_max = viGetZOrderCountMax(&display, &zmax);
        const Result rc_res = viGetDisplayResolution(&display, &rw, &rh);
        const Result rc_lres = viGetDisplayLogicalResolution(&display, &lw, &lh);
        logWrite("SPLASH  display: zmin=%d (0x%x) zmax=%d (0x%x) res=%dx%d (0x%x) logical=%dx%d (0x%x)",
                 zmin, rc_min, zmax, rc_max, rw, rh, rc_res, lw, lh, rc_lres);

        s32 z = (R_SUCCEEDED(rc_max) && zmax > 0) ? zmax : 255;
        rc = viSetLayerZ(&layer, z);
        logWrite("SPLASH  layer id=%lu z=%d (setZ rc=0x%x)", layer.layer_id, z, rc);
    }

    static const ViLayerStack stacks[] = {
        ViLayerStack_Default, ViLayerStack_Screenshot, ViLayerStack_Recording, ViLayerStack_Arbitrary,
        ViLayerStack_LastFrame, ViLayerStack_Null, ViLayerStack_ApplicationForDebug, ViLayerStack_Lcd,
    };
    for (size_t i = 0; i < sizeof(stacks) / sizeof(stacks[0]); i++) {
        if (stacks[i] == ViLayerStack_LastFrame && !g_cfg.last_frame_stack)
            continue;
        rc = viAddToLayerStack(&layer, stacks[i]);
        if (R_FAILED(rc))
            logWrite("SPLASH  AddToLayerStack(%d) failed: 0x%x", stacks[i], rc);
    }

    STEP(viSetLayerSize(&layer, g_cfg.layer_w, g_cfg.layer_h));
    STEP(viSetLayerPosition(&layer, 0.0f, 0.0f));
    STEP(nwindowCreateFromLayer(&win, &layer));
    win_created = true;
    STEP(framebufferCreate(&fb, &win, SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_4444, nbuf));
    fb_created = true;
    logWrite("SPLASH  layer+framebuffer ready in %lums (fb_size=%u, expected %u)",
             msSince(t0), fb.fb_size, (u32)FB_SIZE);

    if (fb.fb_size != FB_SIZE) {
        logWrite("SPLASH  fb_size mismatch, swizzle would be wrong -> aborting");
        goto cleanup;
    }

    /* 3. Draw one frame. */
    u64 tb = armGetSystemTick();
    u16 *px = framebufferBegin(&fb, NULL);
    const u64 begin_ms = msSince(tb);
    drawTestScreen(px);
    framebufferEnd(&fb);
    logWrite("SPLASH  frame presented: begin=%lums draw+end=%lums, detect->visible=%lums",
             begin_ms, msSince(tb) - begin_ms, msSince(g_launch_tick));

    /* 4. Hold. */
    {
        const HoldResult hr = holdFor(g_cfg.duration_ms);
        if (hr != Hold_Done) {
            logWrite("SPLASH  cancelled after %lums: %s", msSince(t0), holdResultName(hr));
            goto cleanup;
        }
    }

    /* 5. Optional: present a second frame. With 1 buffer this tells us whether a
     *    re-dequeue blocks (needed for the fade in M5). */
    if (g_cfg.test_redraw) {
        logWrite("SPLASH  test_redraw: dequeuing again (buffers=%u)...", nbuf);
        tb = armGetSystemTick();
        px = framebufferBegin(&fb, NULL);
        const u64 b2 = msSince(tb);
        drawTestScreen(px);
        framebufferEnd(&fb);
        logWrite("SPLASH  test_redraw: begin=%lums total=%lums", b2, msSince(tb));
        holdFor(500);
    }

cleanup:
    {
        const u64 tc = armGetSystemTick();
        if (fb_created)    framebufferClose(&fb);
        if (win_created)   nwindowClose(&win);
        if (layer_created) viDestroyManagedLayer(&layer);
        __nx_vi_layer_id = 0;
        if (display_open)  viCloseDisplay(&display);
        if (vi_init)       viExit();
        dynheapRelease();
        buttonEventsClose();
        logWrite("SPLASH  end tid=%016lX teardown=%lums total=%lums", g_program_id, msSince(tc), msSince(t0));
    }

    atomic_store(&g_running, false);
}

/* ---- public ---- */

void splashReap(void) {
    if (g_thread_created && !atomic_load(&g_running)) {
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_thread_created = false;
    }
}

bool splashStart(u64 pid, u64 program_id, u64 launch_tick, const Config *cfg) {
    splashReap();
    if (g_thread_created) {
        logWrite("SPLASH  already running, ignoring launch of %016lX", program_id);
        return false;
    }

    g_program_id = program_id;
    g_launch_tick = launch_tick;
    g_cfg = *cfg;
    atomic_store(&g_pid, pid);
    atomic_store(&g_cancel, false);
    atomic_store(&g_running, true);

    /* Priority 0x2C: above the monitor thread (49) so drawing isn't delayed by polling. */
    Result rc = threadCreate(&g_thread, splashThread, NULL, g_thread_stack, sizeof(g_thread_stack), 0x2C, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&g_thread);
        if (R_FAILED(rc))
            threadClose(&g_thread);
    }
    if (R_FAILED(rc)) {
        logWrite("SPLASH  thread start failed: 0x%x", rc);
        atomic_store(&g_running, false);
        return false;
    }
    g_thread_created = true;
    return true;
}

void splashCancel(u64 pid) {
    if (atomic_load(&g_running) && atomic_load(&g_pid) == pid)
        atomic_store(&g_cancel, true);
}
