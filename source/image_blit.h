#pragma once
/*
 * Turns decoded PNG rows (8-bit straight RGBA, any size) into screen rows of
 * premultiplied RGBA4444 (DESIGN.md §3, R4).
 *
 *  - Images up to the screen size are centered at 1:1.
 *  - Larger images are scaled down to fit, keeping the aspect ratio, using area
 *    averaging (each source pixel lands in exactly one output bin), done one row at a time.
 *  - Areas not covered by the image are filled with an opaque background color.
 *  - Colors are premultiplied by alpha (the vi compositor uses premultiplied alpha,
 *    verified on hardware), then quantized to 4 bits with a 4x4 ordered dither.
 *
 * Output rows are emitted strictly top to bottom through a callback. Platform-independent.
 */
#include <stdbool.h>
#include <stdint.h>

#include "png_stream.h"

/* Receives one full output row (screen_w RGBA4444 pixels, libtesla channel order:
 * r bits 0-3, g 4-7, b 8-11, a 12-15). */
typedef void (*BlitStoreRow)(void *user, uint32_t y, const uint16_t *row);

typedef struct {
    uint32_t screen_w, screen_h;
    uint32_t src_w, src_h;
    uint32_t out_w, out_h;     /* scaled image size */
    uint32_t off_x, off_y;     /* image position on screen */
    uint16_t bg;               /* background pixel */

    uint32_t *sum;             /* out_w * 4 accumulators (premultiplied) */
    uint32_t *cnt;             /* out_w counts */
    uint16_t *row;             /* screen_w output row */
    int32_t cur_bin;           /* output image row being accumulated, -1 = none */
    uint32_t next_screen_y;    /* next screen row to emit */

    BlitStoreRow store;
    void *store_user;
} Blitter;

bool blitInit(Blitter *b, uint32_t screen_w, uint32_t screen_h, uint32_t src_w, uint32_t src_h,
              uint32_t bg_rgb, BlitStoreRow store, void *store_user);
void blitFree(Blitter *b);

/* PngRowCallback-compatible: pass the Blitter as user. */
bool blitPngRow(void *blitter, uint32_t y, const uint8_t *rgba);

/* Emits the remaining rows (last image bin and bottom letterbox). Call after decoding. */
void blitFinish(Blitter *b);

/* Quantize helpers (exposed for the test pattern). */
uint16_t blitPack4444(uint32_t r8, uint32_t g8, uint32_t b8, uint32_t a8, uint32_t x, uint32_t y);
