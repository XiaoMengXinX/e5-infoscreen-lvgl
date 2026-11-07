#pragma once

#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define INPUT_MAX_DEVICES 12

void input_init(lv_display_t *display);
int input_poll_fds(struct pollfd *fds);
void input_dispatch(const struct pollfd *fds, int count);
int input_touch_count(void);
lv_indev_t *input_touch(void);
void input_focus_key(uint32_t key);
void input_defocus(void);
/* Moves a group's focus for the arrow keys and shows it; the first press only
 * reveals the focus already there. */
void input_focus_move(lv_group_t *group, int dir);
/* Moves the focus to the nearest object of the group that way on screen, for
 * layouts read as a grid; with nothing shown focused it acts as input_focus_move. */
void input_focus_toward(lv_group_t *group, int dx, int dy);
/* The group's focus as the keys show it: LVGL focuses the first object a group
 * gets without marking it, and a view opening over another leaves the focus
 * behind on it; neither counts. NULL when there is none. */
lv_obj_t *input_key_focus(lv_group_t *group);
void input_set_group(lv_group_t *group);
lv_group_t *input_group(void);
