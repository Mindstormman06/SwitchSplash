/*
 * PC test harness for the streaming WAV reader.
 *
 *   test_wav <in.wav> <out_48k_stereo_s16.raw> [chunk_frames]
 *
 * Prints "rate channels total_frames length_ms out_frames result".
 */
#include <stdio.h>
#include <stdlib.h>

#include "wav_stream.h"

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.wav out.raw [chunk]\n", argv[0]);
        return 2;
    }
    const uint32_t chunk = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 777;

    WavStream w;
    WavResult r = wavOpen(&w, argv[1]);
    if (r != WAV_OK) {
        printf("0 0 0 0 0 %d %s\n", r, wavResultString(r));
        return 1;
    }

    FILE *out = fopen(argv[2], "wb");
    int16_t *buf = malloc((size_t)chunk * 4);
    uint64_t total = 0;
    for (;;) {
        const uint32_t n = wavRead(&w, buf, chunk);
        fwrite(buf, 4, n, out);
        total += n;
        if (n < chunk)
            break;
    }
    /* Reading after the end must keep returning 0. */
    if (wavRead(&w, buf, chunk) != 0)
        fprintf(stderr, "read after end returned data\n");

    printf("%u %u %u %u %llu 0 ok\n", w.rate, w.channels, w.total_frames, wavLengthMs(&w), (unsigned long long)total);
    fclose(out);
    free(buf);
    wavClose(&w);
    return 0;
}
