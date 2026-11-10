#include "volkeys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "util.h"

#define CONFIG "/etc/config/e5-plugin-volkeys"

static const char *const modes[] = {"brightness", "power", "volume"};
static struct volkeys_config config;
static struct stat config_stat;
static bool config_exists, cut;

static void reload(void)
{
    char value[32];
    config.mode = VOLKEYS_BRIGHTNESS;
    uci_get("e5-plugin-volkeys.settings.mode", value, sizeof value);
    for (unsigned i = 0; i < sizeof modes / sizeof *modes; i++)
        if (!strcmp(value, modes[i]))
            config.mode = (enum volkeys_mode)i;
    uci_get("e5-plugin-volkeys.settings.step", value, sizeof value);
    int step = atoi(value);
    config.step = step >= 5 && step <= 25 ? step : 10;
    uci_get("e5-plugin-volkeys.settings.off_at_min", value, sizeof value);
    config.off_at_min = strcmp(value, "0") != 0;
    config_exists = stat(CONFIG, &config_stat) == 0;
}

void volkeys_refresh(void)
{
    struct stat current;
    bool exists = stat(CONFIG, &current) == 0;
    if (exists != config_exists || (exists &&
        (current.st_ino != config_stat.st_ino || current.st_size != config_stat.st_size ||
         current.st_mtim.tv_sec != config_stat.st_mtim.tv_sec ||
         current.st_mtim.tv_nsec != config_stat.st_mtim.tv_nsec)))
        reload();
}

const struct volkeys_config *volkeys_config(void)
{
    return &config;
}

const char *volkeys_mode_name(enum volkeys_mode mode)
{
    return modes[mode];
}

bool volkeys_save(const char *field, const char *value)
{
    if (!strcmp(field, "mode")) {
        bool valid = false;
        for (unsigned i = 0; i < sizeof modes / sizeof *modes; i++)
            valid |= !strcmp(value, modes[i]);
        if (!valid)
            return false;
    } else if (!strcmp(field, "step")) {
        char *end;
        long step = strtol(value, &end, 10);
        if (*end || step < 5 || step > 25)
            return false;
    } else if (strcmp(field, "off_at_min") || (strcmp(value, "0") && strcmp(value, "1"))) {
        return false;
    }
    char command[384];
    snprintf(command, sizeof command,
             "touch " CONFIG " && uci set e5-plugin-volkeys.settings=settings && "
             "uci set e5-plugin-volkeys.settings.%s='%s' && uci commit e5-plugin-volkeys", field, value);
    if (system(command))
        return false;
    reload();
    return true;
}

int volkeys_brightness(int brightness, int direction)
{
    int percent = (brightness * 100 + 127) / 255 + direction * config.step;
    if (percent < config.step)
        percent = config.step;
    if (percent > 100)
        percent = 100;
    return (percent * 255 + 50) / 100;
}

enum volkeys_action volkeys_action(int direction, bool repeat, bool blank, bool call, int brightness)
{
    if (call)
        return VOLKEYS_AUDIO;
    if (cut)
        return direction > 0 && !repeat ? VOLKEYS_RESTORE : VOLKEYS_NONE;
    if (config.mode == VOLKEYS_VOLUME)
        return VOLKEYS_AUDIO;
    if (config.mode == VOLKEYS_POWER)
        return repeat ? VOLKEYS_NONE : VOLKEYS_TOGGLE;
    if (blank)
        return !repeat && (direction > 0 || !config.off_at_min) ? VOLKEYS_WAKE : VOLKEYS_NONE;
    if (direction < 0 && !repeat && config.off_at_min && brightness <= (config.step * 255 + 50) / 100)
        return VOLKEYS_CUT;
    return VOLKEYS_ADJUST;
}

bool volkeys_cut_active(void)
{
    return cut;
}

void volkeys_set_cut(bool on)
{
    cut = on;
}

void volkeys_init(void)
{
    volkeys_set_cut(false);
    reload();
}
