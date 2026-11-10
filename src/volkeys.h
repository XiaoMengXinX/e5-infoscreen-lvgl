#pragma once

#include <stdbool.h>

enum volkeys_mode { VOLKEYS_BRIGHTNESS, VOLKEYS_POWER, VOLKEYS_VOLUME };
enum volkeys_action { VOLKEYS_NONE, VOLKEYS_ADJUST, VOLKEYS_TOGGLE, VOLKEYS_WAKE,
                      VOLKEYS_CUT, VOLKEYS_RESTORE, VOLKEYS_AUDIO };

struct volkeys_config {
    enum volkeys_mode mode;
    int step;
    bool off_at_min;
};

void volkeys_init(void);
void volkeys_refresh(void);
const struct volkeys_config *volkeys_config(void);
const char *volkeys_mode_name(enum volkeys_mode mode);
bool volkeys_save(const char *field, const char *value);
int volkeys_brightness(int brightness, int direction);
enum volkeys_action volkeys_action(int direction, bool repeat, bool blank, bool call, int brightness);
bool volkeys_cut_active(void);
void volkeys_set_cut(bool on);
