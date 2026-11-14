#include "fade_pixels.h"

void fadeScale4444(uint16_t *dst, const uint16_t *src, size_t count, unsigned from, unsigned to) {
    if (from == 0 || from > 15 || to >= from) {
        if (dst != src)
            for (size_t i = 0; i < count; i++)
                dst[i] = src[i];
        return;
    }

    /* Per-nibble lookup, then a 256-entry table for a whole byte (two nibbles).
     * Re-estimate the full-opacity value (v * 15 / from) and scale that to the new level,
     * instead of scaling v directly, so rounding errors don't compound across steps. */
    uint8_t nib[16];
    for (unsigned v = 0; v < 16; v++) {
        unsigned o = (v * 15 + from / 2) / from;
        if (o > 15) o = 15;
        unsigned nv = (o * to + 7) / 15;
        if (nv > to) nv = to;
        nib[v] = (uint8_t)nv;
    }
    uint8_t byte_lut[256];
    for (unsigned b = 0; b < 256; b++)
        byte_lut[b] = (uint8_t)(nib[b & 0xF] | (nib[b >> 4] << 4));

    for (size_t i = 0; i < count; i++) {
        const uint16_t p = src[i];
        dst[i] = (uint16_t)(byte_lut[p & 0xFF] | (byte_lut[p >> 8] << 8));
    }
}
