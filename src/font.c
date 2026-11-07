#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SIZES_H

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct glyph {
    uint32_t letter;
    uint16_t adv_w, box_w, box_h;
    int16_t ofs_x, ofs_y;
    uint8_t *bitmap;
};

struct sized_font {
    lv_font_t font;
    FT_Face face;
    FT_Size size;
    int px;
    bool bold;
    struct glyph *glyphs;
    uint32_t capacity, count;
    struct sized_font *next;
};

/* LVGL draws text on several threads; FreeType's faces and the glyph caches
 * are shared, and a glyph is read only under the lock, as a cache can move. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static FT_Library library;
static FT_Face faces[2], fallback;
static struct sized_font *fonts;
static uint8_t coverage[256];

/* A .ttc holds several regional faces; Chinese text needs the SC one. */
static FT_Face open_face(const char *const *paths, const char *family)
{
    for (; *paths; paths++) {
        FT_Face face;
        if (FT_New_Face(library, *paths, -1, &face))
            continue;
        long count = face->num_faces;
        FT_Done_Face(face);
        for (long i = 0; i < count; i++) {
            if (FT_New_Face(library, *paths, i, &face))
                break;
            if (!family || (face->family_name && strstr(face->family_name, family)))
                return face;
            FT_Done_Face(face);
        }
        if (!FT_New_Face(library, *paths, 0, &face))
            return face;
    }
    return NULL;
}

void font_init(void)
{
    static const char *const regular[] = {
        "/usr/share/fonts/e5-noto/NotoSansCJK-Regular.ttc",
        "/mnt/e5-disk/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        NULL,
    };
    static const char *const bold[] = {
        "/usr/share/fonts/e5-noto/NotoSansCJK-Bold.ttc",
        "/mnt/e5-disk/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
        NULL,
    };
    static const char *const latin[] = {"/usr/share/fonts/ttf-dejavu/DejaVuSans.ttf", NULL};
    /* Light text on dark backgrounds looks thin when coverage is blended linearly. */
    for (int i = 0; i < 256; i++)
        coverage[i] = (uint8_t)(255.0 * pow(i / 255.0, 0.8) + 0.5);
    if (FT_Init_FreeType(&library)) {
        fprintf(stderr, "freetype: init failed\n");
        exit(1);
    }
    fallback = open_face(latin, NULL);
    faces[0] = open_face(regular, "Noto Sans CJK SC");
    faces[1] = open_face(bold, "Noto Sans CJK SC");
    if (!faces[0])
        faces[0] = fallback;
    if (!faces[1])
        faces[1] = faces[0];
    if (!faces[0]) {
        fprintf(stderr, "freetype: no usable font\n");
        exit(1);
    }
}

static struct glyph *slot_for(struct sized_font *f, uint32_t letter)
{
    uint32_t mask = f->capacity - 1, i = (letter * 2654435761u) & mask;
    while (f->glyphs[i].letter && f->glyphs[i].letter != letter)
        i = (i + 1) & mask;
    return &f->glyphs[i];
}

static void grow(struct sized_font *f)
{
    struct glyph *old = f->glyphs;
    uint32_t old_capacity = f->capacity;
    f->capacity = old_capacity ? old_capacity * 2 : 256;
    f->glyphs = calloc(f->capacity, sizeof *f->glyphs);
    for (uint32_t i = 0; i < old_capacity; i++)
        if (old[i].letter)
            *slot_for(f, old[i].letter) = old[i];
    free(old);
}

static const struct glyph *render(struct sized_font *f, uint32_t letter)
{
    if (!letter)
        return NULL;
    if ((f->count + 1) * 4 > f->capacity * 3)
        grow(f);
    struct glyph *g = slot_for(f, letter);
    if (g->letter)
        return g->bitmap || g->adv_w ? g : NULL;
    g->letter = letter;
    f->count++;
    FT_UInt index = FT_Get_Char_Index(f->face, letter);
    if (!index)
        return NULL;
    FT_Activate_Size(f->size);
    if (FT_Load_Glyph(f->face, index, FT_LOAD_TARGET_LIGHT) || FT_Render_Glyph(f->face->glyph, FT_RENDER_MODE_NORMAL))
        return NULL;
    FT_GlyphSlot slot = f->face->glyph;
    g->adv_w = (uint16_t)((slot->advance.x + 32) >> 6);
    g->box_w = (uint16_t)slot->bitmap.width;
    g->box_h = (uint16_t)slot->bitmap.rows;
    g->ofs_x = (int16_t)slot->bitmap_left;
    g->ofs_y = (int16_t)(slot->bitmap_top - (int)slot->bitmap.rows);
    if (g->box_w && g->box_h) {
        g->bitmap = malloc((size_t)g->box_w * g->box_h);
        for (int y = 0; y < g->box_h; y++)
            for (int x = 0; x < g->box_w; x++)
                g->bitmap[(size_t)y * g->box_w + x] = coverage[slot->bitmap.buffer[y * slot->bitmap.pitch + x]];
    }
    return g;
}

static bool glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc, uint32_t letter, uint32_t next)
{
    pthread_mutex_lock(&lock);
    const struct glyph *g = render((struct sized_font *)font, letter);
    if (!g) {
        pthread_mutex_unlock(&lock);
        return false;
    }
    dsc->adv_w = g->adv_w;
    dsc->box_w = g->box_w;
    dsc->box_h = g->box_h;
    dsc->ofs_x = g->ofs_x;
    dsc->ofs_y = g->ofs_y;
    dsc->format = LV_FONT_GLYPH_FORMAT_A8;
    dsc->gid.index = letter;
    dsc->is_placeholder = 0;
    pthread_mutex_unlock(&lock);
    return true;
}

static const void *glyph_bitmap(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *buf)
{
    struct sized_font *f = (struct sized_font *)dsc->resolved_font;
    pthread_mutex_lock(&lock);
    const struct glyph *g = render(f, dsc->gid.index);
    bool drawn = g && g->bitmap && buf;
    if (drawn)
        for (int y = 0; y < g->box_h; y++)
            memcpy(buf->data + (size_t)y * buf->header.stride, g->bitmap + (size_t)y * g->box_w, g->box_w);
    pthread_mutex_unlock(&lock);
    return drawn ? buf : NULL;
}

static struct sized_font *create(FT_Face face, int px, bool bold)
{
    struct sized_font *f = calloc(1, sizeof *f);
    f->face = face;
    f->px = px;
    f->bold = bold;
    FT_New_Size(face, &f->size);
    FT_Activate_Size(f->size);
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
    f->font.get_glyph_dsc = glyph_dsc;
    f->font.get_glyph_bitmap = glyph_bitmap;
    f->font.line_height = (int32_t)((f->size->metrics.ascender - f->size->metrics.descender + 63) >> 6);
    f->font.base_line = (int32_t)((-f->size->metrics.descender + 63) >> 6);
    f->font.underline_position = -2;
    f->font.underline_thickness = 1;
    return f;
}

const lv_font_t *font_get(int px, bool bold)
{
    if (px < 6)
        px = 6;
    for (struct sized_font *f = fonts; f; f = f->next)
        if (f->px == px && f->bold == bold)
            return &f->font;
    pthread_mutex_lock(&lock);
    struct sized_font *f = create(faces[bold], px, bold), *tail = f;
    if (fallback && fallback != faces[bold]) {
        tail = create(fallback, px, false);
        f->font.fallback = &tail->font;
    }
    /* LVGL's symbols (LV_SYMBOL_*) come from its built-in font. */
    tail->font.fallback = &lv_font_montserrat_14;
    f->next = fonts;
    fonts = f;
    pthread_mutex_unlock(&lock);
    return &f->font;
}
