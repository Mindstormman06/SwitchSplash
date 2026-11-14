#pragma once
#include <switch.h>

/*
 * Jingle playback (DESIGN.md M4, §6).
 *
 * Streams a WAV from SD through our own audout session on a dedicated thread, as sys-tune
 * does. We never touch audren (what games use), so the game's audio init is unaffected.
 * The audout session is opened when playback starts and closed as soon as it ends.
 */

/* Opens the WAV and starts playback. Returns false (and logs why) if the file is
 * missing/invalid or audout can't be started. `volume` is 0.0-1.0. */
bool audioStart(const char *path, float volume);

/* Starts a linear fade to silence over `ms`, after which playback stops. Takes effect on
 * the next buffer (up to ~170 ms of already-queued audio plays first). No-op if idle. */
void audioFadeOut(u32 ms);

/* True while the playback thread is running (including the fade). */
bool audioIsPlaying(void);

/* Waits for the playback thread to finish and releases everything. Safe if idle. */
void audioJoin(void);
