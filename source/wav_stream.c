#include "wav_stream.h"

#include <string.h>

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

const char *wavResultString(WavResult r) {
    switch (r) {
        case WAV_OK:              return "ok";
        case WAV_ERR_OPEN:        return "cannot open file";
        case WAV_ERR_NOT_WAV:     return "not a WAV file";
        case WAV_ERR_FORMAT:      return "malformed WAV";
        case WAV_ERR_UNSUPPORTED: return "unsupported WAV (need 16-bit PCM, mono or stereo)";
    }
    return "unknown error";
}

WavResult wavOpen(WavStream *w, const char *path) {
    memset(w, 0, sizeof(*w));
    w->f = fopen(path, "rb");
    if (!w->f)
        return WAV_ERR_OPEN;

    WavResult res = WAV_OK;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, w->f) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        res = WAV_ERR_NOT_WAV;
        goto fail;
    }

    bool have_fmt = false;
    uint16_t bits = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, w->f) != 8) { res = WAV_ERR_FORMAT; goto fail; }
        const uint32_t len = le32(ch + 4);

        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[40];
            if (len < 16 || len > sizeof(fmt) || fread(fmt, 1, len, w->f) != len) { res = WAV_ERR_FORMAT; goto fail; }
            uint16_t tag = le16(fmt);
            w->channels = le16(fmt + 2);
            w->rate = le32(fmt + 4);
            bits = le16(fmt + 14);
            if (tag == 0xFFFE && len >= 26)
                tag = le16(fmt + 24); /* WAVE_FORMAT_EXTENSIBLE: first 2 bytes of the subformat GUID */
            if (tag != 1 || bits != 16 || w->channels < 1 || w->channels > 2 ||
                w->rate < 8000 || w->rate > 192000) {
                res = WAV_ERR_UNSUPPORTED;
                goto fail;
            }
            have_fmt = true;
            if (len & 1) fseek(w->f, 1, SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt) { res = WAV_ERR_FORMAT; goto fail; }
            w->data_left = len; /* clamped by EOF while reading if the header lies */
            break;
        } else {
            if (fseek(w->f, (long)(len + (len & 1)), SEEK_CUR) != 0) { res = WAV_ERR_FORMAT; goto fail; }
        }
    }

    const uint32_t frame_bytes = w->channels * 2;
    w->data_left -= w->data_left % frame_bytes;
    w->total_frames = w->data_left / frame_bytes;
    w->step = ((uint64_t)w->rate << 32) / WAV_OUT_RATE;
    return WAV_OK;

fail:
    fclose(w->f);
    w->f = NULL;
    return res;
}

void wavClose(WavStream *w) {
    if (w->f)
        fclose(w->f);
    w->f = NULL;
}

uint32_t wavLengthMs(const WavStream *w) {
    return w->rate ? (uint32_t)((uint64_t)w->total_frames * 1000 / w->rate) : 0;
}

/* Reads one input frame as stereo. Returns false at end of data. */
static bool readFrame(WavStream *w, int16_t out[2]) {
    const uint32_t fb = w->channels * 2;
    if (w->rlen - w->rpos < fb) {
        /* Refill, keeping any partial frame (can't happen with whole-frame data, but cheap). */
        const uint32_t keep = w->rlen - w->rpos;
        memmove(w->rbuf, w->rbuf + w->rpos, keep);
        uint32_t want = (uint32_t)sizeof(w->rbuf) - keep;
        want -= want % fb;
        if (want > w->data_left)
            want = w->data_left;
        const size_t got = want ? fread(w->rbuf + keep, 1, want, w->f) : 0;
        w->data_left -= (uint32_t)got;
        if (got < want)
            w->data_left = 0; /* file shorter than the data chunk claims */
        w->rpos = 0;
        w->rlen = keep + (uint32_t)got;
        if (w->rlen < fb)
            return false;
    }
    const uint8_t *p = w->rbuf + w->rpos;
    out[0] = (int16_t)le16(p);
    out[1] = w->channels == 2 ? (int16_t)le16(p + 2) : out[0];
    w->rpos += fb;
    return true;
}

uint32_t wavRead(WavStream *w, int16_t *out, uint32_t frames) {
    if (!w->started) {
        w->started = true;
        if (!readFrame(w, w->cur)) {
            w->done = true;
        } else {
            w->has_next = readFrame(w, w->next);
            if (!w->has_next)
                memcpy(w->next, w->cur, sizeof(w->cur));
        }
    }

    uint32_t n = 0;
    while (n < frames && !w->done) {
        /* Linear interpolation between cur and next at position frac. */
        const int64_t f = (int64_t)(w->frac >> 16); /* 16-bit fraction */
        for (int c = 0; c < 2; c++) {
            const int32_t a = w->cur[c], b = w->next[c];
            out[n * 2 + c] = (int16_t)(a + (((int64_t)(b - a) * f) >> 16));
        }
        n++;

        w->frac += w->step;
        while (w->frac >= (1ULL << 32)) {
            w->frac -= (1ULL << 32);
            if (!w->has_next) {
                w->done = true;
                break;
            }
            memcpy(w->cur, w->next, sizeof(w->cur));
            w->has_next = readFrame(w, w->next);
            if (!w->has_next)
                memcpy(w->next, w->cur, sizeof(w->cur));
        }
    }
    return n;
}
