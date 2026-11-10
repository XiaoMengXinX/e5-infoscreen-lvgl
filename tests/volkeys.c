#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int generation, reads, writes;
static char mode[32], step[32], off[32];
static int test_stat(const char *path, struct stat *out)
{
    memset(out, 0, sizeof *out);
    out->st_ino = generation;
    return generation ? 0 : -1;
}

#define stat(...) test_stat(__VA_ARGS__)
#define system test_system
#include "../src/volkeys.c"
#undef stat
#undef system

void uci_get(const char *option, char *out, size_t size)
{
    reads++;
    const char *value = strstr(option, ".mode") ? mode : strstr(option, ".step") ? step : off;
    snprintf(out, size, "%s", value);
}

int test_system(const char *command)
{
    assert(strstr(command, "uci commit e5-plugin-volkeys"));
    assert(!strstr(command, "e5-infoscreen.main"));
    writes++;
    return 0;
}

int main(void)
{
    volkeys_init();
    assert(config.mode == VOLKEYS_BRIGHTNESS && config.step == 10 && config.off_at_min);
    for (int step = 5; step <= 25; step++) {
        config.step = step;
        for (int level = 10; level <= 255; level++) {
            int percent = (level * 100 + 127) / 255;
            for (int direction = -1; direction <= 1; direction += 2) {
                int expected = percent + direction * step;
                if (expected < step)
                    expected = step;
                if (expected > 100)
                    expected = 100;
                int next = volkeys_brightness(level, direction);
                assert(next >= 10 && next <= 255);
                assert((next * 100 + 127) / 255 == expected);
            }
        }
        int minimum = 255;
        for (int i = 0; i < 20; i++)
            minimum = volkeys_brightness(minimum, -1);
        assert((minimum * 100 + 127) / 255 == step);
        assert(volkeys_brightness(minimum, -1) == minimum);
        assert(volkeys_action(-1, false, false, false, minimum) == VOLKEYS_CUT);
        assert(volkeys_action(-1, true, false, false, minimum) == VOLKEYS_ADJUST);
        assert(volkeys_action(-1, false, false, false, minimum + 1) == VOLKEYS_ADJUST);
    }
    config.step = 10;
    assert(volkeys_action(1, false, false, false, 120) == VOLKEYS_ADJUST);
    assert(volkeys_action(-1, false, true, false, 10) == VOLKEYS_NONE);
    assert(volkeys_action(1, false, true, false, 10) == VOLKEYS_WAKE);
    assert(volkeys_action(1, true, true, false, 10) == VOLKEYS_NONE);
    assert(volkeys_action(-1, true, false, false, 10) == VOLKEYS_ADJUST);
    assert(volkeys_action(-1, false, false, false, 10) == VOLKEYS_CUT);
    volkeys_set_cut(true);
    assert(volkeys_action(-1, false, true, false, 10) == VOLKEYS_NONE);
    assert(volkeys_action(1, true, true, false, 10) == VOLKEYS_NONE);
    assert(volkeys_action(1, false, true, false, 10) == VOLKEYS_RESTORE);
    assert(volkeys_action(-1, false, true, true, 10) == VOLKEYS_AUDIO);
    volkeys_set_cut(false);

    snprintf(mode, sizeof mode, "power");
    snprintf(step, sizeof step, "25");
    snprintf(off, sizeof off, "0");
    generation++;
    volkeys_refresh();
    assert(config.mode == VOLKEYS_POWER && config.step == 25 && !config.off_at_min);
    assert(reads == 6);
    for (int i = 0; i < 20; i++)
        volkeys_refresh();
    assert(reads == 6);
    assert(volkeys_action(1, false, false, false, 120) == VOLKEYS_TOGGLE);
    assert(volkeys_action(-1, true, false, false, 120) == VOLKEYS_NONE);
    assert(volkeys_action(1, false, false, true, 120) == VOLKEYS_AUDIO);
    snprintf(mode, sizeof mode, "volume");
    generation++;
    volkeys_refresh();
    assert(volkeys_action(-1, true, true, false, 120) == VOLKEYS_AUDIO);
    assert(volkeys_save("mode", "volume") && writes == 1);
    assert(!volkeys_save("mode", "power;reboot"));
    assert(!volkeys_save("step", "4") && !volkeys_save("step", "26"));
    assert(!volkeys_save("step", "10;reboot"));
    assert(!volkeys_save("unknown", "1"));
    assert(writes == 1);

    snprintf(mode, sizeof mode, "brightness");
    generation++;
    volkeys_refresh();
    assert(volkeys_action(-1, false, true, false, 10) == VOLKEYS_WAKE);
    assert(volkeys_action(-1, false, false, false, 10) == VOLKEYS_ADJUST);
    puts("Volume keys: shared configuration, external updates, repeat, call priority and cut/wake policy passed");
}
