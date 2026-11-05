#include "theme.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "font.h"
#include "util.h"

struct theme theme = {.scale = 1};
static char wallpaper_revisions[2][24];

bool theme_apply(const cJSON *screen)
{
    struct theme t;
    memset(&t, 0, sizeof t);
    const char *size = jstr(screen, "ui_size");
    t.scale = !size ? 1 : !strcmp(size, "small") ? 0.85f : !strcmp(size, "large") ? 1.2f : !strcmp(size, "extra") ? 1.4f : 1;
    const char *mode = jstr(screen, "ui_theme");
    t.light = mode && !strcmp(mode, "light");
    t.motion = !jfalse(screen, "ui_motion");
    const char *density = jstr(screen, "ui_density");
    t.compact = density && !strcmp(density, "compact");
    t.status_bar = !jfalse(screen, "status_bar");
    t.nav_bar = !jfalse(screen, "nav_bar");
    const char *desktop = jstr(screen, "desktop_wallpaper"), *lock = jstr(screen, "lock_wallpaper");
    t.desktop_wallpaper = desktop && !strcmp(desktop, "custom");
    t.lock_wallpaper = lock && !strcmp(lock, "custom");
    t.surface_opa = t.desktop_wallpaper ? LV_OPA_70 : LV_OPA_COVER;

    const char *accent = jstr(screen, "ui_accent");
    int a = accent && !strcmp(accent, "blue") ? 1 : accent && !strcmp(accent, "violet") ? 2 : 0;
    if (t.light) {
        static const uint32_t accents[] = {0x086c57, 0x195ca0, 0x7041a5};
        static const uint32_t actives[] = {0xd2f1e5, 0xd9eafd, 0xede0fc};
        t.bg = lv_color_hex(0xedf2f7);
        t.chrome = lv_color_hex(0xf4f8fc);
        t.surface = lv_color_hex(0xffffff);
        t.button = lv_color_hex(0xdde8f2);
        t.active = lv_color_hex(actives[a]);
        t.danger = lv_color_hex(0xffdcde);
        t.nav = lv_color_hex(0xf4f8fc);
        t.text = lv_color_hex(0x17283b);
        t.dim = lv_color_hex(0x50647a);
        t.line = lv_color_hex(0xc6d2df);
        t.accent = lv_color_hex(accents[a]);
        t.good = lv_color_hex(0x1f8a52);
        t.fair = lv_color_hex(0x855900);
        t.poor = lv_color_hex(0xad292f);
        t.hero_start = lv_color_hex(0xdff8ef);
        t.hero_end = lv_color_hex(0xf3f8ff);
    } else {
        static const uint32_t accents[] = {0x85ebcd, 0x89c6ff, 0xc3adff};
        t.bg = lv_color_hex(0x0c121b);
        t.chrome = lv_color_hex(0x0c121b);
        t.surface = lv_color_hex(0x18212e);
        t.button = lv_color_hex(0x233246);
        t.active = lv_color_hex(0x1c3937);
        t.danger = lv_color_hex(0x5a2420);
        t.nav = lv_color_hex(0x111b28);
        t.text = lv_color_hex(0xf1f5fa);
        t.dim = lv_color_hex(0x96a5b9);
        t.line = lv_color_hex(0x283444);
        t.accent = lv_color_hex(accents[a]);
        t.good = lv_color_hex(0x3ccf7a);
        t.fair = lv_color_hex(0xf2c14b);
        t.poor = lv_color_hex(0xf0625a);
        t.hero_start = lv_color_hex(0x213b42);
        t.hero_end = lv_color_hex(0x192738);
    }

    bool changed = memcmp(&t, &theme, sizeof t) != 0;
    const char *revisions[2] = {jstr(screen, "desktop_wallpaper_rev"), jstr(screen, "lock_wallpaper_rev")};
    for (int i = 0; i < 2; i++) {
        const char *rev = revisions[i] ? revisions[i] : "";
        if (strncmp(rev, wallpaper_revisions[i], sizeof wallpaper_revisions[i] - 1)) {
            snprintf(wallpaper_revisions[i], sizeof wallpaper_revisions[i], "%s", rev);
            changed = true;
        }
    }
    memcpy(&theme, &t, sizeof t);
    return changed;
}

float theme_cap(float max)
{
    float scale = theme.scale;
    if (theme.scale > max)
        theme.scale = max;
    return scale;
}

int S(int px)
{
    return (int)lroundf((float)px * theme.scale);
}

const lv_font_t *F(int px)
{
    return font_get(S(px), false);
}

const lv_font_t *FB(int px)
{
    return font_get(S(px), true);
}

lv_color_t grade_color(const char *word)
{
    if (!strcmp(word, "excellent") || !strcmp(word, "good"))
        return theme.accent;
    if (!strcmp(word, "fair"))
        return theme.fair;
    return theme.poor;
}
