#pragma once
#include <switch.h>

#define BOOTSPLASH_CONFIG_PATH "sdmc:/config/bootsplash/config.ini"

typedef struct {
    bool enabled;          /* enabled=1 */
    u32  duration_ms;      /* duration_ms=6000, measured from launch detection */
    bool skip_hbl;         /* skip_hbl=1: no splash for hbmenu title takeover */
    u32  background;       /* background=000000 (RRGGBB): letterbox / transparent areas */
    bool home_cancel;      /* home_cancel=1: HOME/power press ends the splash */
    bool last_frame_stack; /* last_frame_stack=0: include the layer in AM's resume snapshot */

    /* Test settings. */
    u32  buffers;          /* buffers=1 (1 or 2) */
    bool test_pattern;     /* test_pattern=0: draw the M2 alpha test screen for every title */
    bool test_redraw;      /* test_redraw=0: re-present one frame after the hold (may block with 1 buffer!) */
} Config;

/* Reads config.ini. Missing file/keys -> defaults. Called on every launch so edits
 * apply without a reboot. */
void configLoad(Config *out);
