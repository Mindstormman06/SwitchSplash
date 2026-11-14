#include "png_stream.h"

#include <stdlib.h>
#include <string.h>

#include "miniz/miniz.h"

#define IN_BUF_SIZE 8192

struct PngImage {
    FILE *f;
    PngInfo info;

    uint32_t channels;       /* samples per pixel */
    uint32_t bits_pp;        /* bits per pixel */
    uint32_t filter_bpp;     /* bytes per pixel for filtering, min 1 */
    uint32_t row_bytes;      /* bytes per scanline, without the filter byte */

    /* Palette and transparency. */
    uint8_t palette[256][4];
    uint32_t palette_size;
    bool has_trns_key;
    uint16_t trns_key[3];    /* gray, or R G B, in raw sample units */

    /* IDAT reader state. */
    uint32_t idat_remaining; /* bytes left in the current IDAT chunk */
    bool idat_done;          /* hit a non-IDAT chunk after the IDAT run */

    bool decoded;
};

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static bool readExact(FILE *f, void *buf, size_t n) {
    return fread(buf, 1, n, f) == n;
}

static bool skipBytes(FILE *f, uint32_t n) {
    return fseek(f, (long)n, SEEK_CUR) == 0;
}

const char *pngResultString(PngResult r) {
    switch (r) {
        case PNG_OK:              return "ok";
        case PNG_ERR_OPEN:        return "cannot open file";
        case PNG_ERR_SIGNATURE:   return "not a PNG file";
        case PNG_ERR_FORMAT:      return "malformed PNG";
        case PNG_ERR_UNSUPPORTED: return "unsupported PNG (interlaced or invalid depth/color type)";
        case PNG_ERR_TOO_LARGE:   return "image too large";
        case PNG_ERR_NO_MEMORY:   return "out of memory";
        case PNG_ERR_INFLATE:     return "corrupt compressed data";
        case PNG_ERR_TRUNCATED:   return "file truncated";
        case PNG_ERR_ABORTED:     return "aborted";
    }
    return "unknown error";
}

static bool validDepth(uint8_t ct, uint8_t d) {
    switch (ct) {
        case 0: return d == 1 || d == 2 || d == 4 || d == 8 || d == 16;
        case 3: return d == 1 || d == 2 || d == 4 || d == 8;
        case 2: case 4: case 6: return d == 8 || d == 16;
        default: return false;
    }
}

PngResult pngOpen(PngImage **out, const char *path) {
    *out = NULL;

    PngImage *img = calloc(1, sizeof(*img));
    if (!img)
        return PNG_ERR_NO_MEMORY;

    PngResult res = PNG_OK;
    img->f = fopen(path, "rb");
    if (!img->f) {
        free(img);
        return PNG_ERR_OPEN;
    }

    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    uint8_t hdr[8];
    if (!readExact(img->f, hdr, 8) || memcmp(hdr, sig, 8) != 0) {
        res = PNG_ERR_SIGNATURE;
        goto fail;
    }

    bool have_ihdr = false;
    for (;;) {
        uint8_t ch[8];
        if (!readExact(img->f, ch, 8)) { res = PNG_ERR_TRUNCATED; goto fail; }
        const uint32_t len = be32(ch);
        const uint8_t *type = ch + 4;
        if (len > 0x7FFFFFFF) { res = PNG_ERR_FORMAT; goto fail; }

        if (!memcmp(type, "IHDR", 4)) {
            uint8_t d[13];
            if (len != 13 || !readExact(img->f, d, 13)) { res = PNG_ERR_FORMAT; goto fail; }
            img->info.width = be32(d);
            img->info.height = be32(d + 4);
            img->info.bit_depth = d[8];
            img->info.color_type = d[9];
            img->info.interlace = d[12];
            if (d[10] != 0 || d[11] != 0) { res = PNG_ERR_UNSUPPORTED; goto fail; }
            have_ihdr = true;
        } else if (!have_ihdr) {
            res = PNG_ERR_FORMAT;
            goto fail;
        } else if (!memcmp(type, "PLTE", 4)) {
            if (len % 3 != 0 || len > 256 * 3) { res = PNG_ERR_FORMAT; goto fail; }
            uint8_t p[256 * 3];
            if (!readExact(img->f, p, len)) { res = PNG_ERR_TRUNCATED; goto fail; }
            img->palette_size = len / 3;
            for (uint32_t i = 0; i < img->palette_size; i++) {
                img->palette[i][0] = p[i * 3];
                img->palette[i][1] = p[i * 3 + 1];
                img->palette[i][2] = p[i * 3 + 2];
                img->palette[i][3] = 255;
            }
        } else if (!memcmp(type, "tRNS", 4)) {
            uint8_t t[256];
            if (len > sizeof(t) || !readExact(img->f, t, len)) { res = PNG_ERR_FORMAT; goto fail; }
            if (img->info.color_type == 3) {
                for (uint32_t i = 0; i < len && i < 256; i++)
                    img->palette[i][3] = t[i];
            } else if (img->info.color_type == 0 && len >= 2) {
                img->has_trns_key = true;
                img->trns_key[0] = (uint16_t)((t[0] << 8) | t[1]);
            } else if (img->info.color_type == 2 && len >= 6) {
                img->has_trns_key = true;
                for (int i = 0; i < 3; i++)
                    img->trns_key[i] = (uint16_t)((t[i * 2] << 8) | t[i * 2 + 1]);
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            img->idat_remaining = len;
            break; /* positioned at the start of the IDAT data */
        } else if (!memcmp(type, "IEND", 4)) {
            res = PNG_ERR_TRUNCATED; /* no image data */
            goto fail;
        } else {
            if (!skipBytes(img->f, len)) { res = PNG_ERR_TRUNCATED; goto fail; }
        }

        if (!skipBytes(img->f, 4)) { res = PNG_ERR_TRUNCATED; goto fail; } /* CRC (not verified) */
    }

    const PngInfo *in = &img->info;
    if (in->interlace != 0 || !validDepth(in->color_type, in->bit_depth)) {
        res = PNG_ERR_UNSUPPORTED;
        goto fail;
    }
    if (in->width == 0 || in->height == 0) { res = PNG_ERR_FORMAT; goto fail; }
    if (in->width > PNG_MAX_DIM || in->height > PNG_MAX_DIM) { res = PNG_ERR_TOO_LARGE; goto fail; }
    if (in->color_type == 3 && img->palette_size == 0) { res = PNG_ERR_FORMAT; goto fail; }

    static const uint8_t channels_for[7] = { 1, 0, 3, 1, 2, 0, 4 };
    img->channels = channels_for[in->color_type];
    img->bits_pp = img->channels * in->bit_depth;
    img->filter_bpp = img->bits_pp >= 8 ? img->bits_pp / 8 : 1;
    img->row_bytes = (uint32_t)(((uint64_t)in->width * img->bits_pp + 7) / 8);

    *out = img;
    return PNG_OK;

fail:
    fclose(img->f);
    free(img);
    return res;
}

const PngInfo *pngGetInfo(const PngImage *img) {
    return &img->info;
}

void pngClose(PngImage *img) {
    if (!img)
        return;
    if (img->f)
        fclose(img->f);
    free(img);
}

/* Reads up to `cap` bytes of IDAT payload, crossing chunk boundaries. Returns bytes read;
 * 0 means the IDAT run is over (or an error). *error is set on I/O/format problems. */
static size_t readIdat(PngImage *img, uint8_t *buf, size_t cap, bool *error) {
    size_t total = 0;
    while (total < cap && !img->idat_done) {
        if (img->idat_remaining == 0) {
            uint8_t ch[12]; /* CRC of previous chunk + next chunk header */
            if (!readExact(img->f, ch, 12)) { *error = true; return total; }
            if (memcmp(ch + 8, "IDAT", 4) != 0) {
                img->idat_done = true;
                break;
            }
            img->idat_remaining = be32(ch + 4);
            continue;
        }
        size_t want = cap - total;
        if (want > img->idat_remaining)
            want = img->idat_remaining;
        const size_t got = fread(buf + total, 1, want, img->f);
        if (got == 0) { *error = true; return total; }
        total += got;
        img->idat_remaining -= (uint32_t)got;
    }
    return total;
}

static inline uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
    const int p = (int)a + b - c;
    const int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

static bool unfilter(uint8_t filter, uint8_t *cur, const uint8_t *prev, uint32_t n, uint32_t bpp) {
    switch (filter) {
        case 0: break;
        case 1:
            for (uint32_t i = bpp; i < n; i++) cur[i] += cur[i - bpp];
            break;
        case 2:
            for (uint32_t i = 0; i < n; i++) cur[i] += prev[i];
            break;
        case 3:
            for (uint32_t i = 0; i < n; i++) {
                const uint32_t left = i >= bpp ? cur[i - bpp] : 0;
                cur[i] += (uint8_t)((left + prev[i]) >> 1);
            }
            break;
        case 4:
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t a = i >= bpp ? cur[i - bpp] : 0;
                const uint8_t c = i >= bpp ? prev[i - bpp] : 0;
                cur[i] += paeth(a, prev[i], c);
            }
            break;
        default:
            return false;
    }
    return true;
}

/* Raw sample i (sub-byte or 8/16-bit) from an unfiltered row. */
static inline uint32_t sampleAt(const uint8_t *row, uint32_t i, uint32_t depth) {
    switch (depth) {
        case 16: return ((uint32_t)row[i * 2] << 8) | row[i * 2 + 1];
        case 8:  return row[i];
        default: {
            const uint32_t bit = i * depth;
            const uint32_t shift = 8 - depth - (bit & 7);
            return (row[bit >> 3] >> shift) & ((1u << depth) - 1);
        }
    }
}

/* Scale a raw sample to 8 bits. */
static inline uint8_t to8(uint32_t v, uint32_t depth) {
    switch (depth) {
        case 1:  return (uint8_t)(v * 255);
        case 2:  return (uint8_t)(v * 85);
        case 4:  return (uint8_t)(v * 17);
        case 8:  return (uint8_t)v;
        default: return (uint8_t)(v >> 8);
    }
}

static void convertRow(const PngImage *img, const uint8_t *row, uint8_t *rgba) {
    const uint32_t w = img->info.width, d = img->info.bit_depth;
    switch (img->info.color_type) {
        case 0:
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t s = sampleAt(row, x, d);
                const uint8_t g = to8(s, d);
                rgba[x * 4 + 0] = rgba[x * 4 + 1] = rgba[x * 4 + 2] = g;
                rgba[x * 4 + 3] = (img->has_trns_key && s == img->trns_key[0]) ? 0 : 255;
            }
            break;
        case 2:
            for (uint32_t x = 0; x < w; x++) {
                const uint32_t r = sampleAt(row, x * 3, d), g = sampleAt(row, x * 3 + 1, d), b = sampleAt(row, x * 3 + 2, d);
                rgba[x * 4 + 0] = to8(r, d);
                rgba[x * 4 + 1] = to8(g, d);
                rgba[x * 4 + 2] = to8(b, d);
                rgba[x * 4 + 3] = (img->has_trns_key && r == img->trns_key[0] && g == img->trns_key[1] && b == img->trns_key[2]) ? 0 : 255;
            }
            break;
        case 3:
            for (uint32_t x = 0; x < w; x++) {
                uint32_t idx = sampleAt(row, x, d);
                if (idx >= img->palette_size)
                    idx = 0; /* out-of-range index: spec says error; be lenient */
                memcpy(&rgba[x * 4], img->palette[idx], 4);
            }
            break;
        case 4:
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t g = to8(sampleAt(row, x * 2, d), d);
                rgba[x * 4 + 0] = rgba[x * 4 + 1] = rgba[x * 4 + 2] = g;
                rgba[x * 4 + 3] = to8(sampleAt(row, x * 2 + 1, d), d);
            }
            break;
        case 6:
            for (uint32_t x = 0; x < w; x++)
                for (uint32_t c = 0; c < 4; c++)
                    rgba[x * 4 + c] = to8(sampleAt(row, x * 4 + c, d), d);
            break;
    }
}

PngResult pngDecode(PngImage *img, PngRowCallback cb, void *user) {
    if (img->decoded)
        return PNG_ERR_FORMAT;
    img->decoded = true;

    const uint32_t line = img->row_bytes + 1; /* filter byte + data */
    PngResult res = PNG_OK;

    tinfl_decompressor *inf = malloc(sizeof(tinfl_decompressor));
    uint8_t *dict = malloc(TINFL_LZ_DICT_SIZE);
    uint8_t *inbuf = malloc(IN_BUF_SIZE);
    uint8_t *cur = malloc(line);
    uint8_t *prev = calloc(1, line);
    uint8_t *rgba = malloc((size_t)img->info.width * 4);
    if (!inf || !dict || !inbuf || !cur || !prev || !rgba) {
        res = PNG_ERR_NO_MEMORY;
        goto done;
    }
    tinfl_init(inf);

    size_t in_pos = 0, in_avail = 0, dict_ofs = 0;
    uint32_t line_fill = 0, y = 0;
    bool io_error = false;

    while (y < img->info.height) {
        if (in_pos == in_avail && !img->idat_done) {
            in_avail = readIdat(img, inbuf, IN_BUF_SIZE, &io_error);
            in_pos = 0;
            if (io_error) { res = PNG_ERR_TRUNCATED; goto done; }
        }
        const bool more_input = !img->idat_done || in_pos < in_avail;

        size_t in_size = in_avail - in_pos;
        size_t out_size = TINFL_LZ_DICT_SIZE - dict_ofs;
        const mz_uint32 flags = TINFL_FLAG_PARSE_ZLIB_HEADER | (img->idat_done ? 0 : TINFL_FLAG_HAS_MORE_INPUT);
        const tinfl_status st = tinfl_decompress(inf, inbuf + in_pos, &in_size, dict, dict + dict_ofs, &out_size, flags);
        in_pos += in_size;

        /* Feed the produced bytes to the scanline assembler. */
        const uint8_t *src = dict + dict_ofs;
        size_t left = out_size;
        while (left > 0 && y < img->info.height) {
            size_t take = line - line_fill;
            if (take > left)
                take = left;
            memcpy(cur + line_fill, src, take);
            line_fill += take;
            src += take;
            left -= take;

            if (line_fill == line) {
                if (!unfilter(cur[0], cur + 1, prev + 1, img->row_bytes, img->filter_bpp)) {
                    res = PNG_ERR_FORMAT;
                    goto done;
                }
                convertRow(img, cur + 1, rgba);
                if (!cb(user, y, rgba)) {
                    res = PNG_ERR_ABORTED;
                    goto done;
                }
                uint8_t *t = prev; prev = cur; cur = t;
                line_fill = 0;
                y++;
            }
        }
        dict_ofs = (dict_ofs + out_size) & (TINFL_LZ_DICT_SIZE - 1);

        if (y >= img->info.height)
            break; /* all rows done; ignore trailing data and checksum */
        if (st == TINFL_STATUS_DONE) {
            res = PNG_ERR_TRUNCATED; /* stream ended before the last row */
            goto done;
        }
        if (st < 0) {
            res = PNG_ERR_INFLATE;
            goto done;
        }
        if (st == TINFL_STATUS_NEEDS_MORE_INPUT && !more_input) {
            res = PNG_ERR_TRUNCATED;
            goto done;
        }
    }

done:
    free(rgba);
    free(prev);
    free(cur);
    free(inbuf);
    free(dict);
    free(inf);
    return res;
}
