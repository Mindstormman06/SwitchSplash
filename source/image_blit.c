#include "image_blit.h"

#include <stdlib.h>
#include <string.h>

/* 4x4 Bayer matrix, values 0..15. */
static const uint8_t g_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/* 8-bit -> 4-bit with ordered dithering. The threshold adds 0..15/16 of one 4-bit step
 * before truncation, so the average over a 4x4 tile matches the 8-bit value. */
static inline uint32_t q4(uint32_t v8, uint32_t t) {
    const uint32_t q = (v8 * 15 + t * 16) / 255;
    return q > 15 ? 15 : q;
}

uint16_t blitPack4444(uint32_t r8, uint32_t g8, uint32_t b8, uint32_t a8, uint32_t x, uint32_t y) {
    /* Alpha is rounded, not dithered, and premultiplied color is clamped to it so a pixel
     * can never be brighter than its coverage allows (that would glow when composited). */
    const uint32_t a4 = (a8 * 15 + 127) / 255;
    const uint32_t t = g_bayer[y & 3][x & 3];
    uint32_t r4 = q4(r8, t), g4 = q4(g8, t), b4 = q4(b8, t);
    if (r4 > a4) r4 = a4;
    if (g4 > a4) g4 = a4;
    if (b4 > a4) b4 = a4;
    return (uint16_t)(r4 | (g4 << 4) | (b4 << 8) | (a4 << 12));
}

bool blitInit(Blitter *b, uint32_t screen_w, uint32_t screen_h, uint32_t src_w, uint32_t src_h,
              uint32_t bg_rgb, BlitStoreRow store, void *store_user) {
    memset(b, 0, sizeof(*b));
    b->screen_w = screen_w;
    b->screen_h = screen_h;
    b->src_w = src_w;
    b->src_h = src_h;
    b->store = store;
    b->store_user = store_user;
    b->cur_bin = -1;

    if (src_w <= screen_w && src_h <= screen_h) {
        b->out_w = src_w;
        b->out_h = src_h;
    } else if ((uint64_t)src_w * screen_h >= (uint64_t)src_h * screen_w) {
        /* Wider than the screen's aspect ratio: fit width. */
        b->out_w = screen_w;
        b->out_h = (uint32_t)(((uint64_t)src_h * screen_w + src_w / 2) / src_w);
    } else {
        b->out_h = screen_h;
        b->out_w = (uint32_t)(((uint64_t)src_w * screen_h + src_h / 2) / src_h);
    }
    if (b->out_w == 0) b->out_w = 1;
    if (b->out_h == 0) b->out_h = 1;
    b->off_x = (screen_w - b->out_w) / 2;
    b->off_y = (screen_h - b->out_h) / 2;

    b->bg = (uint16_t)(((bg_rgb >> 20) & 0xF) | (((bg_rgb >> 12) & 0xF) << 4) | (((bg_rgb >> 4) & 0xF) << 8) | (0xF << 12));

    b->sum = calloc((size_t)b->out_w * 4, sizeof(uint32_t));
    b->cnt = calloc(b->out_w, sizeof(uint32_t));
    b->row = malloc((size_t)screen_w * sizeof(uint16_t));
    if (!b->sum || !b->cnt || !b->row) {
        blitFree(b);
        return false;
    }
    return true;
}

void blitFree(Blitter *b) {
    free(b->sum);
    free(b->cnt);
    free(b->row);
    b->sum = NULL;
    b->cnt = NULL;
    b->row = NULL;
}

static void emitBackgroundRow(Blitter *b) {
    for (uint32_t x = 0; x < b->screen_w; x++)
        b->row[x] = b->bg;
    b->store(b->store_user, b->next_screen_y++, b->row);
}

/* Emits background rows until the screen row for image bin `bin` is next. */
static void emitBackgroundUntil(Blitter *b, uint32_t screen_y) {
    while (b->next_screen_y < screen_y && b->next_screen_y < b->screen_h)
        emitBackgroundRow(b);
}

static void flushBin(Blitter *b) {
    if (b->cur_bin < 0)
        return;
    const uint32_t sy = b->off_y + (uint32_t)b->cur_bin;
    emitBackgroundUntil(b, sy);

    uint16_t *row = b->row;
    for (uint32_t x = 0; x < b->off_x; x++)
        row[x] = b->bg;
    for (uint32_t x = 0; x < b->out_w; x++) {
        const uint32_t n = b->cnt[x];
        const uint32_t sx = b->off_x + x;
        if (n == 0) {
            row[sx] = b->bg; /* cannot happen when downscaling; defensive */
            continue;
        }
        const uint32_t *s = &b->sum[x * 4];
        const uint32_t a = (s[3] + n / 2) / n;
        const uint32_t r = (s[0] + n / 2) / n, g = (s[1] + n / 2) / n, bl = (s[2] + n / 2) / n;
        if (a == 255) {
            row[sx] = blitPack4444(r, g, bl, 255, sx, sy);
        } else {
            /* Composite over the opaque background (premultiplied "over"), so transparent
             * parts of the image show the background color, not the game. */
            const uint32_t ia = 255 - a;
            const uint32_t br = ((b->bg >> 0) & 0xF) * 17, bgc = ((b->bg >> 4) & 0xF) * 17, bb = ((b->bg >> 8) & 0xF) * 17;
            row[sx] = blitPack4444(r + (br * ia + 127) / 255, g + (bgc * ia + 127) / 255, bl + (bb * ia + 127) / 255, 255, sx, sy);
        }
    }
    for (uint32_t x = b->off_x + b->out_w; x < b->screen_w; x++)
        row[x] = b->bg;

    b->store(b->store_user, b->next_screen_y++, row);

    memset(b->sum, 0, (size_t)b->out_w * 4 * sizeof(uint32_t));
    memset(b->cnt, 0, (size_t)b->out_w * sizeof(uint32_t));
    b->cur_bin = -1;
}

bool blitPngRow(void *user, uint32_t y, const uint8_t *rgba) {
    Blitter *b = user;
    const int32_t bin = (int32_t)(((uint64_t)y * b->out_h) / b->src_h);
    if (bin != b->cur_bin) {
        flushBin(b);
        b->cur_bin = bin;
    }

    for (uint32_t x = 0; x < b->src_w; x++) {
        const uint32_t ox = (uint32_t)(((uint64_t)x * b->out_w) / b->src_w);
        const uint8_t *p = &rgba[x * 4];
        const uint32_t a = p[3];
        uint32_t *s = &b->sum[ox * 4];
        /* Accumulate premultiplied color so averaging across alpha edges is correct. */
        s[0] += (p[0] * a + 127) / 255;
        s[1] += (p[1] * a + 127) / 255;
        s[2] += (p[2] * a + 127) / 255;
        s[3] += a;
        b->cnt[ox]++;
    }
    return true;
}

void blitFinish(Blitter *b) {
    flushBin(b);
    while (b->next_screen_y < b->screen_h)
        emitBackgroundRow(b);
}
