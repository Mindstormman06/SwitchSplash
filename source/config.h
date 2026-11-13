#pragma once
#include <switch.h>

#define BOOTSPLASH_CONFIG_PATH "sdmc:/config/bootsplash/config.ini"

typedef struct {
    bool enabled;       /* enabled=1 */
    u32  duration_ms;   /* duration_ms=3000 */
    bool skip_hbl;      /* skip_hbl=1: no splash for hbmenu title takeover */

    /* Milestone 2 test settings. */
    u32  buffers;       /* buffers=1 (1 or 2) */
    u32  color;         /* color=203060 (RRGGBB) */
    bool alpha_test;    /* alpha_test=1: draw the blending test stripes */
    bool test_redraw;   /* test_redraw=0: re-present one frame after the hold (may block with 1 buffer!) */

    /* Diagnostics for "splash invisible during load" (2026-11-13). */
    u32  layer_w;          /* layer_w=1920 (vi display space) */
    u32  layer_h;          /* layer_h=1080 */
    bool home_cancel;      /* home_cancel=1: HOME/power press ends the splash */
    bool last_frame_stack; /* last_frame_stack=1: add the layer to ViLayerStack_LastFrame */
} Config;

/* Reads config.ini. Missing file/keys -> defaults. Called on every launch so edits
 * apply without a reboot. */
void configLoad(Config *out);
