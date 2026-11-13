#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void setDefaults(Config *c) {
    c->enabled     = true;
    c->duration_ms = 6000;
    c->skip_hbl    = true;
    c->buffers     = 1;
    c->color       = 0x203060;
    c->alpha_test  = true;
    c->test_redraw = false;
    c->layer_w     = 1920;
    c->layer_h     = 1080;
    c->home_cancel = true;
    c->last_frame_stack = true;
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

        /* Strip trailing comments. */
        char *cmt = strpbrk(val, ";#");
        if (cmt && cmt != val) {
            *cmt = '\0';
            val = trim(val);
        }

        if      (!strcmp(key, "enabled"))     c->enabled     = parseBool(val);
        else if (!strcmp(key, "duration_ms")) c->duration_ms = strtoul(val, NULL, 10);
        else if (!strcmp(key, "skip_hbl"))    c->skip_hbl    = parseBool(val);
        else if (!strcmp(key, "buffers"))     c->buffers     = strtoul(val, NULL, 10);
        else if (!strcmp(key, "color"))       c->color       = strtoul(val[0] == '#' ? val + 1 : val, NULL, 16) & 0xFFFFFF;
        else if (!strcmp(key, "alpha_test"))  c->alpha_test  = parseBool(val);
        else if (!strcmp(key, "test_redraw")) c->test_redraw = parseBool(val);
        else if (!strcmp(key, "layer_w"))     c->layer_w     = strtoul(val, NULL, 10);
        else if (!strcmp(key, "layer_h"))     c->layer_h     = strtoul(val, NULL, 10);
        else if (!strcmp(key, "home_cancel")) c->home_cancel = parseBool(val);
        else if (!strcmp(key, "last_frame_stack")) c->last_frame_stack = parseBool(val);
    }
    fclose(f);

    if (c->buffers < 1) c->buffers = 1;
    if (c->buffers > 2) c->buffers = 2;
    if (c->duration_ms > 30000) c->duration_ms = 30000;
    if (c->layer_w < 64 || c->layer_w > 1920) c->layer_w = 1920;
    if (c->layer_h < 64 || c->layer_h > 1080) c->layer_h = 1080;
}
