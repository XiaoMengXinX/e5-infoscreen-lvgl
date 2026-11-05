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
void input_set_group(lv_group_t *group);
lv_group_t *input_group(void);
