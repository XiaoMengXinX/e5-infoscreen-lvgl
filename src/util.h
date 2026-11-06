#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

const cJSON *jget(const cJSON *object, const char *path);
bool jnum(const cJSON *object, const char *path, double *out);
double jnum_or(const cJSON *object, const char *path, double fallback);
const char *jstr(const cJSON *object, const char *path);
bool jtrue(const cJSON *object, const char *path);
bool jfalse(const cJSON *object, const char *path);
const char *jlabel(const cJSON *label);

void error_text(const char *in, char *out, size_t size);
void label_text(const cJSON *label, char *out, size_t size);

void fmt_trim(double value, int decimals, char *out, size_t size);
void fmt_bytes(double bytes, char *out, size_t size);
void fmt_amount(double bytes, char *out, size_t size);
void fmt_speed(double bytes_per_s, bool bytes_mode, char *num, char *unit, size_t size);
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
void screen_option_set(const char *name, const char *value);

struct local_time {
    int year, month, day, hour, minute, second;
};
struct local_time local_now(void);
struct local_time local_at(double epoch);
