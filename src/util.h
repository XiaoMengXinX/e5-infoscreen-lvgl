#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

const cJSON *jget(const cJSON *object, const char *path);
bool jnum(const cJSON *object, const char *path, double *out);
double jnum_or(const cJSON *object, const char *path, double fallback);
const char *jstr(const cJSON *object, const char *path);
bool jhas(const cJSON *object, const char *path);
const char *jstr_or(const cJSON *object, const char *path, const char *fallback);
bool jstr_is(const cJSON *object, const char *path, const char *value);
const cJSON *jget_or(const cJSON *object, const char *path, const cJSON *fallback);
bool jtrue(const cJSON *object, const char *path);
bool jfalse(const cJSON *object, const char *path);
const char *jlabel(const cJSON *label);

void error_text(const char *in, char *out, size_t size);
void label_text(const cJSON *label, char *out, size_t size);

void fmt_trim(double value, int decimals, char *out, size_t size);
void fmt_bytes(double bytes, char *out, size_t size);
void fmt_amount(double bytes, char *out, size_t size);
/* At most four digits, and no more decimals than asked for. */
void fmt_speed(double bytes_per_s, bool bytes_mode, int decimals, char *num, char *unit, size_t size);
void fmt_duration(double seconds, char *out, size_t size);
void fmt_temp(const cJSON *value, char *out, size_t size);
const char *tech_name(const char *tech);
const char *operator_name(const char *name);
const char *charge_state(const cJSON *battery);
void battery_text(const cJSON *battery, char *out, size_t size);
void usb_device_name(const cJSON *device, char *out, size_t size);

/* The screen's own options, kept in UCI as e5-infoscreen.main.<name>. Reading one
 * gives an empty string when it is not set. */
void screen_option(const char *name, char *out, size_t size);
bool screen_option_set(const char *name, const char *value);
/* A setting the web interface's plugin also has: its copy (e5-plugin-<plugin>.settings.<setting>)
 * when there is one, else the screen's own. */
void shared_option(const char *name, const char *plugin, const char *setting, char *out, size_t size);
void shared_option_set(const char *name, const char *plugin, const char *setting, const char *value);
/* Any UCI option, "package.section.option"; an empty string when it is not set. */
void uci_get(const char *option, char *out, size_t size);

struct local_time {
    int year, month, day, hour, minute, second;
};
struct local_time local_now(void);
struct local_time local_at(double epoch);
/* "2026-11-07T00:38:00+00:00" (or Z, or no offset for UTC) to seconds since the epoch. */
bool iso_epoch(const char *iso, double *epoch);
