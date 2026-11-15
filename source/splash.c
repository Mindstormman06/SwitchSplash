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
#include "gif_canvas.h"
#include "gif_stream.h"
#include "image_blit.h"
#include "log.h"
#include "png_stream.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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
    Hold_Error,
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
        case Hold_Error:         return "error";
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

/* ---- Persistent display connection (latency, 2026-11-13) ----
 * vi:m, the default display and an nvdrv reference are opened on the first splash and kept
 * for the process lifetime, so later splashes skip reconnecting. Cheap: a few IPC sessions
 * plus the 0x15000-byte nvdrv transfer memory from the inner heap. The 2 MiB framebuffer
 * is still acquired and released per splash. */
static bool g_display_ready = false;
static ViDisplay g_display;

static Result ensureDisplay(void) {
    if (g_display_ready)
        return 0;
    Result rc = viInitialize(ViServiceType_Manager);
    if (R_FAILED(rc))
        return rc;
    rc = viOpenDefaultDisplay(&g_display);
    if (R_FAILED(rc)) {
        viExit();
        return rc;
    }
    /* An extra nvInitialize reference keeps nvdrv open across framebufferCreate/Close. */
    rc = nvInitialize();
    if (R_FAILED(rc)) {
        viCloseDisplay(&g_display);
        viExit();
        return rc;
    }
    g_display_ready = true;
    return 0;
}

/* ---- Decoded-image cache ----
 * The first launch decodes splash.png (~90 ms) and saves the finished framebuffer next to
 * it as splash.cache. Later launches read that straight into the framebuffer. The header
 * ties the cache to the PNG's size + mtime, the background colour and the framebuffer
 * layout; any mismatch means decode again and rewrite. */
#define CACHE_MAGIC   0x31435353u /* "SSC1" */
#define CACHE_VERSION 1u

typedef struct {
    u32 magic, version;
    u64 png_size, png_mtime;
    u32 background, fb_size;
} CacheHeader;

static void cachePathFor(const char *image_path, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", image_path);
    char *slash = strrchr(out, '/');
    if (slash)
        snprintf(slash + 1, out_size - (size_t)(slash + 1 - out), "splash.cache");
}

static bool cacheHeaderFor(const char *image_path, CacheHeader *h) {
    struct stat st;
    if (stat(image_path, &st) != 0)
        return false;
    memset(h, 0, sizeof(*h));
    h->magic = CACHE_MAGIC;
    h->version = CACHE_VERSION;
    h->png_size = (u64)st.st_size;
    h->png_mtime = (u64)st.st_mtime;
    h->background = g_cfg.background;
    h->fb_size = FB_SIZE;
    return true;
}

/* Opens the cache if it matches; returns the FILE positioned at the pixel data, or NULL. */
static FILE *cacheOpen(const char *cache_path, const CacheHeader *want) {
    FILE *f = fopen(cache_path, "rb");
    if (!f)
        return NULL;
    CacheHeader have;
    if (fread(&have, 1, sizeof(have), f) != sizeof(have) || memcmp(&have, want, sizeof(have)) != 0) {
        fclose(f);
        return NULL;
    }
    return f;
}

static void cacheWrite(const char *cache_path, const CacheHeader *h, const u16 *px) {
    char tmp[136];
    snprintf(tmp, sizeof(tmp), "%s.tmp", cache_path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return;
    const bool ok = fwrite(h, 1, sizeof(*h), f) == sizeof(*h) && fwrite(px, 1, FB_SIZE, f) == FB_SIZE;
    fclose(f);
    remove(cache_path);
    if (!ok || rename(tmp, cache_path) != 0)
        remove(tmp);
}

/* ---- Animation test (groundwork for GIF playback, 2026-11-14) ----
 * test_anim=1 replaces the image with an animated pattern to find out, on hardware, which
 * redraw method works and how fast:
 *   anim_mode=double : 2 framebuffers, framebufferBegin/End per frame (standard path)
 *   anim_mode=inplace: 1 framebuffer, redrawn while presented + cache flush, no re-dequeue
 * anim_width=640|1280 picks the framebuffer size (16:9); the layer scales it to full screen.
 * Pattern: scrolling colour bands, a white bar moving 4 px/frame, and a binary frame counter
 * (top-left squares), so stutter, tearing and dropped frames are visible. */
typedef struct {
    u32 w, h, stride, gobs, size;
} FbGeom;

static FbGeom fbGeom(u32 w, u32 h) {
    FbGeom g;
    g.w = w;
    g.h = h;
    g.stride = (w * BYTES_PER_PIXEL + 63) & ~63u;
    g.gobs = g.stride / 64;
    g.size = g.stride * ((h + 127) & ~127u);
    return g;
}

static inline u32 pixelOffsetG(const FbGeom *g, u32 x, u32 y) {
    const u32 xb = x * BYTES_PER_PIXEL;
    const u32 off = (y / 128) * (g->gobs * 8192) + (xb / 64) * 8192 + ((y % 128) / 8) * 512 +
                    ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
    return off / BYTES_PER_PIXEL;
}

static void drawAnimFrame(u16 *fb, const FbGeom *g, u32 frame) {
    const u16 white = rgba4444(0xFF, 0xFF, 0xFF, 0xF);
    const u32 bar_x = (frame * 4) % g->w;
    const u32 sq = g->w / 64; /* counter square size */
    for (u32 y = 0; y < g->h; y++) {
        const u32 t = (y * 255 / g->h + frame * 3) & 0xFF;
        const u16 row = rgba4444(t, (t * 2) & 0xFF, 255 - t, 0xF);
        for (u32 x = 0; x < g->w; x++) {
            u16 c = row;
            if (x >= bar_x && x < bar_x + 8)
                c = white;
            if (y >= sq && y < 2 * sq && x >= sq && x < sq + 16 * sq * 2) {
                const u32 bit = (x - sq) / (sq * 2);
                const bool inside = ((x - sq) % (sq * 2)) < sq;
                if (inside)
                    c = (frame >> (15 - bit)) & 1 ? white : rgba4444(0, 0, 0, 0xF);
            }
            fb[pixelOffsetG(g, x, y)] = c;
        }
    }
}

typedef struct {
    u64 sum, max;
} Stat;

static void statAdd(Stat *s, u64 us) {
    s->sum += us;
    if (us > s->max)
        s->max = us;
}

static void runAnimTest(Framebuffer *fb, const FbGeom *g, bool inplace, u32 duration_ms, u64 since) {
    const u64 start = armGetSystemTick();
    Stat wait = {0}, draw = {0}, present = {0}, frame_t = {0};
    u32 frames = 0;
    u16 *px = NULL;
    u64 last = start;

    while (msSince(start) < duration_ms && !interrupted(since)) {
        const u64 f0 = armGetSystemTick();
        if (!inplace || frames == 0) {
            px = framebufferBegin(fb, NULL);
        }
        const u64 f1 = armGetSystemTick();
        drawAnimFrame(px, g, frames);
        const u64 f2 = armGetSystemTick();
        if (!inplace || frames == 0) {
            framebufferEnd(fb);
        } else {
            armDCacheFlush(px, g->size);
        }
        const u64 f3 = armGetSystemTick();

        statAdd(&wait, armTicksToNs(f1 - f0) / 1000);
        statAdd(&draw, armTicksToNs(f2 - f1) / 1000);
        statAdd(&present, armTicksToNs(f3 - f2) / 1000);
        if (frames > 0)
            statAdd(&frame_t, armTicksToNs(f0 - last) / 1000);
        last = f0;
        frames++;

        if (inplace) {
            /* No buffer queue to pace us: aim for ~60 fps. */
            const u64 spent = armTicksToNs(armGetSystemTick() - f0);
            if (spent < 16666666ULL)
                svcSleepThread(16666666ULL - spent);
        }
    }

    const u64 total = msSince(start);
    const u32 n = frames ? frames : 1;
    logWrite("ANIM    mode=%s %ux%u bufs=%u: %u frames in %lums = %lu.%lu fps", inplace ? "inplace" : "double",
             g->w, g->h, fb->num_fbs, frames, total, total ? frames * 1000UL / total : 0,
             total ? (frames * 10000UL / total) % 10 : 0);
    logWrite("ANIM    avg/max us: wait %lu/%lu  draw %lu/%lu  present %lu/%lu  frame-to-frame %lu/%lu",
             wait.sum / n, wait.max, draw.sum / n, draw.max, present.sum / n, present.max,
             frames > 1 ? frame_t.sum / (frames - 1) : 0, frame_t.max);
}

/* ---- GIF splash (2026-11-14) ----
 * Hardware test results decided the design: double-buffered 640x360 ran at a locked 60 fps
 * in the 2 MiB budget; 1280x720 double buffering was refused (0x10801, resource limit);
 * 1280x720 in place worked but tore. So GIFs render into a 640x360 canvas (centred, or scaled
 * down if bigger), which is copied into the back buffer for each new frame. Memory, all in
 * the 2 MiB block: 2 x 480 KiB framebuffers + 450 KiB canvas + 450 KiB disposal backup. */
#define GIF_W 640
#define GIF_H 360

static bool hasSuffix(const char *s, const char *suffix) {
    const size_t a = strlen(s), b = strlen(suffix);
    return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

static u16 *presentCanvas(Framebuffer *fb, const FbGeom *g, const u16 *canvas) {
    u16 *px = framebufferBegin(fb, NULL);
    for (u32 y = 0; y < g->h; y++) {
        const u16 *row = &canvas[y * g->w];
        for (u32 x = 0; x < g->w; x++)
            px[pixelOffsetG(g, x, y)] = row[x];
    }
    framebufferEnd(fb);
    return px;
}

/* GIF delay -> ms. Like browsers, 0-1 cs means "as fast as the author forgot to set": 100 ms. */
static u32 gifDelayMs(u16 cs) {
    return cs <= 1 ? 100 : (u32)cs * 10;
}

static HoldResult cancelReason(u64 since) {
    if (atomic_load(&g_cancel))
        return Hold_AppExited;
    if (g_cfg.home_cancel) {
        if (focusLastHomeTick() > since)
            return Hold_HomePressed;
        if (focusLastPowerTick() > since)
            return Hold_PowerPressed;
    }
    return Hold_Done;
}

/* Plays the GIF for the splash's duration. Returns Hold_Done when the time is up, a cancel
 * reason, or Hold_Error if nothing could be shown. *last_px is the last presented buffer. */
static HoldResult playGif(Framebuffer *fb, const FbGeom *g, GifFile *gif, u64 since, bool *audio_on,
                          u16 **last_px, bool *log_held) {
    const GifInfo *info = gifGetInfo(gif);
    const size_t cbytes = (size_t)g->w * g->h * sizeof(u16);
    u16 *canvas = dynheapAlloc(cbytes);
    u16 *backup = dynheapAlloc(cbytes); /* may be NULL: "restore previous" then clears instead */
    if (!canvas) {
        logWrite("SPLASH  no room for the GIF canvas");
        return Hold_Error;
    }
    GifCanvas gc;
    gifCanvasInit(&gc, canvas, backup, g->w, g->h, info->width, info->height, g_cfg.background, true);

    /* First frame. */
    GifFrame fr;
    u64 td = armGetSystemTick();
    GifResult r = gifNextFrame(gif, &fr, gifCanvasRow, &gc);
    if (r != GIF_OK) {
        logWrite("SPLASH  GIF first frame failed: %s (%d)", gifResultString(r), r);
        return Hold_Error;
    }
    gifCanvasEndFrame(&gc, &fr);
    const u64 decode_first = msSince(td);
    *last_px = presentCanvas(fb, g, canvas);
    logWrite("SPLASH  GIF %ux%u loop=%d: first frame presented (decode %lums), detect->visible=%lums",
             info->width, info->height, info->loop_count, decode_first, msSince(g_launch_tick));
    logHold(false);
    *log_held = false;

    if (g_cfg.audio)
        *audio_on = audioStart(g_audio_path, (float)g_cfg.volume / 100.0f);

    /* How long to hold: duration_ms, or with duration_ms=0 the jingle's length, or one play
     * of a finite GIF, or the default for an endless GIF without a jingle. */
    const bool until_audio_end = g_cfg.duration_ms == 0 && *audio_on;
    const bool until_anim_end = g_cfg.duration_ms == 0 && !*audio_on && info->loop_count != 0;
    const u32 hold_ms = g_cfg.duration_ms ? g_cfg.duration_ms
                       : (until_audio_end || until_anim_end) ? MAX_SPLASH_MS : DEFAULT_SPLASH_MS;
    const u64 hold_start = armGetSystemTick();

    u64 shown_at = armGetSystemTick();
    u32 delay = gifDelayMs(fr.delay_cs);
    u32 plays = 1, frames = 1, late = 0;
    u64 decode_sum = 0, decode_max = 0;
    bool animating = true, pending = false;

    for (;;) {
        const HoldResult c = cancelReason(since);
        if (c != Hold_Done)
            return c;
        if (msSince(hold_start) >= hold_ms || (until_audio_end && !audioIsPlaying()))
            break;
        if (until_anim_end && !animating && msSince(shown_at) >= delay)
            break;

        /* Decode the next frame into the canvas while the current one is on screen. */
        if (animating && !pending) {
            td = armGetSystemTick();
            r = gifNextFrame(gif, &fr, gifCanvasRow, &gc);
            if (r == GIF_END) {
                const bool more = info->loop_count == 0 || (info->loop_count > 0 && plays <= (u32)info->loop_count);
                if (more) {
                    gifRewind(gif);
                    gifCanvasReset(&gc);
                    plays++;
                    r = gifNextFrame(gif, &fr, gifCanvasRow, &gc);
                } else {
                    animating = false; /* play once / loops done: hold the last frame */
                }
            }
            if (animating) {
                if (r != GIF_OK) {
                    logWrite("SPLASH  GIF frame %u failed: %s (%d), holding last frame", frames, gifResultString(r), r);
                    animating = false;
                } else {
                    gifCanvasEndFrame(&gc, &fr);
                    pending = true;
                    const u64 d = armTicksToNs(armGetSystemTick() - td) / 1000;
                    decode_sum += d;
                    if (d > decode_max)
                        decode_max = d;
                }
            }
        }

        if (pending && msSince(shown_at) >= delay) {
            if (msSince(shown_at) > delay + 17)
                late++;
            *last_px = presentCanvas(fb, g, canvas);
            shown_at = armGetSystemTick();
            delay = gifDelayMs(fr.delay_cs);
            pending = false;
            frames++;
        } else {
            svcSleepThread(2000000ULL);
        }
    }

    logWrite("SPLASH  GIF played %u frames (%u play%s), decode avg %luus max %luus, %u late", frames, plays,
             plays == 1 ? "" : "s", frames > 1 ? (unsigned long)(decode_sum / (frames - 1)) : 0UL,
             (unsigned long)decode_max, late);
    return Hold_Done;
}

static void splashThread(void *arg) {
    Result rc = 0;
    ViLayer layer = {0};
    NWindow win = {0};
    Framebuffer fb = {0};
    PngImage *img = NULL;
    FILE *cache = NULL;
    CacheHeader cache_hdr;
    char cache_path[128] = "";
    bool cache_valid_hdr = false, need_cache_write = false, log_held = true;
    LayerAlphaMethod alpha_method = LayerAlpha_None;
    bool layer_created = false, win_created = false, fb_created = false;

    const u64 t0 = armGetSystemTick();
    const bool anim = g_cfg.test_anim;
    const bool is_gif = !anim && g_has_image && hasSuffix(g_image_path, ".gif");
    GifFile *gif = NULL;
    const FbGeom geom = anim     ? fbGeom(g_cfg.anim_width, g_cfg.anim_width * 9 / 16)
                      : is_gif   ? fbGeom(GIF_W, GIF_H)
                                 : fbGeom(SCREEN_W, SCREEN_H);
    const u32 nbuf = anim ? (g_cfg.anim_inplace ? 1 : 2) : is_gif ? 2 : g_cfg.buffers;
    /* GIFs also need a canvas and a disposal backup in the same block. */
    const size_t gif_extra = is_gif ? 2 * (((size_t)GIF_W * GIF_H * sizeof(u16) + 0xFFF) & ~0xFFFUL) : 0;
    const size_t fb_total = (((size_t)geom.size * nbuf + 0xFFF) & ~0xFFFUL) + gif_extra;

    logWrite("SPLASH  begin tid=%016lX image=%s detect->begin=%lums",
             g_program_id, g_has_image ? g_image_path : "(test pattern)", msSince(g_launch_tick));
    logWrite("SPLASH  cfg: duration=%ums buffers=%u bg=%06X home_cancel=%d audio=%d volume=%u audio_fade=%ums fade=%ums mode=%d",
             g_cfg.duration_ms, nbuf, g_cfg.background, g_cfg.home_cancel, g_cfg.audio, g_cfg.volume,
             g_cfg.audio_fade_ms, g_cfg.fade_ms, g_cfg.fade_mode);
    bool audio_on = false, audio_fade_requested = false;

    /* 0. Validate the image before touching the display, so a bad file never shows anything.
     *    A matching cache is the fast path; otherwise the PNG is opened and checked. */
    if (is_gif) {
        const GifResult gr = gifOpen(&gif, g_image_path);
        if (gr != GIF_OK) {
            logWrite("SPLASH  cannot use %s: %s (%d) -> no splash", g_image_path, gifResultString(gr), gr);
            goto cleanup;
        }
    } else if (g_has_image) {
        cachePathFor(g_image_path, cache_path, sizeof(cache_path));
        cache_valid_hdr = cacheHeaderFor(g_image_path, &cache_hdr);
        if (cache_valid_hdr)
            cache = cacheOpen(cache_path, &cache_hdr);
        if (!cache) {
            const PngResult pr = pngOpen(&img, g_image_path);
            if (pr != PNG_OK) {
                logWrite("SPLASH  cannot use %s: %s (%d) -> no splash", g_image_path, pngResultString(pr), pr);
                goto cleanup;
            }
            const PngInfo *pi = pngGetInfo(img);
            logWrite("SPLASH  image %ux%u depth=%u color_type=%u (no valid cache, decoding)",
                     pi->width, pi->height, pi->bit_depth, pi->color_type);
            need_cache_write = cache_valid_hdr;
        }
    }

    /* 1. Transient memory. */
    rc = dynheapAcquire(fb_total);
    if (R_FAILED(rc)) {
        logWrite("SPLASH  svcSetHeapSize(%lu) failed: 0x%x -> skipping splash (system pool exhausted?)",
                 (unsigned long)fb_total, rc);
        goto cleanup;
    }

    /* 2. Layer (libtesla sequence) on the persistent display. */
    STEP(ensureDisplay());
    STEP(viCreateManagedLayer(&g_display, (ViLayerFlags)0, 0, &__nx_vi_layer_id));
    STEP(viCreateLayer(&g_display, &layer));
    layer_created = true;
    STEP(viSetLayerScalingMode(&layer, ViScalingMode_FitToLayer));

    {
        s32 zmax = -1;
        const Result rc_max = viGetZOrderCountMax(&g_display, &zmax);
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
    STEP(framebufferCreate(&fb, &win, geom.w, geom.h, PIXEL_FORMAT_RGBA_4444, nbuf));
    fb_created = true;

    if (fb.fb_size != geom.size) {
        logWrite("SPLASH  fb_size mismatch (%u != %u), swizzle would be wrong -> aborting", fb.fb_size, geom.size);
        goto cleanup;
    }
    logWrite("SPLASH  layer ready in %lums (%ux%u, %u buffer%s, heap %lu KiB)", msSince(t0), geom.w, geom.h, nbuf,
             nbuf == 1 ? "" : "s", (unsigned long)(dynheapSize() / 1024));

    if (anim) {
        logHold(false);
        log_held = false;
        runAnimTest(&fb, &geom, g_cfg.anim_inplace, g_cfg.duration_ms ? g_cfg.duration_ms : DEFAULT_SPLASH_MS, t0);
        goto cleanup;
    }

    /* Find a working layer-alpha command now, at alpha 1.0, so nothing visible changes. */
    if (g_cfg.fade_ms > 0 && g_cfg.fade_mode == FadeMode_Auto)
        alpha_method = probeLayerAlpha(layer.layer_id);

    if (is_gif) {
        u16 *last_px = NULL;
        const HoldResult hr = playGif(&fb, &geom, gif, t0, &audio_on, &last_px, &log_held);
        if (hr == Hold_Error)
            goto cleanup;
        if (hr != Hold_Done) {
            logWrite("SPLASH  cancelled after %lums: %s", msSince(t0), holdResultName(hr));
            goto cleanup;
        }
        if (audio_on && audioIsPlaying()) {
            audioFadeOut(g_cfg.audio_fade_ms);
            audio_fade_requested = true;
        }
        if (!runFade(&fb, &layer, alpha_method, last_px, t0))
            logWrite("SPLASH  fade interrupted after %lums", msSince(t0));
        goto cleanup;
    }

    /* 3. Draw one frame. The buffer is dequeued for the whole decode, so nothing partial
     *    is ever shown; until framebufferEnd the stock boot screen stays visible. */
    u64 tb = armGetSystemTick();
    u16 *px = framebufferBegin(&fb, NULL);
    bool drawn = true;
    const char *source = "test pattern";
    if (cache) {
        drawn = fread(px, 1, FB_SIZE, cache) == FB_SIZE;
        source = "cache";
        fclose(cache);
        cache = NULL;
        if (!drawn) {
            /* Unreadable cache: fall back to decoding the PNG now, and rewrite it. */
            logWrite("SPLASH  cache read failed, decoding PNG instead");
            drawn = pngOpen(&img, g_image_path) == PNG_OK && drawImage(px, img);
            need_cache_write = drawn && cache_valid_hdr;
            source = "png (cache fallback)";
        }
    } else if (g_has_image) {
        drawn = drawImage(px, img);
        source = "png";
    } else {
        drawTestScreen(px);
    }
    if (!drawn)
        memset(px, 0, fb.fb_size); /* fully transparent: present nothing visible */
    framebufferEnd(&fb);

    if (!drawn)
        goto cleanup;
    logWrite("SPLASH  frame presented from %s: draw=%lums, detect->visible=%lums", source, msSince(tb),
             msSince(g_launch_tick));

    /* The image is up: the log writer may use the SD card again. */
    logHold(false);
    log_held = false;

    /* Free the decoder's file handle and buffers while we hold. */
    pngClose(img);
    img = NULL;

    /* First launch with this image: save the finished frame for next time. Done before
     * the jingle starts so the two never compete for the SD card. */
    if (need_cache_write) {
        const u64 tw = armGetSystemTick();
        cacheWrite(cache_path, &cache_hdr, px);
        logWrite("SPLASH  cache written: %s (%lums)", cache_path, msSince(tw));
    }

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
        gifClose(gif);
        if (cache)
            fclose(cache);
        if (fb_created)    framebufferClose(&fb);
        if (win_created)   nwindowClose(&win);
        if (layer_created) viDestroyManagedLayer(&layer);
        __nx_vi_layer_id = 0;
        /* The display connection stays open for the next splash (ensureDisplay). */
        dynheapRelease();
        if (log_held)
            logHold(false); /* failed before presenting */
        /* The image is gone; let the jingle finish its fade, then release audout. */
        if (audio_on)
            audioJoin();
        logWrite("SPLASH  end tid=%016lX teardown=%lums total=%lums", g_program_id, msSince(tc), msSince(t0));
    }

    atomic_store(&g_running, false);
}

/* ---- public ---- */

/* splash.gif wins over splash.png when a folder has both. */
static bool findInFolder(const char *folder, char *out, size_t out_size) {
    struct stat st;
    snprintf(out, out_size, "%s/splash.gif", folder);
    if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
        return true;
    snprintf(out, out_size, "%s/splash.png", folder);
    return stat(out, &st) == 0 && S_ISREG(st.st_mode);
}

SplashImageSource splashFindImage(u64 program_id, bool hbl, char *out, size_t out_size) {
    char folder[64];

    if (hbl) {
        if (findInFolder(SPLASH_ROOT "/hbl", out, out_size))
            return SplashImage_Hbl;
        out[0] = '\0';
        return SplashImage_None;
    }

    snprintf(folder, sizeof(folder), SPLASH_ROOT "/%016lX", program_id);
    if (findInFolder(folder, out, out_size))
        return SplashImage_Title;

    if (findInFolder(SPLASH_ROOT "/default", out, out_size))
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
