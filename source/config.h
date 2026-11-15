#pragma once
#include <switch.h>

#define BOOTSPLASH_CONFIG_PATH "sdmc:/config/bootsplash/config.ini"

typedef enum {
    FadeMode_Auto = 0, /* vi layer alpha if available, else pixels */
    FadeMode_Pixels,   /* force the pixel fade (for testing) */
    FadeMode_None,     /* no visual fade: the image disappears at once */
} FadeMode;

typedef struct {
    bool enabled;          /* enabled=1 */
    u32  duration_ms;      /* duration_ms=6000, measured from launch detection; 0 = jingle length */
    bool audio;            /* audio=1: play splash.wav next to the image */
    u32  volume;           /* volume=100 (0-100) */
    u32  audio_fade_ms;    /* audio_fade_ms=500: jingle fade when the splash time is up */
    u32  fade_ms;          /* fade_ms=500: image fade-out when the splash time is up (0 = none) */
    FadeMode fade_mode;    /* fade_mode=auto | pixels | none */
    bool skip_hbl;         /* skip_hbl=0: hbmenu title takeover uses hbl/ (if present); 1 = never */
    u32  hbl_duration_ms;  /* hbl_duration_ms=2500: duration_ms for hbmenu launches (0 = jingle length) */
    u32  background;       /* background=000000 (RRGGBB): letterbox / transparent areas */
    bool home_cancel;      /* home_cancel=1: HOME/power press ends the splash */
    bool last_frame_stack; /* last_frame_stack=0: include the layer in AM's resume snapshot */

    /* Test settings. */
    u32  buffers;          /* buffers=1 (1 or 2) */
    bool test_pattern;     /* test_pattern=0: draw the M2 alpha test screen for every title */
    bool test_redraw;      /* test_redraw=0: re-present one frame after the hold (may block with 1 buffer!) */
    bool test_anim;        /* test_anim=0: animated test pattern instead of the image (GIF groundwork) */
    bool anim_inplace;     /* anim_mode=double|inplace */
    u32  anim_width;       /* anim_width=640|1280 (16:9) */
} Config;

/* Reads config.ini. Missing file/keys -> defaults. Called on every launch so edits
 * apply without a reboot. */
void configLoad(Config *out);

/* Overlays another config.ini on top (only the keys it sets change). Used for the
 * per-folder configs: default/, <TITLE ID>/ and hbl/. Returns false if the file is absent. */
bool configApply(Config *c, const char *path);
