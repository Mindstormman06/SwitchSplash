#pragma once
/*
 * Streaming WAV reader producing 48 kHz stereo s16 (audout's fixed format), DESIGN.md M4.
 *
 * Input: RIFF/WAVE, PCM 16-bit (format 1, or WAVE_FORMAT_EXTENSIBLE with PCM subformat),
 * mono or stereo, any sample rate 8-192 kHz. Non-48 kHz input is converted with linear
 * interpolation (fine for a jingle; 48 kHz input is passed through exactly).
 * Reads the file in small chunks; memory use is a few KiB.
 *
 * Platform-independent (plain C + stdio) so it can be tested on a PC.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define WAV_OUT_RATE 48000

typedef enum {
    WAV_OK = 0,
    WAV_ERR_OPEN = -1,
    WAV_ERR_NOT_WAV = -2,
    WAV_ERR_FORMAT = -3,       /* malformed chunks / no fmt or data */
    WAV_ERR_UNSUPPORTED = -4,  /* not 16-bit PCM, >2 channels, odd rate */
} WavResult;

typedef struct {
    FILE *f;
    uint32_t rate, channels;
    uint32_t data_left;        /* bytes of sample data not yet read from the file */
    uint32_t total_frames;     /* input frames */

    /* chunked file reader */
    uint8_t rbuf[4096];
    uint32_t rpos, rlen;

    /* resampler state (positions in 32.32 fixed point) */
    int16_t cur[2], next[2];
    bool has_next, started, done;
    uint64_t frac, step;
} WavStream;

WavResult wavOpen(WavStream *w, const char *path);
void wavClose(WavStream *w);

/* Duration of the file in milliseconds. */
uint32_t wavLengthMs(const WavStream *w);

/* Produces up to `frames` stereo frames (2 x int16 each) at 48 kHz. Returns the number
 * produced; fewer than requested means the end of the file was reached. */
uint32_t wavRead(WavStream *w, int16_t *out, uint32_t frames);

const char *wavResultString(WavResult r);
