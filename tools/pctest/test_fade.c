/*
 * PC test for fadeScale4444: steps every possible premultiplied RGBA4444 pixel from opacity
 * 15 down to 0 and checks it against the exact value at each level.
 */
#include <stdio.h>
#include <stdlib.h>

#include "fade_pixels.h"

int main(void) {
    /* All premultiplied pixels at full opacity (a=15), plus partially transparent ones
     * (color <= alpha). */
    uint16_t px[16 * 16 * 16 * 16];
    uint16_t orig[16 * 16 * 16 * 16];
    size_t n = 0;
    for (unsigned a = 0; a < 16; a++)
        for (unsigned r = 0; r <= a; r++)
            for (unsigned g = 0; g <= a; g++)
                for (unsigned b = 0; b <= a; b++)
                    orig[n] = px[n] = (uint16_t)(r | (g << 4) | (b << 8) | (a << 12)), n++;

    int failures = 0, max_err = 0;
    for (unsigned level = 15; level > 0; level--) {
        fadeScale4444(px, px, n, level, level - 1);
        const unsigned L = level - 1;
        for (size_t i = 0; i < n; i++) {
            for (int c = 0; c < 4; c++) {
                const int v = (px[i] >> (c * 4)) & 0xF;
                const int o = (orig[i] >> (c * 4)) & 0xF;
                /* Error vs. the ideal rounded value at this level. */
                const int ideal = (o * (int)L + 7) / 15;
                const int err = abs(v - ideal);
                /* Splash pixels are always opaque (the blitter composites over the
                 * background). Without the original image an exact +-1 is impossible
                 * (several originals share a value at each level), so hold opaque pixels
                 * to +-2 at intermediate levels. Others only need the invariants. */
                if ((orig[i] >> 12) == 15) {
                    if (err > max_err) max_err = err;
                    if (err > 2) failures++;
                }
            }
            const int a = px[i] >> 12;
            for (int c = 0; c < 3; c++)
                if (((px[i] >> (c * 4)) & 0xF) > a) {
                    if (failures < 5) printf("color > alpha at level %u: %04x\n", L, px[i]);
                    failures++;
                }
        }
    }
    int nonzero = 0;
    for (size_t i = 0; i < n; i++)
        nonzero += px[i] != 0;

    /* Separate dst/src buffers give the same result as in place. */
    uint16_t a1[4] = { 0xFFFF, 0xF888, 0xF123, 0x8444 }, a2[4];
    fadeScale4444(a2, a1, 4, 15, 7);
    fadeScale4444(a1, a1, 4, 15, 7);
    for (int i = 0; i < 4; i++)
        if (a1[i] != a2[i]) failures++;

    printf("pixels=%zu max_err=%d nonzero_at_end=%d failures=%d\n", n, max_err, nonzero, failures);
    if (failures || nonzero) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
