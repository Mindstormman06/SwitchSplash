#include "audio_player.h"
#include "log.h"
#include "wav_stream.h"

#include <malloc.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* 4 x 8 KiB = 4 x 2048 frames = ~170 ms queued. Enough headroom for SD stalls while
 * the game is loading, small enough that a fade request is heard quickly. */
#define NUM_BUFFERS   4
#define BUFFER_BYTES  0x2000
#define BUFFER_FRAMES (BUFFER_BYTES / 4)

static Thread g_thread;
static bool g_thread_created = false;
static atomic_bool g_playing = false;
static atomic_uint g_fade_request_ms = 0; /* 0 = none */

static WavStream g_wav;
static float g_volume = 1.0f;
static AudioOutBuffer g_bufs[NUM_BUFFERS];
static void *g_mem[NUM_BUFFERS];

alignas(0x1000) static u8 g_stack[0x4000];

/* Fade state, owned by the audio thread. */
static u32 g_fade_total, g_fade_left;
static bool g_fading, g_finished;

/* Fills one buffer. Returns the number of frames written. */
static u32 fillBuffer(AudioOutBuffer *b) {
    if (g_finished)
        return 0;

    const u32 req = atomic_exchange(&g_fade_request_ms, 0);
    if (req && !g_fading) {
        g_fading = true;
        g_fade_total = g_fade_left = req * (WAV_OUT_RATE / 1000);
    }

    s16 *out = b->buffer;
    u32 n = wavRead(&g_wav, out, BUFFER_FRAMES);
    if (n < BUFFER_FRAMES)
        g_finished = true;

    for (u32 i = 0; i < n; i++) {
        float g = g_volume;
        if (g_fading) {
            if (g_fade_left == 0) {
                n = i; /* fade complete: cut here */
                g_finished = true;
                break;
            }
            g *= (float)g_fade_left / (float)g_fade_total;
            g_fade_left--;
        }
        if (g != 1.0f) {
            out[i * 2]     = (s16)((float)out[i * 2] * g);
            out[i * 2 + 1] = (s16)((float)out[i * 2 + 1] * g);
        }
    }

    b->data_size = (u64)n * 4;
    b->data_offset = 0;
    return n;
}

static void audioThread(void *arg) {
    const u64 t0 = armGetSystemTick();
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        logWrite("AUDIO  audoutInitialize failed: 0x%x", rc);
        goto done;
    }
    rc = audoutStartAudioOut();
    if (R_FAILED(rc)) {
        logWrite("AUDIO  audoutStartAudioOut failed: 0x%x", rc);
        audoutExit();
        goto done;
    }
    if (audoutGetSampleRate() != WAV_OUT_RATE || audoutGetChannelCount() != 2 || audoutGetPcmFormat() != PcmFormat_Int16)
        logWrite("AUDIO  unexpected device format: %u Hz, %u ch, fmt %d", audoutGetSampleRate(),
                 audoutGetChannelCount(), audoutGetPcmFormat());

    u32 queued = 0;
    for (int i = 0; i < NUM_BUFFERS; i++) {
        if (fillBuffer(&g_bufs[i]) == 0)
            break;
        if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&g_bufs[i])))
            queued++;
    }
    logWrite("AUDIO  playing (%u buffers queued, start took %lums)", queued,
             (unsigned long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ULL));

    while (queued > 0) {
        /* libnx hands back one released buffer per call, but the event is signalled once
         * even if several finished, so after waking (or timing out) drain them all. */
        AudioOutBuffer *released = NULL;
        u32 count = 0;
        rc = audoutWaitPlayFinish(&released, &count, 50000000ULL);
        for (;;) {
            if (R_SUCCEEDED(rc) && released && count > 0) {
                queued--;
                if (fillBuffer(released) > 0 && R_SUCCEEDED(audoutAppendAudioOutBuffer(released)))
                    queued++;
            }
            released = NULL;
            count = 0;
            rc = audoutGetReleasedAudioOutBuffer(&released, &count);
            if (R_FAILED(rc) || !released || count == 0)
                break;
        }
    }

    audoutStopAudioOut();
    audoutExit();
    logWrite("AUDIO  finished after %lums%s", (unsigned long)(armTicksToNs(armGetSystemTick() - t0) / 1000000ULL),
             g_fading ? " (faded out)" : "");

done:
    atomic_store(&g_playing, false);
}

bool audioStart(const char *path, float volume) {
    audioJoin();

    const WavResult wr = wavOpen(&g_wav, path);
    if (wr != WAV_OK) {
        if (wr != WAV_ERR_OPEN) /* a missing file just means "no jingle" */
            logWrite("AUDIO  cannot use %s: %s (%d)", path, wavResultString(wr), wr);
        return false;
    }
    logWrite("AUDIO  %s: %u Hz, %u ch, %ums%s", path, g_wav.rate, g_wav.channels, wavLengthMs(&g_wav),
             g_wav.rate != WAV_OUT_RATE ? " (resampled to 48 kHz)" : "");

    for (int i = 0; i < NUM_BUFFERS; i++) {
        g_mem[i] = memalign(0x1000, BUFFER_BYTES);
        if (!g_mem[i]) {
            logWrite("AUDIO  out of memory");
            audioJoin(); /* frees what was allocated */
            wavClose(&g_wav);
            return false;
        }
        memset(&g_bufs[i], 0, sizeof(g_bufs[i]));
        g_bufs[i].buffer = g_mem[i];
        g_bufs[i].buffer_size = BUFFER_BYTES;
    }

    g_volume = volume < 0.0f ? 0.0f : volume > 1.0f ? 1.0f : volume;
    g_fading = g_finished = false;
    atomic_store(&g_fade_request_ms, 0);
    atomic_store(&g_playing, true);

    /* Above the splash thread (0x2C): an audio underrun is audible, a late frame isn't. */
    Result rc = threadCreate(&g_thread, audioThread, NULL, g_stack, sizeof(g_stack), 0x2A, -2);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&g_thread);
        if (R_FAILED(rc))
            threadClose(&g_thread);
    }
    if (R_FAILED(rc)) {
        logWrite("AUDIO  thread start failed: 0x%x", rc);
        atomic_store(&g_playing, false);
        audioJoin();
        return false;
    }
    g_thread_created = true;
    return true;
}

void audioFadeOut(u32 ms) {
    if (atomic_load(&g_playing))
        atomic_store(&g_fade_request_ms, ms ? ms : 1);
}

bool audioIsPlaying(void) {
    return atomic_load(&g_playing);
}

void audioJoin(void) {
    if (g_thread_created) {
        threadWaitForExit(&g_thread);
        threadClose(&g_thread);
        g_thread_created = false;
    }
    for (int i = 0; i < NUM_BUFFERS; i++) {
        free(g_mem[i]);
        g_mem[i] = NULL;
    }
    wavClose(&g_wav);
}
