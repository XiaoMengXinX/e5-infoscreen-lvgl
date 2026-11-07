#include <poll.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "api.h"
#include "app.h"
#include "drm.h"
#include "font.h"
#include "input.h"
#include "stk.h"

static uint32_t tick_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static volatile sig_atomic_t dump_requested;

static void request_dump(int sig)
{
    dump_requested = 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--map-sim-channels"))
        return stk_map_channels();
    signal(SIGPIPE, SIG_IGN);
    signal(SIGUSR1, request_dump);
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *display = drm_display_create("/dev/dri/card0");
    if (!display)
        return 1;
    font_init();
    api_init();
    input_init(display);
    shell_init();

    struct pollfd fds[INPUT_MAX_DEVICES + 1];
    for (;;) {
        uint32_t wait = lv_timer_handler();
        int n = input_poll_fds(fds);
        fds[n] = (struct pollfd){.fd = api_fd(), .events = POLLIN};
        /* Replies held through a page's fast motion run as soon as it slows, which
         * a finger coming to rest does without an event to wake the loop. */
        int timeout = wait == LV_NO_TIMER_READY ? -1 : (int)wait;
        bool flush_held = api_held() && !shell_sliding();
        if (api_held())
            timeout = flush_held ? 0 : timeout < 0 || timeout > 20 ? 20 : timeout;
        int ready = poll(fds, (nfds_t)n + 1, timeout);
        if (dump_requested) {
            dump_requested = 0;
            drm_display_save("/tmp/screen.png");
        }
        if (ready > 0)
            input_dispatch(fds, n);
        /* Filling a page drops frames, so its replies wait while it moves fast. */
        if ((ready > 0 && fds[n].revents & POLLIN) || flush_held)
            api_dispatch(shell_sliding());
    }
}
