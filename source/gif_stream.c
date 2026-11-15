#include "gif_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LZW_MAX 4096

struct GifFile {
    FILE *f;
    GifInfo info;
    uint8_t global_pal[256 * 3];
    int global_pal_size;
    long first_frame_pos;   /* file offset of the first block after the header */

    /* pending Graphic Control Extension for the next image */
    uint16_t gce_delay;
    uint8_t gce_disposal;
    int gce_transparent;

    uint8_t local_pal[256 * 3];

    /* sub-block reader */
    uint8_t block[256];
    int block_len, block_pos;
    bool blocks_done;

    /* LZW tables */
    uint16_t prefix[LZW_MAX];
    uint8_t suffix[LZW_MAX];
    uint8_t stack[LZW_MAX + 1];

    uint8_t *row;           /* one frame row of indices */
    uint16_t row_cap;
};

const char *gifResultString(GifResult r) {
    switch (r) {
        case GIF_OK:            return "ok";
        case GIF_END:           return "end";
        case GIF_ERR_OPEN:      return "cannot open file";
        case GIF_ERR_SIGNATURE: return "not a GIF file";
        case GIF_ERR_FORMAT:    return "malformed GIF";
        case GIF_ERR_TOO_LARGE: return "GIF too large";
        case GIF_ERR_NO_MEMORY: return "out of memory";
        case GIF_ERR_LZW:       return "corrupt image data";
        case GIF_ERR_TRUNCATED: return "file truncated";
    }
    return "unknown error";
}

static int rd8(GifFile *g) {
    return fgetc(g->f);
}

static bool readN(GifFile *g, void *buf, size_t n) {
    return fread(buf, 1, n, g->f) == n;
}

/* Skips a chain of data sub-blocks. */
static bool skipSubBlocks(GifFile *g) {
    for (;;) {
        const int len = rd8(g);
        if (len < 0)
            return false;
        if (len == 0)
            return true;
        if (fseek(g->f, len, SEEK_CUR) != 0)
            return false;
    }
}

GifResult gifOpen(GifFile **out, const char *path) {
    *out = NULL;
    GifFile *g = calloc(1, sizeof(*g));
    if (!g)
        return GIF_ERR_NO_MEMORY;
    g->f = fopen(path, "rb");
    if (!g->f) {
        free(g);
        return GIF_ERR_OPEN;
    }

    GifResult res = GIF_OK;
    uint8_t hdr[13];
    if (!readN(g, hdr, 13) || memcmp(hdr, "GIF", 3) != 0 || (memcmp(hdr + 3, "87a", 3) != 0 && memcmp(hdr + 3, "89a", 3) != 0)) {
        res = GIF_ERR_SIGNATURE;
        goto fail;
    }
    g->info.width = (uint16_t)(hdr[6] | (hdr[7] << 8));
    g->info.height = (uint16_t)(hdr[8] | (hdr[9] << 8));
    g->info.loop_count = -1;
    if (g->info.width == 0 || g->info.height == 0) { res = GIF_ERR_FORMAT; goto fail; }
    if (g->info.width > GIF_MAX_DIM || g->info.height > GIF_MAX_DIM) { res = GIF_ERR_TOO_LARGE; goto fail; }

    if (hdr[10] & 0x80) {
        g->global_pal_size = 2 << (hdr[10] & 7);
        if (!readN(g, g->global_pal, (size_t)g->global_pal_size * 3)) { res = GIF_ERR_TRUNCATED; goto fail; }
    }
    g->first_frame_pos = ftell(g->f);
    g->gce_transparent = -1;

    /* Pre-scan for the NETSCAPE loop block (it normally precedes the first image). */
    for (;;) {
        const int b = rd8(g);
        if (b != 0x21)
            break;
        const int label = rd8(g);
        if (label == 0xFF) {
            const int len = rd8(g);
            uint8_t app[11] = {0};
            if (len == 11 && readN(g, app, 11) && memcmp(app, "NETSCAPE2.0", 11) == 0) {
                uint8_t sub[4];
                if (rd8(g) == 3 && readN(g, sub, 3) && sub[0] == 1)
                    g->info.loop_count = sub[1] | (sub[2] << 8);
                skipSubBlocks(g);
                break;
            }
            if (len != 11)
                fseek(g->f, len, SEEK_CUR);
            if (!skipSubBlocks(g))
                break;
        } else {
            if (!skipSubBlocks(g))
                break;
        }
    }
    fseek(g->f, g->first_frame_pos, SEEK_SET);

    *out = g;
    return GIF_OK;

fail:
    fclose(g->f);
    free(g);
    return res;
}

const GifInfo *gifGetInfo(const GifFile *g) {
    return &g->info;
}

GifResult gifRewind(GifFile *g) {
    g->gce_delay = 0;
    g->gce_disposal = 0;
    g->gce_transparent = -1;
    return fseek(g->f, g->first_frame_pos, SEEK_SET) == 0 ? GIF_OK : GIF_ERR_TRUNCATED;
}

void gifClose(GifFile *g) {
    if (!g)
        return;
    if (g->f)
        fclose(g->f);
    free(g->row);
    free(g);
}

/* ---- image data ---- */

/* Next byte of the LZW data stream across sub-blocks, or -1 at the end. */
static int nextDataByte(GifFile *g) {
    while (g->block_pos >= g->block_len) {
        if (g->blocks_done)
            return -1;
        const int len = rd8(g);
        if (len <= 0) {
            g->blocks_done = true;
            return -1;
        }
        if (!readN(g, g->block, (size_t)len)) {
            g->blocks_done = true;
            return -1;
        }
        g->block_len = len;
        g->block_pos = 0;
    }
    return g->block[g->block_pos++];
}

/* Real row number for the n-th decoded row of an interlaced frame. */
static uint16_t interlacedRow(uint16_t n, uint16_t h) {
    static const uint8_t start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
    for (int pass = 0; pass < 4; pass++) {
        const uint16_t count = h > start[pass] ? (uint16_t)((h - start[pass] + step[pass] - 1) / step[pass]) : 0;
        if (n < count)
            return (uint16_t)(start[pass] + n * step[pass]);
        n = (uint16_t)(n - count);
    }
    return 0;
}

static GifResult decodeImage(GifFile *g, GifFrame *fr, GifRowCallback cb, void *user) {
    const int min_size = rd8(g);
    if (min_size < 2 || min_size > 11)
        return GIF_ERR_LZW;

    if (fr->w > g->row_cap) {
        free(g->row);
        g->row = malloc(fr->w);
        if (!g->row)
            return GIF_ERR_NO_MEMORY;
        g->row_cap = fr->w;
    }

    g->block_len = g->block_pos = 0;
    g->blocks_done = false;

    const int clear = 1 << min_size, end = clear + 1;
    int width = min_size + 1, next = end + 1, prev = -1;
    uint32_t bits = 0;
    int nbits = 0;
    uint8_t first = 0;

    uint32_t col = 0;
    uint16_t rows_done = 0;
    const uint32_t total_rows = fr->h;
    GifResult res = GIF_OK;

    while (rows_done < total_rows) {
        while (nbits < width) {
            const int b = nextDataByte(g);
            if (b < 0) {
                res = GIF_ERR_TRUNCATED;
                goto out;
            }
            bits |= (uint32_t)b << nbits;
            nbits += 8;
        }
        int code = (int)(bits & ((1u << width) - 1));
        bits >>= width;
        nbits -= width;

        if (code == clear) {
            width = min_size + 1;
            next = end + 1;
            prev = -1;
            continue;
        }
        if (code == end)
            break;

        /* Unpack `code` onto the stack (reversed). */
        int sp = 0, c = code;
        if (prev < 0) {
            if (code >= clear) { res = GIF_ERR_LZW; goto out; }
            g->stack[sp++] = (uint8_t)code;
            first = (uint8_t)code;
        } else {
            if (code > next || (code == next && next >= LZW_MAX)) { res = GIF_ERR_LZW; goto out; }
            if (code == next) { /* KwKwK case */
                g->stack[sp++] = first;
                c = prev;
            }
            while (c >= clear) {
                if (sp >= LZW_MAX) { res = GIF_ERR_LZW; goto out; }
                g->stack[sp++] = g->suffix[c];
                c = g->prefix[c];
            }
            g->stack[sp++] = (uint8_t)c;
            first = (uint8_t)c;
            if (next < LZW_MAX) {
                g->prefix[next] = (uint16_t)prev;
                g->suffix[next] = first;
                next++;
                if (next == (1 << width) && width < 12)
                    width++;
            }
        }
        prev = code;

        /* Emit the stack into rows. */
        while (sp > 0) {
            g->row[col++] = g->stack[--sp];
            if (col == fr->w) {
                const uint16_t r = fr->interlaced ? interlacedRow(rows_done, fr->h) : rows_done;
                cb(user, fr, r, g->row);
                col = 0;
                if (++rows_done == total_rows) {
                    sp = 0;
                    break;
                }
            }
        }
    }

out:
    /* Skip whatever is left of the image's sub-blocks (trailing codes, padding). */
    if (!g->blocks_done)
        skipSubBlocks(g);
    if (res == GIF_ERR_TRUNCATED && rows_done > 0)
        res = GIF_OK; /* a short frame still displays (browsers do the same) */
    return res;
}

GifResult gifNextFrame(GifFile *g, GifFrame *fr, GifRowCallback cb, void *user) {
    for (;;) {
        const int b = rd8(g);
        if (b < 0 || b == 0x3B)
            return GIF_END;

        if (b == 0x21) { /* extension */
            const int label = rd8(g);
            if (label == 0xF9) {
                const int len = rd8(g);
                uint8_t d[4];
                if (len != 4 || !readN(g, d, 4))
                    return GIF_ERR_FORMAT;
                g->gce_disposal = (d[0] >> 2) & 7;
                g->gce_delay = (uint16_t)(d[1] | (d[2] << 8));
                g->gce_transparent = (d[0] & 1) ? d[3] : -1;
            }
            if (!skipSubBlocks(g))
                return GIF_ERR_TRUNCATED;
            continue;
        }

        if (b != 0x2C)
            return GIF_ERR_FORMAT;

        /* Image descriptor */
        uint8_t d[9];
        if (!readN(g, d, 9))
            return GIF_ERR_TRUNCATED;
        memset(fr, 0, sizeof(*fr));
        fr->x = (uint16_t)(d[0] | (d[1] << 8));
        fr->y = (uint16_t)(d[2] | (d[3] << 8));
        fr->w = (uint16_t)(d[4] | (d[5] << 8));
        fr->h = (uint16_t)(d[6] | (d[7] << 8));
        fr->interlaced = (d[8] & 0x40) != 0;
        fr->delay_cs = g->gce_delay;
        fr->disposal = g->gce_disposal;
        fr->transparent = g->gce_transparent;
        if (d[8] & 0x80) {
            fr->palette_size = 2 << (d[8] & 7);
            if (!readN(g, g->local_pal, (size_t)fr->palette_size * 3))
                return GIF_ERR_TRUNCATED;
            fr->palette = g->local_pal;
        } else {
            fr->palette = g->global_pal;
            fr->palette_size = g->global_pal_size;
        }
        /* GCE applies to one image only. */
        g->gce_delay = 0;
        g->gce_disposal = 0;
        g->gce_transparent = -1;

        if (fr->w == 0 || fr->h == 0 || fr->w > GIF_MAX_DIM || fr->h > GIF_MAX_DIM) {
            /* Empty frame: consume its data and report it (it can still carry a delay). */
            rd8(g);
            skipSubBlocks(g);
            fr->w = fr->h = 0;
            return GIF_OK;
        }
        if (fr->palette_size == 0)
            return GIF_ERR_FORMAT;
        return decodeImage(g, fr, cb, user);
    }
}
