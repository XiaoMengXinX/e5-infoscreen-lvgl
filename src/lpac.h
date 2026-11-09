#pragma once

#include <stdbool.h>

#include "cJSON.h"

/* Bridges lpac's AT device through a pty to the selected SIM's command ring. */

typedef void (*lpac_progress_fn)(const char *step, const cJSON *data);
/* data is the "lpa" payload's data field, valid only during the call. */
typedef void (*lpac_done_fn)(bool ok, const cJSON *data, const char *error, const char *detail);

bool lpac_present(void);

/* Runs one lpac command against the SIM slot (0 or 1), e.g. {"profile", "list",
 * NULL}, queued behind earlier calls. progress may be NULL. */
void lpac_call(int card, char *const argv[], lpac_progress_fn progress, lpac_done_fn done);

/* Continues the current download after its metadata preview. */
bool lpac_download_confirm(bool accept);
void lpac_cancel(void);

/* Lets the modem re-read the SIM in the slot after a profile switch. */
void lpac_sim_refresh(int card, lpac_done_fn done);

/* Stops whatever runs and empties the queue without calling back. */
void lpac_stop(void);
