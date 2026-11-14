#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void setDefaults(Config *c) {
    c->enabled          = true;
    c->duration_ms      = 6000;
    c->audio            = true;
    c->volume           = 100;
    c->audio_fade_ms    = 500;
    c->fade_ms          = 500;
    c->fade_mode        = FadeMode_Auto;
    c->skip_hbl         = true;
    c->background       = 0x000000;
    c->home_cancel      = true;
    c->last_frame_stack = false;
    c->buffers          = 1;
    c->test_pattern     = false;
    c->test_redraw      = false;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static bool parseBool(const char *v) {
    return v[0] == '1' || v[0] == 'y' || v[0] == 'Y' || v[0] == 't' || v[0] == 'T';
}

static u32 parseColor(const char *v) {
    if (v[0] == '#')
        v++;
    return strtoul(v, NULL, 16) & 0xFFFFFF;
}

void configLoad(Config *c) {
    setDefaults(c);

    FILE *f = fopen(BOOTSPLASH_CONFIG_PATH, "r");
    if (!f)
        return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == ';' || *s == '#' || *s == '[')
            continue;

        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);

        /* Trailing comments: ';' anywhere, or '#' after whitespace (so "#RRGGBB" works). */
        for (char *p = val; *p; p++) {
            if (*p == ';' || (*p == '#' && p > val && isspace((unsigned char)p[-1]))) {
                *p = '\0';
                break;
            }
        }
        val = trim(val);

        if      (!strcmp(key, "enabled"))          c->enabled          = parseBool(val);
        else if (!strcmp(key, "duration_ms"))      c->duration_ms      = strtoul(val, NULL, 10);
        else if (!strcmp(key, "skip_hbl"))         c->skip_hbl         = parseBool(val);
        else if (!strcmp(key, "audio"))            c->audio            = parseBool(val);
        else if (!strcmp(key, "volume"))           c->volume           = strtoul(val, NULL, 10);
        else if (!strcmp(key, "audio_fade_ms"))    c->audio_fade_ms    = strtoul(val, NULL, 10);
        else if (!strcmp(key, "fade_ms"))          c->fade_ms          = strtoul(val, NULL, 10);
        else if (!strcmp(key, "fade_mode"))
            c->fade_mode = !strcmp(val, "pixels") ? FadeMode_Pixels : !strcmp(val, "none") ? FadeMode_None : FadeMode_Auto;
        else if (!strcmp(key, "background"))       c->background       = parseColor(val);
        else if (!strcmp(key, "home_cancel"))      c->home_cancel      = parseBool(val);
        else if (!strcmp(key, "last_frame_stack")) c->last_frame_stack = parseBool(val);
        else if (!strcmp(key, "buffers"))          c->buffers          = strtoul(val, NULL, 10);
        else if (!strcmp(key, "test_pattern"))     c->test_pattern     = parseBool(val);
        else if (!strcmp(key, "test_redraw"))      c->test_redraw      = parseBool(val);
    }
    fclose(f);

    if (c->buffers < 1) c->buffers = 1;
    if (c->buffers > 2) c->buffers = 2;
    if (c->duration_ms > 30000) c->duration_ms = 30000;
    if (c->volume > 100) c->volume = 100;
    if (c->audio_fade_ms > 5000) c->audio_fade_ms = 5000;
    if (c->fade_ms > 5000) c->fade_ms = 5000;
}
