#pragma once
/*
 * Streaming PNG decoder (DESIGN.md §2, M3).
 *
 * Decodes one row at a time: inflate (tinfl, 32 KiB window) -> unfilter (needs only the
 * previous row) -> convert to 8-bit RGBA -> callback. Never holds the whole image.
 *
 * Supported: all standard color types (gray, RGB, palette, gray+alpha, RGBA), bit depths
 * 1/2/4/8/16, PLTE, tRNS. Not supported: Adam7 interlacing (rejected with an error).
 * Width/height limit: PNG_MAX_DIM.
 *
 * Platform-independent (plain C + stdio) so it can be tested on a PC.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define PNG_MAX_DIM 4096

typedef enum {
    PNG_OK = 0,
    PNG_ERR_OPEN = -1,
    PNG_ERR_SIGNATURE = -2,
    PNG_ERR_FORMAT = -3,       /* malformed chunk structure */
    PNG_ERR_UNSUPPORTED = -4,  /* interlaced, bad depth/type combination */
    PNG_ERR_TOO_LARGE = -5,
    PNG_ERR_NO_MEMORY = -6,
    PNG_ERR_INFLATE = -7,
    PNG_ERR_TRUNCATED = -8,
    PNG_ERR_ABORTED = -9,      /* row callback asked to stop */
} PngResult;

typedef struct {
    uint32_t width, height;
    uint8_t bit_depth, color_type, interlace;
} PngInfo;

typedef struct PngImage PngImage;

/* Called once per image row, top to bottom. `rgba` holds width*4 bytes (straight alpha).
 * Return false to abort decoding. */
typedef bool (*PngRowCallback)(void *user, uint32_t y, const uint8_t *rgba);

/* Opens the file and parses chunks up to the first IDAT. On success *out must be freed
 * with pngClose. */
PngResult pngOpen(PngImage **out, const char *path);
const PngInfo *pngGetInfo(const PngImage *img);

/* Decodes all rows. Can only be called once per pngOpen. */
PngResult pngDecode(PngImage *img, PngRowCallback cb, void *user);

void pngClose(PngImage *img);

const char *pngResultString(PngResult r);
