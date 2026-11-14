/*
 * PC test harness for the streaming PNG decoder and blitter.
 *
 *   test_png <in.png> <out_rgba8.raw> <out_screen4444.raw> [bg_rrggbb]
 *
 * out_rgba8.raw:      decoded image, width*height*4 bytes (compare against PIL).
 * out_screen4444.raw: 1280x720 RGBA4444 screen, linear, little-endian u16.
 * Prints "w h result" on stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image_blit.h"
#include "png_stream.h"

typedef struct {
    FILE *raw;
    Blitter *blit;
} Ctx;

static bool onRow(void *user, uint32_t y, const uint8_t *rgba) {
    Ctx *c = user;
    (void)y;
    fwrite(rgba, 1, (size_t)c->blit->src_w * 4, c->raw);
    return blitPngRow(c->blit, y, rgba);
}

static uint32_t g_expect_y = 0;
static void storeRow(void *user, uint32_t y, const uint16_t *row) {
    FILE *f = user;
    if (y != g_expect_y)
        fprintf(stderr, "row order error: got %u expected %u\n", y, g_expect_y);
    g_expect_y++;
    fwrite(row, sizeof(uint16_t), 1280, f);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s in.png out_rgba8.raw out_screen.raw [bg]\n", argv[0]);
        return 2;
    }
    const uint32_t bg = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 16) : 0;

    PngImage *img;
    PngResult r = pngOpen(&img, argv[1]);
    if (r != PNG_OK) {
        printf("0 0 %d %s\n", r, pngResultString(r));
        return 1;
    }
    const PngInfo *info = pngGetInfo(img);

    FILE *raw = fopen(argv[2], "wb");
    FILE *scr = fopen(argv[3], "wb");
    Blitter blit;
    if (!blitInit(&blit, 1280, 720, info->width, info->height, bg, storeRow, scr)) {
        printf("0 0 nomem\n");
        return 1;
    }
    Ctx c = { raw, &blit };
    r = pngDecode(img, onRow, &c);
    blitFinish(&blit);
    if (g_expect_y != 720)
        fprintf(stderr, "screen rows emitted: %u (expected 720)\n", g_expect_y);

    printf("%u %u %d %s\n", info->width, info->height, r, pngResultString(r));
    blitFree(&blit);
    pngClose(img);
    fclose(raw);
    fclose(scr);
    return r == PNG_OK ? 0 : 1;
}
