#include "keyboard.h"

#include <ctype.h>
#include <string.h>

#include "app.h"
#include "input.h"
#include "theme.h"

#define NO_REPEAT LV_BUTTONMATRIX_CTRL_NO_REPEAT
#define SPECIAL (LV_BUTTONMATRIX_CTRL_CHECKED | NO_REPEAT)
#define SPACER (LV_BUTTONMATRIX_CTRL_HIDDEN | LV_BUTTONMATRIX_CTRL_DISABLED | 1)

enum layout { LOWER, UPPER, SYMBOLS };

static lv_obj_t *keyboard, *target, *scroller;
static enum layout layout;
static const char *maps[3][42];
static lv_buttonmatrix_ctrl_t ctrls[3][37];
static char upper[26][2];

/* Rows of 20 width units: ten keys, nine keys inset by half a key, the shift
 * row, and the bottom row with space. */
static void build_maps(void)
{
    static const char *const letters[26] = {
        "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "a", "s", "d", "f", "g", "h", "j", "k", "l",
        "z", "x", "c", "v", "b", "n", "m",
    };
    static const char *const symbols[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "/", ":", ";", "(",
                                          ")", "¥", "&", "@", "\"", ".", ",", "?", "!", "'", "_", "#"};
    for (int i = 0; i < 26; i++) {
        upper[i][0] = (char)toupper((unsigned char)letters[i][0]);
        upper[i][1] = 0;
    }
    for (int l = 0; l < 3; l++) {
        const char **m = maps[l];
        lv_buttonmatrix_ctrl_t *c = ctrls[l];
        int n = 0, k = 0;
        for (int i = 0; i < 10; i++, k++) {
            m[n++] = l == SYMBOLS ? symbols[i] : l == UPPER ? upper[i] : letters[i];
            *c++ = 2 | NO_REPEAT;
        }
        m[n++] = "\n";
        if (l == SYMBOLS) {
            for (int i = 10; i < 20; i++) {
                m[n++] = symbols[i];
                *c++ = 2 | NO_REPEAT;
            }
        } else {
            m[n++] = " ";
            *c++ = SPACER;
            for (int i = 0; i < 9; i++, k++) {
                m[n++] = l == UPPER ? upper[k] : letters[k];
                *c++ = 2 | NO_REPEAT;
            }
            m[n++] = " ";
            *c++ = SPACER;
        }
        m[n++] = "\n";
        m[n++] = l == SYMBOLS ? "*" : LV_SYMBOL_UP;
        *c++ = 3 | SPECIAL;
        for (int i = 0; i < 7; i++, k++) {
            m[n++] = l == SYMBOLS ? symbols[20 + i] : l == UPPER ? upper[k] : letters[k];
            *c++ = 2 | NO_REPEAT;
        }
        m[n++] = LV_SYMBOL_BACKSPACE;
        *c++ = 3 | LV_BUTTONMATRIX_CTRL_CHECKED;
        m[n++] = "\n";
        m[n++] = l == SYMBOLS ? "ABC" : "123";
        *c++ = 4 | SPECIAL;
        m[n++] = L("空格", "space");
        *c++ = 9 | NO_REPEAT;
        m[n++] = L("完成", "Done");
        *c++ = 4 | SPECIAL;
        m[n++] = LV_SYMBOL_DOWN;
        *c++ = 3 | SPECIAL;
        m[n] = "";
    }
}

static void show(enum layout next)
{
    layout = next;
    lv_buttonmatrix_set_map(keyboard, maps[layout]);
    lv_buttonmatrix_set_ctrl_map(keyboard, ctrls[layout]);
}

void keyboard_close(void)
{
    if (!keyboard)
        return;
    lv_obj_delete(keyboard);
    keyboard = target = NULL;
    if (scroller)
        lv_obj_set_style_pad_bottom(scroller, S(14), 0);
    scroller = NULL;
}

static void on_key(lv_event_t *e)
{
    const uint32_t *id = lv_event_get_param(e);
    const char *text = id ? lv_buttonmatrix_get_button_text(keyboard, *id) : NULL;
    if (!text || !target)
        return;
    if (!strcmp(text, LV_SYMBOL_UP)) {
        show(layout == UPPER ? LOWER : UPPER);
    } else if (!strcmp(text, "123")) {
        show(SYMBOLS);
    } else if (!strcmp(text, "ABC")) {
        show(LOWER);
    } else if (!strcmp(text, LV_SYMBOL_BACKSPACE)) {
        lv_textarea_delete_char(target);
    } else if (!strcmp(text, L("完成", "Done"))) {
        lv_obj_send_event(target, LV_EVENT_READY, NULL);
    } else if (!strcmp(text, LV_SYMBOL_DOWN)) {
        input_defocus();
    } else if (!strcmp(text, L("空格", "space"))) {
        lv_textarea_add_char(target, ' ');
    } else {
        lv_textarea_add_text(target, text);
        if (layout == UPPER)
            show(LOWER);
    }
}

static void open(lv_obj_t *ta)
{
    keyboard_close();
    build_maps();
    target = ta;
    keyboard = lv_buttonmatrix_create(lv_layer_top());
    lv_group_remove_obj(keyboard);
    lv_obj_set_size(keyboard, LV_PCT(100), S(164));
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_color_t base = theme.light ? lv_color_hex(0xd1d5db) : lv_color_hex(0x1b1d21);
    lv_color_t key = theme.light ? lv_color_white() : lv_color_hex(0x4a4d53);
    lv_color_t special = theme.light ? lv_color_hex(0xaab0bb) : lv_color_hex(0x2e3136);
    lv_color_t ink = theme.light ? lv_color_hex(0x111111) : lv_color_white();
    lv_obj_set_style_bg_color(keyboard, base, 0);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(keyboard, S(5), 0);
    lv_obj_set_style_pad_row(keyboard, S(7), 0);
    lv_obj_set_style_pad_column(keyboard, S(5), 0);
    lv_obj_set_style_text_font(keyboard, F(16), LV_PART_ITEMS);
    lv_obj_set_style_text_color(keyboard, ink, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, key, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(keyboard, S(5), LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_border_width(keyboard, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(keyboard, special, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(keyboard, F(13), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(keyboard, lv_color_mix(ink, key, 60), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(keyboard, lv_color_mix(ink, special, 60), LV_PART_ITEMS | LV_STATE_CHECKED | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(keyboard, special, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(keyboard, LV_OPA_TRANSP, LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_add_event_cb(keyboard, on_key, LV_EVENT_VALUE_CHANGED, NULL);
    show(LOWER);
    for (scroller = lv_obj_get_parent(ta); scroller && !lv_obj_is_scrollable(scroller);)
        scroller = lv_obj_get_parent(scroller);
    if (scroller)
        lv_obj_set_style_pad_bottom(scroller, lv_obj_get_height(keyboard), 0);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
}

static void on_event(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED)
        open(ta);
    else if ((code == LV_EVENT_READY || code == LV_EVENT_DEFOCUSED || code == LV_EVENT_DELETE) && ta == target)
        keyboard_close();
}

void keyboard_bind(lv_obj_t *textarea)
{
    lv_obj_set_style_border_side(textarea, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_border_width(textarea, 1, LV_PART_CURSOR);
    lv_obj_set_style_border_color(textarea, theme.accent, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(textarea, 500, LV_PART_CURSOR);
    lv_obj_set_style_opa(textarea, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_opa(textarea, LV_OPA_COVER, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_add_event_cb(textarea, on_event, LV_EVENT_ALL, NULL);
}
