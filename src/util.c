#include "util.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "platform_errors.h"

const cJSON *jget(const cJSON *object, const char *path)
{
    char key[64];
    while (object && *path) {
        size_t n = strcspn(path, ".");
        if (n >= sizeof key)
            return NULL;
        memcpy(key, path, n);
        key[n] = 0;
        object = cJSON_GetObjectItemCaseSensitive(object, key);
        path += n + (path[n] == '.');
    }
    return object;
}

bool jnum(const cJSON *object, const char *path, double *out)
{
    const cJSON *item = jget(object, path);
    if (!cJSON_IsNumber(item))
        return false;
    *out = item->valuedouble;
    return true;
}

double jnum_or(const cJSON *object, const char *path, double fallback)
{
    double value;
    return jnum(object, path, &value) ? value : fallback;
}

const char *jstr(const cJSON *object, const char *path)
{
    const cJSON *item = jget(object, path);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

bool jhas(const cJSON *object, const char *path)
{
    const cJSON *item = jget(object, path);
    return item && !cJSON_IsNull(item);
}

const char *jstr_or(const cJSON *object, const char *path, const char *fallback)
{
    const char *s = jstr(object, path);
    return s ? s : fallback;
}

bool jstr_is(const cJSON *object, const char *path, const char *value)
{
    const char *s = jstr(object, path);
    return s && !strcmp(s, value);
}

const cJSON *jget_or(const cJSON *object, const char *path, const cJSON *fallback)
{
    const cJSON *item = jget(object, path);
    return item ? item : fallback;
}

/* JavaScript truthiness for the backend's booleans, which ucode may send as 0/1. */
bool jtrue(const cJSON *object, const char *path)
{
    const cJSON *item = jget(object, path);
    return cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valuedouble != 0);
}

bool jfalse(const cJSON *object, const char *path)
{
    return cJSON_IsFalse(jget(object, path));
}

const char *jlabel(const cJSON *label)
{
    if (cJSON_IsString(label))
        return label->valuestring;
    const char *text = jstr(label, app.en ? "en" : "zh");
    return text ? text : jstr_or(label, "zh", "");
}

void error_text(const char *in, char *out, size_t size)
{
    for (size_t i = 0; i < sizeof PLATFORM_CODES / sizeof *PLATFORM_CODES; i++)
        if (!strcmp(in, PLATFORM_CODES[i][0])) {
            in = PLATFORM_CODES[i][1];
            break;
        }
    size_t len = 0;
    while (*in && len + 1 < size) {
        bool replaced = false;
        if (!app.en)
            for (size_t i = 0; i < sizeof PLATFORM_ERRORS / sizeof *PLATFORM_ERRORS; i++) {
                size_t n = strlen(PLATFORM_ERRORS[i][0]);
                if (strncmp(in, PLATFORM_ERRORS[i][0], n))
                    continue;
                len += (size_t)snprintf(out + len, size - len, "%s", PLATFORM_ERRORS[i][1]);
                len = len < size ? len : size - 1;
                in += n;
                replaced = true;
                break;
            }
        if (!replaced)
            out[len++] = *in++;
    }
    out[len] = 0;
}

void label_text(const cJSON *label, char *out, size_t size)
{
    error_text(jlabel(label), out, size);
}

void fmt_trim(double value, int decimals, char *out, size_t size)
{
    snprintf(out, size, "%.*f", decimals, value);
    if (strchr(out, '.')) {
        char *end = out + strlen(out) - 1;
        while (*end == '0')
            *end-- = 0;
        if (*end == '.')
            *end = 0;
    }
}

void fmt_bytes(double n, char *out, size_t size)
{
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1024 && i < 4) {
        n /= 1024;
        i++;
    }
    snprintf(out, size, "%.*f %s", n >= 100 || i == 0 ? 0 : 1, n, units[i]);
}

static int scaled(double *value, double base, int units)
{
    int unit = 0;
    while (*value >= base && unit < units - 1) {
        *value /= base;
        unit++;
    }
    *value = round(*value * (unit ? 100 : 1)) / (unit ? 100 : 1);
    if (*value >= base && unit < units - 1) {
        *value = 1;
        unit++;
    }
    return unit;
}

void fmt_amount(double bytes, char *out, size_t size)
{
    static const char *const units[] = {"B", "KB", "MB", "GB", "TB"};
    if (!isfinite(bytes)) {
        snprintf(out, size, "—");
        return;
    }
    double value = bytes > 0 ? bytes : 0;
    int unit = scaled(&value, 1000, 5);
    char num[32];
    fmt_trim(value, 2, num, sizeof num);
    snprintf(out, size, "%s %s", num, units[unit]);
}

void fmt_speed(double bytes_per_s, bool bytes_mode, char *num, char *unit, size_t size)
{
    static const char *const bits[] = {"bps", "Kbps", "Mbps", "Gbps", "Tbps"};
    static const char *const bytes[] = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
    const char *const *units = bytes_mode ? bytes : bits;
    if (!isfinite(bytes_per_s)) {
        snprintf(num, size, "—");
        snprintf(unit, size, "%s", units[0]);
        return;
    }
    double value = (bytes_per_s > 0 ? bytes_per_s : 0) * (bytes_mode ? 1 : 8);
    int i = scaled(&value, bytes_mode ? 1024 : 1000, 5);
    fmt_trim(value, 2, num, size);
    snprintf(unit, size, "%s", units[i]);
}

void fmt_duration(double s, char *out, size_t size)
{
    long total = (long)s, d = total / 86400, h = total % 86400 / 3600, m = total % 3600 / 60;
    size_t len = 0;
    if (d)
        len += (size_t)snprintf(out + len, size - len, app.en ? "%ldd " : "%ld天", d);
    if (d || h)
        len += (size_t)snprintf(out + len, size - len, app.en ? "%ldh " : "%ld小时", h);
    snprintf(out + len, size - len, app.en ? "%ldm" : "%ld分", m);
}

void fmt_temp(const cJSON *value, char *out, size_t size)
{
    if (cJSON_IsNumber(value) && isfinite(value->valuedouble))
        snprintf(out, size, "%.1f °C", value->valuedouble);
    else
        snprintf(out, size, "--");
}

const char *tech_name(const char *tech)
{
    static const char *const names[][2] = {
        {"5gnr", "5G"}, {"lte", "4G"}, {"umts", "3G"}, {"hsdpa", "3G"}, {"hsupa", "3G"}, {"hspa", "3G"},
        {"hspa-plus", "3G"}, {"gsm", "2G"}, {"edge", "2G"}, {"gprs", "2G"},
    };
    static char upper[32];
    if (!tech || !*tech)
        return "--";
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcmp(tech, names[i][0]))
            return names[i][1];
    size_t i = 0;
    for (; tech[i] && i + 1 < sizeof upper; i++)
        upper[i] = (char)toupper((unsigned char)tech[i]);
    upper[i] = 0;
    return upper;
}

const char *operator_name(const char *name)
{
    static const struct {
        const char *keys;
        const char *zh, *en;
    } aliases[] = {
        {"|中国移动|中国移动通信|CHINAMOBILE|CHNMOBILE|CMCC|46000|46002|46004|46007|46008|", "中国移动", "China Mobile"},
        {"|中国联通|CHINAUNICOM|CHNUNICOM|UNICOM|CUCC|46001|46006|46009|", "中国联通", "China Unicom"},
        {"|中国电信|CHINATELECOM|CHNTELECOM|CHNCT|CHNCTLTE|CTCC|46003|46005|46011|46012|", "中国电信", "China Telecom"},
        {"|中国广电|CHINABROADNET|CHNBG|CBN|CHINABROADCASTINGNETWORK|46015|", "中国广电", "China Broadnet"},
    };
    if (!name || !*name)
        return name;
    char key[80] = "|";
    size_t len = 1;
    for (const char *p = name; *p && len + 2 < sizeof key; p++)
        if (!strchr(" \t_-", *p))
            key[len++] = (char)toupper((unsigned char)*p);
    key[len++] = '|';
    key[len] = 0;
    for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++)
        if (strstr(aliases[i].keys, key))
            return L(aliases[i].zh, aliases[i].en);
    return name;
}

struct local_time local_at(double epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm;
    double offset;
    if (jnum(app.status, "tz_offset", &offset)) {
        t += (time_t)offset;
        gmtime_r(&t, &tm);
    } else {
        localtime_r(&t, &tm);
    }
    return (struct local_time){tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec};
}

bool iso_epoch(const char *iso, double *epoch)
{
    int y, mo, d, h, mi, s = 0, n = 0, oh = 0, om = 0, sign = 1;
    if (!iso || sscanf(iso, "%4d-%2d-%2dT%2d:%2d%n", &y, &mo, &d, &h, &mi, &n) != 5)
        return false;
    const char *rest = iso + n;
    int used = 0;
    if (sscanf(rest, ":%2d%n", &s, &used) == 1)
        rest += used;
    while (*rest == '.' || (*rest >= '0' && *rest <= '9'))
        rest++;
    if ((*rest == '+' || *rest == '-') && sscanf(rest + 1, "%2d:%2d", &oh, &om) >= 1 && *rest == '-')
        sign = -1;
    /* Days since 1970-01-01 by the civil calendar (Howard Hinnant's days_from_civil). */
    int yy = mo <= 2 ? y - 1 : y;
    int era = (yy >= 0 ? yy : yy - 399) / 400;
    int yoe = yy - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long days = (long)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    *epoch = (double)days * 86400 + h * 3600 + mi * 60 + s - sign * (oh * 60 + om) * 60;
    return true;
}

struct local_time local_now(void)
{
    return local_at((double)time(NULL));
}

const char *charge_state(const cJSON *b)
{
    if (!jtrue(b, "online"))
        return L("使用电池", "On battery");
    const char *status = jstr(b, "status");
    if ((status && !strcmp(status, "Full")) || jnum_or(b, "capacity", 0) >= 100)
        return L("已充满", "Full");
    if ((status && !strcmp(status, "Charging")) || jnum_or(b, "current_ma", 0) > 20)
        return L("充电中", "Charging");
    return L("未充电", "Not charging");
}

void battery_text(const cJSON *b, char *out, size_t size)
{
    double cap, limit;
    if (!jnum(b, "capacity", &cap)) {
        snprintf(out, size, "--");
    } else if (jtrue(b, "paused") && jtrue(b, "online")) {
        int n = snprintf(out, size, "%g%% · %s", cap, L("暂停", "paused"));
        if (jnum(b, "limit", &limit) && n > 0 && (size_t)n < size)
            snprintf(out + n, size - (size_t)n, " · %s %g%%", L("上限", "limit"), limit);
    } else {
        snprintf(out, size, "%g%% · %s", cap, charge_state(b));
    }
}

void usb_device_name(const cJSON *d, char *out, size_t size)
{
    const char *name = jstr_or(d, "name", "");
    if (!*name)
        name = "USB";
    const char *kind = jstr(d, "kind");
    if (!kind || strcmp(kind, "ethernet"))
        snprintf(out, size, "%s", name);
    else if (!strcmp(name, "USB"))
        snprintf(out, size, "%s", L("USB 网卡", "USB Ethernet"));
    else
        snprintf(out, size, "%s · %s", L("USB 网卡", "USB Ethernet"), name);
}

void uci_get(const char *option, char *out, size_t size)
{
    char command[160];
    snprintf(command, sizeof command, "uci -q get %s", option);
    *out = 0;
    FILE *f = popen(command, "r");
    if (!f)
        return;
    if (!fgets(out, (int)size, f))
        *out = 0;
    pclose(f);
    out[strcspn(out, "\n")] = 0;
}

void screen_option(const char *name, char *out, size_t size)
{
    char option[96];
    snprintf(option, sizeof option, "e5-infoscreen.main.%s", name);
    uci_get(option, out, size);
}

void screen_option_set(const char *name, const char *value)
{
    char command[160];
    snprintf(command, sizeof command, "uci set e5-infoscreen.main.%s=%s && uci commit e5-infoscreen", name, value);
    system(command);
}
