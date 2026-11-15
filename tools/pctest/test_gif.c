/*
 * PC test harness for gif_stream + gif_canvas.
 *
 *   test_gif <in.gif> <out.raw> [out_w out_h bg_rrggbb]
 *
 * Decodes every frame (no dithering), appends the out_w x out_h RGBA4444 canvas after each
 * frame to out.raw, then replays the first frame after gifRewind() and appends it again.
 * Prints "W H loop" then one "delay disposal x y w h" line per frame on stdout.
 */
#include <stdio.h>
#include <stdlib.h>

#include "gif_canvas.h"
#include "gif_stream.h"

int main(int argc, char **argv) {
    if (argc < 3)
        return 2;
    const uint32_t ow = argc > 4 ? (uint32_t)atoi(argv[3]) : 640;
    const uint32_t oh = argc > 4 ? (uint32_t)atoi(argv[4]) : 360;
    const uint32_t bg = argc > 5 ? (uint32_t)strtoul(argv[5], NULL, 16) : 0;

    GifFile *g;
    GifResult r = gifOpen(&g, argv[1]);
    if (r != GIF_OK) {
        printf("ERR %d %s\n", r, gifResultString(r));
        return 1;
    }
    const GifInfo *info = gifGetInfo(g);
    printf("%u %u %d\n", info->width, info->height, info->loop_count);

    uint16_t *px = malloc((size_t)ow * oh * 2), *backup = malloc((size_t)ow * oh * 2);
    GifCanvas c;
    gifCanvasInit(&c, px, backup, ow, oh, info->width, info->height, bg, false);
    FILE *out = fopen(argv[2], "wb");

    GifFrame fr;
    int frames = 0;
    while ((r = gifNextFrame(g, &fr, gifCanvasRow, &c)) == GIF_OK) {
        gifCanvasEndFrame(&c, &fr);
        fwrite(px, 2, (size_t)ow * oh, out);
        printf("%u %u %u %u %u %u\n", fr.delay_cs, fr.disposal, fr.x, fr.y, fr.w, fr.h);
        frames++;
    }
    if (r != GIF_END) {
        printf("ERR %d %s after %d frames\n", r, gifResultString(r), frames);
        return 1;
    }

    /* Loop: rewind, reset, first frame again. */
    gifRewind(g);
    gifCanvasReset(&c);
    if (gifNextFrame(g, &fr, gifCanvasRow, &c) == GIF_OK) {
        gifCanvasEndFrame(&c, &fr);
        fwrite(px, 2, (size_t)ow * oh, out);
    }

    fclose(out);
    gifClose(g);
    free(px);
    free(backup);
    return 0;
}
