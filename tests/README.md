Run `python3 build.py`, then `python3 tools/check.py`.

The DRM test simulates completion events, delayed events, timeouts, failed flips, display timing and the partial frames each buffer catches up on. The motion test checks curve accuracy, release velocity and spring results across frame intervals. Hardware measurements use `tools/e5.py`.

The LPAC test checks AT replies, large JSON output, graceful cancellation, download confirmation, refresh failures and long arguments. The eSIM tests check UTF-8 names, complete notification lists, switching while hidden, preview confirmation and cancellation, automatic reporting of new notifications, and rename navigation.

`lpac_bridge.py` covers system-backend selection and shared locks, AT/stdio driver selection and reuse, APDU channel ownership, HTTPS requests, TLS dependencies, metadata confirmation/cancellation, structured errors on failed exits and a 50 KB reply. It also exercises the checked-out system backend's real stdio loop against a simulated lpac/card process. The C tests check system jobs without a PTY and installation of the missing TLS package. These checks run locally without hardware or network requests.

The install test checks byte-count and percentage progress, keeping the screen awake, successful and failed scripts, EOF handling, and cancellation of the script process group.

Multi-SE coverage checks eSTK detection, both SEs and partially available SEs, selection with fresh profile/notification data, notification and delete routing, and per-job AID ownership and child-process environment. It also checks left/right selection with focus retained after loading, and waits for the requested profile state after modem refresh.

APDU interface coverage checks saved CSIM/CCHO selection, keyboard selection, per-job interface ownership, and routing to both SEs through the system backend. Local socket fixtures verify CSIM MANAGE CHANNEL, SELECT, transmission with the actual channel in CLA, and channel cleanup after success or failed selection, alongside the CCHO/CGLA path.

The baseband restart test checks the kernel CP stop/start sequence, lock exclusion, single/dual-SIM presence, data connection restoration, CP failure cleanup and returned WAN availability.

Backend selection coverage checks installed driver capabilities, disabled choices, saved selection, forced stdio routing, and interface-key focus.

The volume-key test covers the original plugin configuration, defaults, cached reads and external edits, call priority, hold behavior, and minimum-brightness cut/wake behavior.
