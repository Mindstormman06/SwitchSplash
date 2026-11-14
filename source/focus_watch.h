#pragma once
#include <switch.h>

/*
 * Detects HOME / power presses so a splash never covers the HOME menu.
 *
 * The hid:sys button events are acquired ONCE for the whole process lifetime on a
 * background thread (Ultrahand's pattern). Hardware results, 2026-11-13:
 *  - Acquiring them fresh for each splash never fired.
 *  - Held for the process lifetime they fire 10-30 ms after a press, also during a game's
 *    boot screen. Each press signals twice (~150-250 ms apart, presumably press and release).
 *  - pdm play-log focus events were also tried: no out_of_focus arrived before the hid
 *    event, and opening pdm:qry delayed the first frame by ~30 ms. Removed.
 */

/* Starts the button-event thread. Call once at startup. */
void focusWatchStart(void);

/* armGetSystemTick() of the most recent HOME / power press, 0 if none yet. */
u64 focusLastHomeTick(void);
u64 focusLastPowerTick(void);
