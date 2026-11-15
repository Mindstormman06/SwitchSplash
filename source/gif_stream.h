#pragma once
/*
 * Streaming GIF decoder (GIF87a / GIF89a).
 *
 * Decodes one frame at a time, one row at a time: sub-block reader -> LZW (12-bit,
 * 16 KiB of tables) -> palette indices -> callback. Never holds a whole frame. Handles
 * global/local colour tables, the Graphic Control Extension (delay, disposal, transparent
 * index), the NETSCAPE2.0 loop count, and interlaced frames (rows are delivered with their
 * real row numbers). Frames can be replayed with gifRewind().
 *
 * Platform-independent (plain C + stdio) so it can be tested on a PC.
 */
#include <stdbool.h>
#include <stdint.h>

#define GIF_MAX_DIM 4096

typedef enum {
    GIF_OK = 0,
    GIF_END = 1,               /* no more frames (trailer reached) */
    GIF_ERR_OPEN = -1,
    GIF_ERR_SIGNATURE = -2,
    GIF_ERR_FORMAT = -3,
    GIF_ERR_TOO_LARGE = -5,
    GIF_ERR_NO_MEMORY = -6,
    GIF_ERR_LZW = -7,
    GIF_ERR_TRUNCATED = -8,
} GifResult;

enum {
    GIF_DISPOSE_NONE = 0,       /* 0 and 1: leave the frame in place */
    GIF_DISPOSE_KEEP = 1,
    GIF_DISPOSE_BACKGROUND = 2, /* clear the frame's rectangle afterwards */
    GIF_DISPOSE_PREVIOUS = 3,   /* restore what was under the frame afterwards */
};

typedef struct {
    uint16_t width, height;     /* logical screen */
    int loop_count;             /* -1: no NETSCAPE block (play once); 0: forever; n: n extra plays */
} GifInfo;

typedef struct {
    uint16_t x, y, w, h;        /* frame rectangle on the logical screen */
    uint16_t delay_cs;          /* centiseconds, as stored (0 = unspecified) */
    uint8_t disposal;
    int transparent;            /* palette index, or -1 */
    bool interlaced;
    const uint8_t *palette;     /* RGB triplets (local if present, else global) */
    int palette_size;
} GifFrame;

typedef struct GifFile GifFile;

/* Called for every row of the frame rectangle (row = 0..h-1, real order even when interlaced).
 * `indices` holds w palette indices. */
typedef void (*GifRowCallback)(void *user, const GifFrame *frame, uint16_t row, const uint8_t *indices);

GifResult gifOpen(GifFile **out, const char *path);
const GifInfo *gifGetInfo(const GifFile *g);

/* Decodes the next frame. Fills *frame before the first row callback. Returns GIF_END
 * after the last frame. */
GifResult gifNextFrame(GifFile *g, GifFrame *frame, GifRowCallback cb, void *user);

/* Restarts at the first frame. */
GifResult gifRewind(GifFile *g);

void gifClose(GifFile *g);
const char *gifResultString(GifResult r);
