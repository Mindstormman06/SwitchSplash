#pragma once
/*
 * Pixel-level fade for premultiplied RGBA4444 (fallback when vi layer alpha isn't
 * available, DESIGN.md M5).
 *
 * The compositor uses premultiplied alpha (verified on hardware), so fading means scaling
 * all four channels together. The source image isn't kept (memory budget), so each step
 * scales the *current* pixels from opacity level `from` to level `to` (0-15), with rounding.
 * Stepping 15 -> 14 -> ... -> 0 keeps every pixel within 1 of the exact value
 * original * level / 15 and preserves color <= alpha. Platform-independent.
 */
#include <stddef.h>
#include <stdint.h>

/* Scales every pixel from opacity level `from` to `to` (to < from <= 15). Reads `src`,
 * writes `dst` (may be the same buffer). */
void fadeScale4444(uint16_t *dst, const uint16_t *src, size_t count, unsigned from, unsigned to);
