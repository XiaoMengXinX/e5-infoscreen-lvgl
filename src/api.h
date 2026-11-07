#pragma once

#include <stdbool.h>

#include "cJSON.h"

/* status is the HTTP status, or 0 when the request did not complete. reply
 * is the parsed body (NULL if it was not JSON) and is freed after the call. */
typedef void (*api_cb)(int status, cJSON *reply, void *ctx);
typedef void (*api_text_cb)(int status, const char *body, void *ctx);
typedef void (*api_event_cb)(cJSON *event);

void api_init(void);
int api_fd(void);
/* Runs the callbacks of finished requests. With hold, replies wait for a later
 * call without it; call events never wait. */
void api_dispatch(bool hold);
bool api_held(void);

void api_get(const char *path, api_cb cb, void *ctx);
void api_get_text(const char *path, api_text_cb cb, void *ctx);
void api_post(const char *path, cJSON *body, api_cb cb, void *ctx);

/* A page read fails on a transport error, an HTTP error, or an envelope
 * that carries only an error. */
bool api_read_ok(int status, const cJSON *reply);
bool api_reply_ok(const cJSON *reply);

void api_call_events(api_event_cb cb);
