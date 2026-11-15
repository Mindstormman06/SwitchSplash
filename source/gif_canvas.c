#include "gif_canvas.h"

#include <string.h>

static const uint8_t g_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

/* Same quantisation as image_blit.c (opaque pixels). */
static inline uint16_t pack(const GifCanvas *c, uint32_t r, uint32_t g, uint32_t b, uint32_t x, uint32_t y) {
    const uint32_t t = c->dither ? g_bayer[y & 3][x & 3] : 0;
    uint32_t r4 = (r * 15 + t * 16) / 255, g4 = (g * 15 + t * 16) / 255, b4 = (b * 15 + t * 16) / 255;
    if (r4 > 15) r4 = 15;
    if (g4 > 15) g4 = 15;
    if (b4 > 15) b4 = 15;
    return (uint16_t)(r4 | (g4 << 4) | (b4 << 8) | (0xFu << 12));
}

/* Output range [*a, *b) covered by source range [s0, s1) along one axis. */
static inline void mapRange(uint32_t s0, uint32_t s1, uint32_t src, uint32_t dst, uint32_t *a, uint32_t *b) {
    *a = (uint32_t)(((uint64_t)s0 * dst + src - 1) / src);
    *b = (uint32_t)(((uint64_t)s1 * dst + src - 1) / src);
    if (*b > dst) *b = dst;
}

void gifCanvasInit(GifCanvas *c, uint16_t *px, uint16_t *backup, uint32_t out_w, uint32_t out_h,
                   uint32_t gif_w, uint32_t gif_h, uint32_t bg_rgb, bool dither) {
    memset(c, 0, sizeof(*c));
    c->px = px;
    c->backup = backup;
    c->out_w = out_w;
    c->out_h = out_h;
    c->gw = gif_w;
    c->gh = gif_h;
    c->bg_rgb = bg_rgb;
    c->dither = dither;
    if (gif_w <= out_w && gif_h <= out_h) {
        c->img_w = gif_w;
        c->img_h = gif_h;
    } else if ((uint64_t)gif_w * out_h >= (uint64_t)gif_h * out_w) {
        c->img_w = out_w;
        c->img_h = (uint32_t)(((uint64_t)gif_h * out_w + gif_w / 2) / gif_w);
    } else {
        c->img_h = out_h;
        c->img_w = (uint32_t)(((uint64_t)gif_w * out_h + gif_h / 2) / gif_h);
    }
    if (!c->img_w) c->img_w = 1;
    if (!c->img_h) c->img_h = 1;
    c->off_x = (out_w - c->img_w) / 2;
    c->off_y = (out_h - c->img_h) / 2;
    gifCanvasReset(c);
}

static void fillRect(GifCanvas *c, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    const uint32_t r = (c->bg_rgb >> 16) & 0xFF, g = (c->bg_rgb >> 8) & 0xFF, b = c->bg_rgb & 0xFF;
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = x0; x < x1; x++)
            c->px[y * c->out_w + x] = pack(c, r, g, b, x, y);
}

void gifCanvasReset(GifCanvas *c) {
    fillRect(c, 0, 0, c->out_w, c->out_h);
    c->have_prev = false;
    c->frame_started = false;
}

/* Canvas rectangle for a GIF rectangle (clipped to the logical screen). */
static bool canvasRect(const GifCanvas *c, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                       uint32_t *x0, uint32_t *y0, uint32_t *x1, uint32_t *y1) {
    if (x >= c->gw || y >= c->gh || w == 0 || h == 0)
        return false;
    const uint32_t xe = x + w > c->gw ? c->gw : x + w, ye = y + h > c->gh ? c->gh : y + h;
    uint32_t a, b;
    mapRange(x, xe, c->gw, c->img_w, &a, &b);
    *x0 = c->off_x + a;
    *x1 = c->off_x + b;
    mapRange(y, ye, c->gh, c->img_h, &a, &b);
    *y0 = c->off_y + a;
    *y1 = c->off_y + b;
    return *x1 > *x0 && *y1 > *y0;
}

static void copyRect(const GifCanvas *c, uint16_t *dst, const uint16_t *src,
                     uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) {
    for (uint32_t y = y0; y < y1; y++)
        memcpy(&dst[y * c->out_w + x0], &src[y * c->out_w + x0], (x1 - x0) * sizeof(uint16_t));
}

static void beginFrame(GifCanvas *c, const GifFrame *fr) {
    if (c->frame_started)
        return;
    c->frame_started = true;

    /* Dispose of the previous frame. */
    if (c->have_prev) {
        uint32_t x0, y0, x1, y1;
        if (canvasRect(c, c->prev_x, c->prev_y, c->prev_w, c->prev_h, &x0, &y0, &x1, &y1)) {
            if (c->prev_disposal == GIF_DISPOSE_BACKGROUND || (c->prev_disposal == GIF_DISPOSE_PREVIOUS && !c->backup))
                fillRect(c, x0, y0, x1, y1);
            else if (c->prev_disposal == GIF_DISPOSE_PREVIOUS)
                copyRect(c, c->px, c->backup, x0, y0, x1, y1);
        }
    }

    /* Remember what this frame will cover, if it asks to be undone afterwards. */
    if (fr->disposal == GIF_DISPOSE_PREVIOUS && c->backup) {
        uint32_t x0, y0, x1, y1;
        if (canvasRect(c, fr->x, fr->y, fr->w, fr->h, &x0, &y0, &x1, &y1))
            copyRect(c, c->backup, c->px, x0, y0, x1, y1);
    }
}

void gifCanvasRow(void *user, const GifFrame *fr, uint16_t row, const uint8_t *indices) {
    GifCanvas *c = user;
    beginFrame(c, fr);

    const uint32_t sy = (uint32_t)fr->y + row;
    if (sy >= c->gh || fr->x >= c->gw)
        return;
    const uint32_t sx_end = (uint32_t)fr->x + fr->w > c->gw ? c->gw : (uint32_t)fr->x + fr->w;

    uint32_t oy0, oy1, ox0, ox1;
    mapRange(sy, sy + 1, c->gh, c->img_h, &oy0, &oy1);
    mapRange(fr->x, sx_end, c->gw, c->img_w, &ox0, &ox1);

    for (uint32_t oy = oy0; oy < oy1; oy++) {
        const uint32_t cy = c->off_y + oy;
        uint16_t *dst = &c->px[cy * c->out_w + c->off_x];
        for (uint32_t ox = ox0; ox < ox1; ox++) {
            const uint32_t sx = (uint32_t)(((uint64_t)ox * c->gw) / c->img_w);
            const int idx = indices[sx - fr->x];
            if (idx == fr->transparent || idx >= fr->palette_size)
                continue;
            const uint8_t *p = &fr->palette[idx * 3];
            dst[ox] = pack(c, p[0], p[1], p[2], c->off_x + ox, cy);
        }
    }
}

void gifCanvasEndFrame(GifCanvas *c, const GifFrame *fr) {
    beginFrame(c, fr); /* frames without rows still trigger the previous disposal */
    c->have_prev = true;
    c->prev_x = fr->x;
    c->prev_y = fr->y;
    c->prev_w = fr->w;
    c->prev_h = fr->h;
    c->prev_disposal = fr->disposal;
    c->frame_started = false;
}
