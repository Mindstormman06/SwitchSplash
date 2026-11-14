/*
 * Splash layer.
 *  M2: managed vi layer above everything, held for N ms, then destroyed.
 *  M3: per-title PNG, decoded row by row straight into the framebuffer.
 *
 * Layer setup follows libtesla (tesla.hpp, Renderer::init) — see DESIGN.md §4.
 */
#include "splash.h"
#include "audio_player.h"
#include "dynheap.h"
#include "fade_pixels.h"
#include "focus_watch.h"
#include "image_blit.h"
#include "log.h"
#include "png_stream.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* Framebuffer resolution. */
#define SCREEN_W 1280
#define SCREEN_H 720
#define BYTES_PER_PIXEL 2 /* RGBA4444 */

/* vi layer coordinates are in 1920x1080 display space (libtesla cfg::ScreenWidth/
 * ScreenHeight). Confirmed on hardware: a 1280x720 layer covered only the top-left 2/3.
 * FitToLayer scales the 1280x720 framebuffer up to fill it. */
#define LAYER_W 1920
#define LAYER_H 1080

/* Must match libnx framebufferCreate's alignment (verified by disassembly):
 * stride 64-byte aligned, height 128-row aligned, block-linear with 16-GOB blocks. */
#define FB_STRIDE     ((SCREEN_W * BYTES_PER_PIXEL + 63) & ~63)
#define FB_HEIGHT_AL  ((SCREEN_H + 127) & ~127)
#define FB_SIZE       (FB_STRIDE * FB_HEIGHT_AL)
#define GOBS_PER_ROW  (FB_STRIDE / 64)

#define SPLASH_ROOT "sdmc:/config/bootsplash"

#define DEFAULT_SPLASH_MS 6000   /* duration_ms=0 without a jingle */
#define MAX_SPLASH_MS     30000  /* upper bound for duration_ms=0 with a jingle */
#define CANCEL_FADE_MS    100    /* audio fade on HOME / app exit */

extern u64 __nx_vi_layer_id;

static Thread g_thread;
static bool g_thread_created = false;
static atomic_bool g_running = false;
static atomic_bool g_cancel = false;
static atomic_ullong g_pid = 0;

static u64 g_program_id;
static u64 g_launch_tick;
static Config g_cfg;
static char g_image_path[128];
static char g_audio_path[128];
static bool g_has_image;

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

/* ---- Layer alpha (M5 fade) ----
 * Switchbrew lists ISystemDisplayService#2209 SetLayerAlpha and IManagerDisplayService
 * #6013 SetLayerOpacity [13.0.0+], without argument layouts. libnx's SetLayerPosition
 * (#2201) packs its floats before the layer id, so {f32, pad, u64} is tried first, then
 * {u64, f32}. A wrong layout fails safely: the float lands in the layer-id slot and the
 * service can't find that layer. Probed with alpha 1.0 before the first frame. */
typedef enum {
    LayerAlpha_None = -1,
    LayerAlpha_Sys2209_FloatFirst = 0,
    LayerAlpha_Sys2209_IdFirst,
    LayerAlpha_Mgr6013_FloatFirst,
    LayerAlpha_Mgr6013_IdFirst,
    LayerAlpha_Count,
} LayerAlphaMethod;

static const char *const g_alpha_method_names[LayerAlpha_Count] = {
    "ISystemDisplayService#2209 {f32,u64}", "ISystemDisplayService#2209 {u64,f32}",
    "IManagerDisplayService#6013 {f32,u64}", "IManagerDisplayService#6013 {u64,f32}",
};

static Result setLayerAlpha(LayerAlphaMethod m, u64 layer_id, float alpha) {
    const bool sys = m == LayerAlpha_Sys2209_FloatFirst || m == LayerAlpha_Sys2209_IdFirst;
    Service *s = sys ? viGetSession_ISystemDisplayService() : viGetSession_IManagerDisplayService();
    const u32 cmd = sys ? 2209 : 6013;
    if (!s || !serviceIsActive(s))
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    if (m == LayerAlpha_Sys2209_FloatFirst || m == LayerAlpha_Mgr6013_FloatFirst) {
        const struct { float alpha; u32 pad; u64 layer_id; } in = { alpha, 0, layer_id };
        return serviceDispatchIn(s, cmd, in);
    } else {
        const struct { u64 layer_id; float alpha; u32 pad; } in = { layer_id, alpha, 0 };
        return serviceDispatchIn(s, cmd, in);
    }
}

static LayerAlphaMethod probeLayerAlpha(u64 layer_id) {
    Result rcs[LayerAlpha_Count] = {0};
    for (int m = 0; m < LayerAlpha_Count; m++) {
        if (m == LayerAlpha_Mgr6013_FloatFirst && hosversionBefore(13, 0, 0))
            break;
        rcs[m] = setLayerAlpha((LayerAlphaMethod)m, layer_id, 1.0f);
        if (R_SUCCEEDED(rcs[m])) {
            logWrite("SPLASH  layer alpha via %s", g_alpha_method_names[m]);
            return (LayerAlphaMethod)m;
        }
    }
    logWrite("SPLASH  no layer alpha command worked (2209: 0x%x/0x%x, 6013: 0x%x/0x%x)",
             rcs[0], rcs[1], hosversionAtLeast(13, 0, 0) ? rcs[2] : 0, hosversionAtLeast(13, 0, 0) ? rcs[3] : 0);
    return LayerAlpha_None;
}

/* M2 test screen (test_pattern=1). Hardware result 2026-11-13: premultiplied alpha.
 *  A (rows 260-339): white, alpha 0 -> solid white.
 *  B (rows 380-459): red, alpha 8/15 -> see-through red.
 *  C (bottom-right box): black, alpha 0 -> fully transparent (boot logo visible). */
static void drawTestScreen(u16 *fb) {
    const u16 bg       = rgba4444(0x20, 0x30, 0x60, 0xF);
    const u16 stripe_a = rgba4444(0xFF, 0xFF, 0xFF, 0x0);
    const u16 stripe_b = rgba4444(0xFF, 0x00, 0x00, 0x8);
    const u16 box_c    = rgba4444(0x00, 0x00, 0x00, 0x0);

    for (u32 y = 0; y < SCREEN_H; y++) {
        u16 row = bg;
        if (y >= 260 && y < 340) row = stripe_a;
        else if (y >= 380 && y < 460) row = stripe_b;
        for (u32 x = 0; x < SCREEN_W; x++)
            fb[pixelOffset(x, y)] = (x >= 900 && y >= 560) ? box_c : row;
    }
}

/* Blitter output -> swizzled framebuffer. */
static void storeRowToFb(void *user, uint32_t y, const uint16_t *row) {
    u16 *fb = user;
    if (y >= SCREEN_H)
        return;
    for (u32 x = 0; x < SCREEN_W; x++)
        fb[pixelOffset(x, y)] = row[x];
}

typedef enum {
    Hold_Done = 0,
    Hold_AppExited,
    Hold_HomePressed,
    Hold_PowerPressed,
} HoldResult;

/* Waits up to `ms`, waking early on app exit or when the user leaves the game.
 * `since` is the splash start tick: button presses before it are ignored.
 * With `until_audio_end`, also returns as soon as the jingle has finished. */
static HoldResult holdFor(u32 ms, u64 since, bool until_audio_end) {
    const u64 start = armGetSystemTick();
    while (msSince(start) < ms) {
        if (until_audio_end && !audioIsPlaying())
            return Hold_Done;
        if (atomic_load(&g_cancel))
            return Hold_AppExited;
        if (g_cfg.home_cancel) {
            if (focusLastHomeTick() > since)
                return Hold_HomePressed;
            if (focusLastPowerTick() > since)
                return Hold_PowerPressed;
        }
        svcSleepThread(10000000ULL);
    }
    return Hold_Done;
}

static bool interrupted(u64 since) {
    if (atomic_load(&g_cancel))
        return true;
    return g_cfg.home_cancel && (focusLastHomeTick() > since || focusLastPowerTick() > since);
}

/* Fades the splash out over cfg.fade_ms (M5). Returns false if interrupted, in which case
 * the caller tears the layer down immediately.
 *  - Layer alpha (if a vi command was found): smooth, ~60 steps, no redraw, no memory.
 *  - Otherwise pixels, 15 steps (4-bit alpha), premultiplied scaling (fade_pixels.c):
 *      buffers=2: proper redraw into the back buffer, copying from the presented one.
 *      buffers=1: in place on the presented buffer + cache flush, no dequeue (a second
 *                 dequeue of a single buffer may block forever). Whether the compositor
 *                 picks up the change without a queue is what this build finds out. */
static bool runFade(Framebuffer *fb, ViLayer *layer, LayerAlphaMethod method, u16 *presented, u64 since) {
    const u32 ms = g_cfg.fade_ms;
    if (ms == 0 || g_cfg.fade_mode == FadeMode_None)
        return true;
    const u64 start = armGetSystemTick();

    if (method != LayerAlpha_None && g_cfg.fade_mode != FadeMode_Pixels) {
        u32 steps = 0;
        for (;;) {
            const u64 t = msSince(start);
            const float a = t >= ms ? 0.0f : 1.0f - (float)t / (float)ms;
            setLayerAlpha(method, layer->layer_id, a);
            steps++;
            if (t >= ms)
                break;
            if (interrupted(since))
                return false;
            svcSleepThread(16000000ULL); /* ~one frame */
        }
        logWrite("SPLASH  fade (layer alpha) %u steps in %lums", steps, msSince(start));
        return true;
    }

    const size_t count = fb->fb_size / sizeof(u16);
    const u64 step_ns = (u64)ms * 1000000ULL / 15;
    u16 *src = presented;
    u64 worst_begin = 0;
    for (unsigned level = 15; level > 0; level--) {
        if (interrupted(since))
            return false;
        const u64 ts = armGetSystemTick();
        if (fb->num_fbs >= 2) {
            const u64 tb = armGetSystemTick();
            u16 *dst = framebufferBegin(fb, NULL);
            const u64 b = msSince(tb);
            if (b > worst_begin) worst_begin = b;
            fadeScale4444(dst, src, count, level, level - 1);
            framebufferEnd(fb);
            src = dst;
        } else {
            fadeScale4444(presented, presented, count, level, level - 1);
            armDCacheFlush(presented, fb->fb_size);
        }
        const u64 spent = armTicksToNs(armGetSystemTick() - ts);
        if (spent < step_ns)
            svcSleepThread(step_ns - spent);
    }
    logWrite("SPLASH  fade (pixels, %s) 15 steps in %lums, worst dequeue %lums",
             fb->num_fbs >= 2 ? "double-buffered" : "in place", msSince(start), worst_begin);
    return true;
}

static const char *holdResultName(HoldResult r) {
    switch (r) {
        case Hold_AppExited:     return "application exited";
        case Hold_HomePressed:   return "HOME pressed";
        case Hold_PowerPressed:  return "power button pressed";
        default:                 return "done";
    }
}

/* Decodes the PNG into the (dequeued) framebuffer. Returns false on error. */
static bool drawImage(u16 *fb, PngImage *img) {
    const PngInfo *info = pngGetInfo(img);
    Blitter blit;
    if (!blitInit(&blit, SCREEN_W, SCREEN_H, info->width, info->height, g_cfg.background, storeRowToFb, fb)) {
        logWrite("SPLASH  blitter out of memory");
        return false;
    }
    const PngResult r = pngDecode(img, blitPngRow, &blit);
    if (r == PNG_OK)
        blitFinish(&blit);
    else
        logWrite("SPLASH  PNG decode failed: %s (%d)", pngResultString(r), r);
    blitFree(&blit);
    return r == PNG_OK;
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
    PngImage *img = NULL;
    LayerAlphaMethod alpha_method = LayerAlpha_None;
    bool vi_init = false, display_open = false, layer_created = false, win_created = false, fb_created = false;

    const u64 t0 = armGetSystemTick();
    const u32 nbuf = g_cfg.buffers;
    const size_t fb_total = ((size_t)FB_SIZE * nbuf + 0xFFF) & ~0xFFFUL;

    logWrite("SPLASH  begin tid=%016lX image=%s detect->begin=%lums",
             g_program_id, g_has_image ? g_image_path : "(test pattern)", msSince(g_launch_tick));
    logWrite("SPLASH  cfg: duration=%ums buffers=%u bg=%06X home_cancel=%d audio=%d volume=%u audio_fade=%ums fade=%ums mode=%d",
             g_cfg.duration_ms, nbuf, g_cfg.background, g_cfg.home_cancel, g_cfg.audio, g_cfg.volume,
             g_cfg.audio_fade_ms, g_cfg.fade_ms, g_cfg.fade_mode);
    bool audio_on = false, audio_fade_requested = false;

    /* 0. Validate the image before touching the display, so a bad file never shows anything. */
    if (g_has_image) {
        const PngResult pr = pngOpen(&img, g_image_path);
        if (pr != PNG_OK) {
            logWrite("SPLASH  cannot use %s: %s (%d) -> no splash", g_image_path, pngResultString(pr), pr);
            goto cleanup;
        }
        const PngInfo *pi = pngGetInfo(img);
        logWrite("SPLASH  image %ux%u depth=%u color_type=%u", pi->width, pi->height, pi->bit_depth, pi->color_type);
    }

    /* 1. Transient memory. */
    rc = dynheapAcquire(fb_total);
    if (R_FAILED(rc)) {
        logWrite("SPLASH  svcSetHeapSize(%lu) failed: 0x%x -> skipping splash (system pool exhausted?)",
                 (unsigned long)fb_total, rc);
        goto cleanup;
    }

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
        s32 zmax = -1;
        const Result rc_max = viGetZOrderCountMax(&display, &zmax);
        const s32 z = (R_SUCCEEDED(rc_max) && zmax > 0) ? zmax : 255;
        rc = viSetLayerZ(&layer, z);
        if (R_FAILED(rc))
            logWrite("SPLASH  viSetLayerZ(%d) failed: 0x%x", z, rc);
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

    STEP(viSetLayerSize(&layer, LAYER_W, LAYER_H));
    STEP(viSetLayerPosition(&layer, 0.0f, 0.0f));
    STEP(nwindowCreateFromLayer(&win, &layer));
    win_created = true;
    STEP(framebufferCreate(&fb, &win, SCREEN_W, SCREEN_H, PIXEL_FORMAT_RGBA_4444, nbuf));
    fb_created = true;

    if (fb.fb_size != FB_SIZE) {
        logWrite("SPLASH  fb_size mismatch (%u != %u), swizzle would be wrong -> aborting", fb.fb_size, (u32)FB_SIZE);
        goto cleanup;
    }
    logWrite("SPLASH  layer ready in %lums", msSince(t0));

    /* Find a working layer-alpha command now, at alpha 1.0, so nothing visible changes. */
    if (g_cfg.fade_ms > 0 && g_cfg.fade_mode == FadeMode_Auto)
        alpha_method = probeLayerAlpha(layer.layer_id);

    /* 3. Draw one frame. The buffer is dequeued for the whole decode, so nothing partial
     *    is ever shown; until framebufferEnd the stock boot screen stays visible. */
    u64 tb = armGetSystemTick();
    u16 *px = framebufferBegin(&fb, NULL);
    bool drawn = true;
    if (g_has_image)
        drawn = drawImage(px, img);
    else
        drawTestScreen(px);
    if (!drawn)
        memset(px, 0, fb.fb_size); /* fully transparent: present nothing visible */
    framebufferEnd(&fb);

    if (!drawn)
        goto cleanup;
    logWrite("SPLASH  frame presented: draw=%lums, detect->visible=%lums", msSince(tb), msSince(g_launch_tick));

    /* Free the decoder's file handle and buffers while we hold. */
    pngClose(img);
    img = NULL;

    /* 4. Jingle, started once the image is up so the two line up. */
    if (g_cfg.audio && g_has_image)
        audio_on = audioStart(g_audio_path, (float)g_cfg.volume / 100.0f);

    /* 5. Hold. duration_ms=0 means "as long as the jingle" (default length without one). */
    {
        const bool until_audio_end = g_cfg.duration_ms == 0 && audio_on;
        const u32 hold_ms = g_cfg.duration_ms ? g_cfg.duration_ms : (audio_on ? MAX_SPLASH_MS : DEFAULT_SPLASH_MS);
        const HoldResult hr = holdFor(hold_ms, t0, until_audio_end);
        if (hr != Hold_Done) {
            logWrite("SPLASH  cancelled after %lums: %s", msSince(t0), holdResultName(hr));
            goto cleanup; /* cleanup fades the audio out quickly */
        }
        if (audio_on && audioIsPlaying()) {
            audioFadeOut(g_cfg.audio_fade_ms);
            audio_fade_requested = true;
        }
    }

    /* 6. Fade the image out alongside the jingle. */
    if (!runFade(&fb, &layer, alpha_method, px, t0)) {
        logWrite("SPLASH  fade interrupted after %lums", msSince(t0));
        goto cleanup;
    }

    /* 7. Optional: present a second frame. With 1 buffer this tells us whether a
     *    re-dequeue blocks. */
    if (g_cfg.test_redraw) {
        logWrite("SPLASH  test_redraw: dequeuing again (buffers=%u)...", nbuf);
        tb = armGetSystemTick();
        px = framebufferBegin(&fb, NULL);
        const u64 b2 = msSince(tb);
        drawTestScreen(px);
        framebufferEnd(&fb);
        logWrite("SPLASH  test_redraw: begin=%lums total=%lums", b2, msSince(tb));
        holdFor(500, t0, false);
    }

cleanup:
    {
        const u64 tc = armGetSystemTick();
        /* Cancelled or failed: a short fade, so the jingle never ends in a click. */
        if (audio_on && !audio_fade_requested)
            audioFadeOut(CANCEL_FADE_MS);
        pngClose(img);
        if (fb_created)    framebufferClose(&fb);
        if (win_created)   nwindowClose(&win);
        if (layer_created) viDestroyManagedLayer(&layer);
        __nx_vi_layer_id = 0;
        if (display_open)  viCloseDisplay(&display);
        if (vi_init)       viExit();
        dynheapRelease();
        /* The image is gone; let the jingle finish its fade, then release audout. */
        if (audio_on)
            audioJoin();
        logWrite("SPLASH  end tid=%016lX teardown=%lums total=%lums", g_program_id, msSince(tc), msSince(t0));
    }

    atomic_store(&g_running, false);
}

/* ---- public ---- */

SplashImageSource splashFindImage(u64 program_id, char *out, size_t out_size) {
    struct stat st;

    snprintf(out, out_size, SPLASH_ROOT "/%016lX/splash.png", program_id);
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
        return SplashImage_Title;

    snprintf(out, out_size, SPLASH_ROOT "/default/splash.png");
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
        return SplashImage_Default;

    out[0] = '\0';
    return SplashImage_None;
}

void splashReap(void) {
    if (g_thread_created && !atomic_load(&g_running)) {
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_thread_created = false;
    }
}

bool splashStart(u64 pid, u64 program_id, u64 launch_tick, const Config *cfg, const char *image_path) {
    splashReap();
    if (g_thread_created) {
        logWrite("SPLASH  already running, ignoring launch of %016lX", program_id);
        return false;
    }

    g_program_id = program_id;
    g_launch_tick = launch_tick;
    g_cfg = *cfg;
    g_has_image = image_path != NULL;
    g_audio_path[0] = '\0';
    if (image_path) {
        snprintf(g_image_path, sizeof(g_image_path), "%s", image_path);
        /* The jingle lives next to the image: <dir>/splash.png -> <dir>/splash.wav */
        snprintf(g_audio_path, sizeof(g_audio_path), "%s", image_path);
        char *slash = strrchr(g_audio_path, '/');
        if (slash)
            snprintf(slash + 1, sizeof(g_audio_path) - (size_t)(slash + 1 - g_audio_path), "splash.wav");
    }
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
