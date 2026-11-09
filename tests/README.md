Run `python3 build.py`, then `python3 tools/check.py`.

The DRM test simulates completion events, delayed events, timeouts, failed flips and display timing. The motion test checks curve accuracy, release velocity and spring results across frame intervals. Hardware measurements use `tools/e5.py`.

The LPAC test checks AT replies, large JSON output, graceful cancellation, download confirmation, refresh failures and long arguments. The eSIM tests check UTF-8 names, complete notification lists, switching while hidden, preview confirmation and cancellation, automatic reporting of new notifications, and rename navigation.

The install test checks byte-count and percentage progress, keeping the screen awake, successful and failed scripts, EOF handling, and cancellation of the script process group.

Multi-SE coverage checks eSTK detection, both SEs and partially available SEs, selection with fresh profile/notification data, notification and delete routing, and per-job AID ownership and child-process environment. It also checks left/right selection with focus retained after loading, and waits for the requested profile state after modem refresh.
