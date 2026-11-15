#pragma once
/*
 * Composites decoded GIF frames into an output canvas of RGBA4444 pixels (linear,
 * out_w x out_h), following the GIF rules: frame rectangles, transparent index, local
 * palettes, and disposal (none / background / previous) applied before the next frame.
 *
 * GIFs that fit the canvas are centred at 1:1; larger ones are scaled down to fit (keeping
 * the aspect ratio) with nearest-neighbour sampling, cheap enough to run every frame.
 * Transparent and disposed areas show the opaque background colour, like PNG splashes.
 * Platform-independent.
 */
#include <stdbool.h>
#include <stdint.h>

#include "gif_stream.h"

typedef struct {
    uint16_t *px;          /* out_w * out_h canvas (caller-owned) */
    uint16_t *backup;      /* same size, for GIF_DISPOSE_PREVIOUS; may be NULL (then treated as background) */
    uint32_t out_w, out_h;
    uint32_t gw, gh;       /* GIF logical screen */
    uint32_t img_w, img_h, off_x, off_y; /* GIF area inside the canvas */
    uint32_t bg_rgb;
    uint16_t bg;
    bool dither;

    bool frame_started;
    bool have_prev;
    uint16_t prev_x, prev_y, prev_w, prev_h;
    uint8_t prev_disposal;
} GifCanvas;

void gifCanvasInit(GifCanvas *c, uint16_t *px, uint16_t *backup, uint32_t out_w, uint32_t out_h,
                   uint32_t gif_w, uint32_t gif_h, uint32_t bg_rgb, bool dither);

/* Clears the canvas to the background and forgets the previous frame (start / loop). */
void gifCanvasReset(GifCanvas *c);

/* GifRowCallback: pass the GifCanvas as `user`. Applies the previous frame's disposal
 * lazily on the first row of a new frame. */
void gifCanvasRow(void *user, const GifFrame *frame, uint16_t row, const uint8_t *indices);

/* Call after gifNextFrame() returned GIF_OK for a frame (handles frames with no rows). */
void gifCanvasEndFrame(GifCanvas *c, const GifFrame *frame);
