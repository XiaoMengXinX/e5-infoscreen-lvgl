#include "icons.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct icon {
    const char *name;
    const char *path;
    lv_point_precise_t *points;
    uint16_t *runs;
    int run_count;
    struct mask *masks;
};

struct mask {
    int size;
    lv_draw_buf_t *buf;
    struct mask *next;
};

static struct icon icons[] = {
    {.name = "home", .path = "m3 10 9-7 9 7M5 9v12h5v-7h4v7h5V9"},
    {.name = "traffic", .path = "M6 3v17m-4-4 4 4 4-4M18 21V4m-4 4 4-4 4 4"},
    {.name = "wifi", .path = "M2 8a16 16 0 0 1 20 0M5 12a11 11 0 0 1 14 0m-11 4a6 6 0 0 1 8 0m-4 4h.01"},
    {.name = "apps", .path = "M3 3h7v7H3zM14 3h7v7h-7zM3 14h7v7H3zM14 14h7v7h-7z"},
    {.name = "settings", .path = "M4 6h16M4 18h16M8 3v6m8 6v6M4 12h16m-4-3v6"},
    {.name = "back", .path = "m14 5-7 7 7 7"},
    {.name = "menu", .path = "M4 6h16M4 12h16M4 18h16"},
    {.name = "lock", .path = "M7 10V7a5 5 0 0 1 10 0v3M5 10h14v11H5zM12 14v3"},
    {.name = "signal", .path = "M4 20v-4m5 4v-8m5 8V8m5 12V3"},
    {.name = "sms", .path = "M3 4h18v13H8l-5 4zM7 8h10m-10 5h7"},
    {.name = "device", .path = "M6 2h12v20H6zM10 18h4"},
    {.name = "info", .path = "M12 11v7m0-12v1M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20"},
    {.name = "up", .path = "m6 14 6-6 6 6"},
    {.name = "close", .path = "m6 6 12 12M18 6 6 18"},
    {.name = "battery", .path = "M3 7h16v10H3zM21 10v4M11 8l-3 4h5l-3 4"},
    {.name = "bell", .path = "M5 17h14l-2-3V9a5 5 0 0 0-10 0v5zM10 21h4"},
    {.name = "volume", .path = "M3 10h4l5-5v14l-5-5H3zM16 8a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14"},
    {.name = "sun", .path = "M16 12a4 4 0 1 1-8 0 4 4 0 0 1 8 0M12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5"},
    {.name = "moon", .path = "M20 14A9 9 0 0 1 10 3a9 9 0 1 0 10 11z"},
    {.name = "palette", .path = "M12 3a9 9 0 1 0 0 18h1a2 2 0 0 0 1-3.7 1.5 1.5 0 0 1 1-2.8h2a4 4 0 0 0 4-4C21 6 17 3 12 3M7 10h.01M10 6.5h.01M15 7h.01M18 10h.01"},
    {.name = "clock", .path = "M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0M12 7v5l3 2"},
    {.name = "globe", .path = "M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0M3 12h18M12 3c5 5 5 13 0 18-5-5-5-13 0-18"},
    {.name = "type", .path = "M3 6h12M9 6v14m-3 0h6M16 13h6m-3 0v7"},
    {.name = "layout", .path = "M3 4h18v16H3zM3 9h18M3 16h18"},
    {.name = "motion", .path = "M9 5l7 7-7 7M3 8h3m-4 4h5m-4 4h3"},
    {.name = "touch", .path = "M9 13V5a2 2 0 0 1 4 0v7l2-2 5 3-1 7H9l-5-7a2 2 0 0 1 3-2z"},
    {.name = "terminal", .path = "M3 4h18v16H3zM6 8l4 4-4 4m7 0h4"},
    {.name = "usb", .path = "M12 20V3m-3 3 3-3 3 3M12 16l-6-4V9m6 4 6-3V7M4 6h4v3H4zM16 4h4v3h-4z"},
    {.name = "bluetooth", .path = "M7 7l10 10-5 4V3l5 4L7 17"},
    {.name = "chip", .path = "M6 6h12v12H6zM9 9h6v6H9zM9 3v3m6-3v3M9 18v3m6-3v3M3 9h3m-3 6h3m12-6h3m-3 6h3"},
    {.name = "refresh", .path = "M3 11a9 9 0 0 1 15.36-6.36L21 7M21 3v4h-4M21 13a9 9 0 0 1-15.36 6.36L3 17M3 21v-4h4"},
    {.name = "navigation", .path = "M5 3h14a1 1 0 0 1 1 1v16a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1M4 15h16M8 18h.01M12 18h.01M16 18h.01"},
    {.name = "download", .path = "M12 3v12m-5-5 5 5 5-5M4 16v5h16v-5"},
    {.name = "power", .path = "M12 2v10M7 5a9 9 0 1 0 10 0"},
    {.name = "restart", .path = "M19.5 8A8 8 0 1 0 20 15M19.5 3.5V8H15"},
    {.name = "android", .path = "M4 16a8 8 0 0 1 16 0v2H4v-2ZM7 9 5 5m12 4 2-4M8 14h.01M16 14h.01"},
    {.name = "check", .path = "m5 12 4 4L19 6"},
    {.name = "trash", .path = "M4 6h16M9 6V3h6v3M6 6l1 15h10l1-15M10 10v7m4-7v7"},
    {.name = "simcard", .path = "M6 3h9l4 4v14H6zM9 12h7v6H9zM12.5 12v6M9 15h7"},
    {.name = "shield", .path = "m12 3 8 3v6c0 5-8 9-8 9s-8-4-8-9V6zM9 12l2 2 4-5"},
    {.name = "heart", .path = "M12 20 4 12a5 5 0 0 1 8-6 5 5 0 0 1 8 6z"},
    {.name = "user", .path = "M16 7a4 4 0 1 1-8 0 4 4 0 0 1 8 0M4 21v-2a8 8 0 0 1 16 0v2"},
    {.name = "search", .path = "M17 10a7 7 0 1 1-14 0 7 7 0 0 1 14 0m-2 5 6 6"},
    {.name = "phone", .path = "M5 4h4l2 5-2.5 1.5a11 11 0 0 0 5 5L15 13l5 2v4a2 2 0 0 1-2 2A16 16 0 0 1 3 6a2 2 0 0 1 2-2"},
    {.name = "hangup", .path = "M3 15.5a14 14 0 0 1 18 0l-1.5 3-4-1.5v-3a10 10 0 0 0-7 0v3l-4 1.5z"},
    {.name = "backspace", .path = "M9 5h11v14H9l-6-7zM12 9l5 6m0-6-5 6"},
    {.name = "plus", .path = "M12 5v14M5 12h14"},
    {.name = "minus", .path = "M5 12h14"},
    {.name = "mic-off", .path = "M9 9v3a3 3 0 0 0 5 2M15 10V5a3 3 0 0 0-6 0M19 11a7 7 0 0 1-1 3.5M5 11a7 7 0 0 0 11 5.7M12 18v3M4 4l16 16"},
    {.name = "keypad", .path = "M6 5h.01M12 5h.01M18 5h.01M6 10h.01M12 10h.01M18 10h.01M6 15h.01M12 15h.01M18 15h.01M12 20h.01"},
};

struct path_builder {
    lv_point_precise_t *points;
    uint16_t *runs;
    int count, capacity, run_count, run_capacity, run_start;
};

static void add_point(struct path_builder *b, double x, double y)
{
    if (b->count == b->capacity) {
        b->capacity = b->capacity ? b->capacity * 2 : 64;
        b->points = realloc(b->points, sizeof *b->points * (size_t)b->capacity);
    }
    b->points[b->count++] = (lv_point_precise_t){(lv_value_precise_t)x, (lv_value_precise_t)y};
}

static void end_run(struct path_builder *b)
{
    int length = b->count - b->run_start;
    if (length > 0) {
        if (b->run_count == b->run_capacity) {
            b->run_capacity = b->run_capacity ? b->run_capacity * 2 : 16;
            b->runs = realloc(b->runs, sizeof *b->runs * (size_t)b->run_capacity);
        }
        b->runs[b->run_count++] = (uint16_t)length;
    }
    b->run_start = b->count;
}

/* SVG elliptical arc, endpoint to centre parameterisation (SVG 1.1 F.6.5). */
static void add_arc(struct path_builder *b, double x1, double y1, double rx, double ry, double phi_deg, int large,
                    int sweep, double x2, double y2)
{
    if (rx == 0 || ry == 0) {
        add_point(b, x2, y2);
        return;
    }
    rx = fabs(rx);
    ry = fabs(ry);
    double phi = phi_deg * M_PI / 180, c = cos(phi), s = sin(phi);
    double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    double xp = c * dx + s * dy, yp = -s * dx + c * dy;
    double lambda = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lambda > 1) {
        rx *= sqrt(lambda);
        ry *= sqrt(lambda);
    }
    double num = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
    double den = rx * rx * yp * yp + ry * ry * xp * xp;
    double k = sqrt(fmax(0, num / den)) * (large == sweep ? -1 : 1);
    double cxp = k * rx * yp / ry, cyp = -k * ry * xp / rx;
    double cx = c * cxp - s * cyp + (x1 + x2) / 2, cy = s * cxp + c * cyp + (y1 + y2) / 2;
    double start = atan2((yp - cyp) / ry, (xp - cxp) / rx);
    double end = atan2((-yp - cyp) / ry, (-xp - cxp) / rx);
    double delta = end - start;
    if (sweep && delta < 0)
        delta += 2 * M_PI;
    else if (!sweep && delta > 0)
        delta -= 2 * M_PI;
    int steps = (int)ceil(fabs(delta) / (M_PI / 16));
    for (int i = 1; i <= steps; i++) {
        double t = start + delta * i / steps;
        add_point(b, cx + rx * cos(t) * c - ry * sin(t) * s, cy + rx * cos(t) * s + ry * sin(t) * c);
    }
}

static void add_cubic(struct path_builder *b, double x0, double y0, double x1, double y1, double x2, double y2,
                      double x3, double y3)
{
    for (int i = 1; i <= 12; i++) {
        double t = i / 12.0, u = 1 - t;
        add_point(b, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                  u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3);
    }
}

static bool number(const char **p, double *out)
{
    while (**p == ' ' || **p == ',')
        (*p)++;
    char *end;
    *out = strtod(*p, &end);
    if (end == *p)
        return false;
    *p = end;
    return true;
}

static bool flag(const char **p, int *out)
{
    while (**p == ' ' || **p == ',')
        (*p)++;
    if (**p != '0' && **p != '1')
        return false;
    *out = *(*p)++ - '0';
    return true;
}

static void parse(struct icon *icon)
{
    struct path_builder b = {0};
    const char *p = icon->path;
    char cmd = 0;
    double x = 0, y = 0, sx = 0, sy = 0, cx2 = 0, cy2 = 0;
    bool cubic = false;
    for (;;) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p)
            break;
        if (isalpha((unsigned char)*p))
            cmd = *p++;
        bool rel = islower((unsigned char)cmd);
        double ox = rel ? x : 0, oy = rel ? y : 0, a, c, d, e, f, g;
        int large, sweep;
        bool was_cubic = cubic;
        cubic = false;
        switch (tolower((unsigned char)cmd)) {
        case 'm':
            if (!number(&p, &a) || !number(&p, &c))
                goto done;
            end_run(&b);
            x = sx = ox + a;
            y = sy = oy + c;
            add_point(&b, x, y);
            cmd = rel ? 'l' : 'L';
            break;
        case 'l':
            if (!number(&p, &a) || !number(&p, &c))
                goto done;
            x = ox + a;
            y = oy + c;
            add_point(&b, x, y);
            break;
        case 'h':
            if (!number(&p, &a))
                goto done;
            x = ox + a;
            add_point(&b, x, y);
            break;
        case 'v':
            if (!number(&p, &a))
                goto done;
            y = oy + a;
            add_point(&b, x, y);
            break;
        case 'a':
            if (!number(&p, &a) || !number(&p, &c) || !number(&p, &d) || !flag(&p, &large) || !flag(&p, &sweep) ||
                !number(&p, &e) || !number(&p, &f))
                goto done;
            add_arc(&b, x, y, a, c, d, large, sweep, ox + e, oy + f);
            x = ox + e;
            y = oy + f;
            break;
        case 'c':
            if (!number(&p, &a) || !number(&p, &c) || !number(&p, &d) || !number(&p, &e) || !number(&p, &f) ||
                !number(&p, &g))
                goto done;
            add_cubic(&b, x, y, ox + a, oy + c, ox + d, oy + e, ox + f, oy + g);
            cx2 = ox + d;
            cy2 = oy + e;
            x = ox + f;
            y = oy + g;
            cubic = true;
            break;
        case 's':
            if (!number(&p, &a) || !number(&p, &c) || !number(&p, &d) || !number(&p, &e))
                goto done;
            add_cubic(&b, x, y, was_cubic ? 2 * x - cx2 : x, was_cubic ? 2 * y - cy2 : y, ox + a, oy + c, ox + d,
                      oy + e);
            cx2 = ox + a;
            cy2 = oy + c;
            x = ox + d;
            y = oy + e;
            cubic = true;
            break;
        case 'z':
            add_point(&b, sx, sy);
            x = sx;
            y = sy;
            break;
        default:
            goto done;
        }
    }
done:
    end_run(&b);
    icon->points = b.points;
    icon->runs = b.runs;
    icon->run_count = b.run_count;
}

static struct icon *lookup(const char *name)
{
    for (size_t i = 0; i < sizeof icons / sizeof *icons; i++)
        if (!strcmp(icons[i].name, name)) {
            if (!icons[i].runs)
                parse(&icons[i]);
            return &icons[i];
        }
    return lookup("apps");
}

static float segment_distance(float px, float py, lv_point_precise_t a, lv_point_precise_t b)
{
    float dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((px - a.x) * dx + (py - a.y) * dy) / len2 : 0;
    t = fminf(1.0f, fmaxf(0.0f, t));
    return hypotf(px - (a.x + t * dx), py - (a.y + t * dy));
}

/* Strokes are rasterised as coverage from the distance to the path, so edges stay smooth at any size. */
static lv_draw_buf_t *rasterize(const struct icon *icon, int size)
{
    lv_draw_buf_t *buf = lv_draw_buf_create(size, size, LV_COLOR_FORMAT_A8, 0);
    if (!buf)
        return NULL;
    float scale = (float)size / 24.0f, half = 0.95f * scale;
    for (int y = 0; y < size; y++) {
        uint8_t *row = (uint8_t *)lv_draw_buf_goto_xy(buf, 0, y);
        for (int x = 0; x < size; x++) {
            float px = x + 0.5f, py = y + 0.5f, d = INFINITY;
            const lv_point_precise_t *pt = icon->points;
            for (int r = 0; r < icon->run_count; pt += icon->runs[r++]) {
                lv_point_precise_t a = {pt[0].x * scale, pt[0].y * scale};
                d = fminf(d, segment_distance(px, py, a, a));
                for (int i = 1; i < icon->runs[r]; i++) {
                    lv_point_precise_t b = {pt[i].x * scale, pt[i].y * scale};
                    d = fminf(d, segment_distance(px, py, a, b));
                    a = b;
                }
            }
            float coverage = fminf(1.0f, fmaxf(0.0f, half - d + 0.5f));
            row[x] = (uint8_t)(coverage * 255.0f + 0.5f);
        }
    }
    return buf;
}

static lv_draw_buf_t *mask_for(struct icon *icon, int size)
{
    for (struct mask *m = icon->masks; m; m = m->next)
        if (m->size == size)
            return m->buf;
    struct mask *m = malloc(sizeof *m);
    m->size = size;
    m->buf = rasterize(icon, size);
    m->next = icon->masks;
    icon->masks = m;
    return m->buf;
}

static void draw(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target_obj(e);
    struct icon *icon = lv_obj_get_user_data(obj);
    if (!icon)
        return;
    lv_area_t area;
    lv_obj_get_content_coords(obj, &area);
    int size = lv_area_get_width(&area);
    lv_draw_buf_t *mask = size > 0 ? mask_for(icon, size) : NULL;
    if (!mask)
        return;
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = mask;
    dsc.recolor = lv_obj_get_style_text_color(obj, LV_PART_MAIN);
    dsc.recolor_opa = LV_OPA_COVER;
    dsc.opa = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
    area.x2 = area.x1 + size - 1;
    area.y2 = area.y1 + size - 1;
    lv_draw_image(lv_event_get_layer(e), &dsc, &area);
}

lv_obj_t *icon_create(lv_obj_t *parent, const char *name, int size)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_clickable(obj, false);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_event_bubble(obj, true);
    lv_obj_set_user_data(obj, lookup(name));
    lv_obj_add_event_cb(obj, draw, LV_EVENT_DRAW_MAIN, NULL);
    return obj;
}

void icon_set(lv_obj_t *obj, const char *name)
{
    struct icon *icon = lookup(name);
    if (lv_obj_get_user_data(obj) == icon)
        return;
    lv_obj_set_user_data(obj, icon);
    lv_obj_invalidate(obj);
}
