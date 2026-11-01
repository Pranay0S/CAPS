#include "fw/cmd.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool cmd_parse_float(const char *s, float *out)
{
    if (!s || !*s)
        return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || !isfinite(v))
        return false;
    *out = (float)v;
    return true;
}

static void result(cmd_result_t *out, cmd_status_t st, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void result(cmd_result_t *out, cmd_status_t st, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->msg, sizeof out->msg, fmt, ap);
    va_end(ap);
    out->status = st;
}

static bool is_fw_verb(const char *verb)
{
    static const char *const VERBS[] = {
        "start", "pause", "stop", "clear", "status", "rate", "vtbi", "ids", "loglevel"
    };
    for (size_t i = 0; i < sizeof VERBS / sizeof VERBS[0]; ++i)
        if (strcmp(verb, VERBS[i]) == 0)
            return true;
    return false;
}

static const char *err_text(pump_err_t e)
{
    switch (e) {
    case PUMP_OK:        return "ok";
    case PUMP_ERR_RANGE: return "value out of range";
    case PUMP_ERR_STATE: return "not allowed in current state";
    }
    return "error";
}

static cmd_status_t from_pump(cmd_result_t *out, pump_t *p, pump_err_t e)
{
    if (e == PUMP_OK)
        result(out, CMD_OK, "state=%s", pump_state_name(p->state));
    else
        result(out, CMD_ERROR, "%s (state=%s)", err_text(e), pump_state_name(p->state));
    return out->status;
}

cmd_status_t cmd_execute(pump_t *p, const char *line, cmd_result_t *out)
{
    memset(out, 0, sizeof(*out));

    char buf[CMD_MSG_MAX];
    snprintf(buf, sizeof buf, "%s", line);

    char *save = NULL;
    char *verb = strtok_r(buf, " \t\r\n", &save);
    char *arg  = strtok_r(NULL, " \t\r\n", &save);
    char *extra = strtok_r(NULL, " \t\r\n", &save);

    if (!verb) {
        result(out, CMD_UNKNOWN, "empty command");
        return out->status;
    }
    for (char *c = verb; *c; ++c)
        *c = (char)tolower((unsigned char)*c);
    snprintf(out->verb, sizeof out->verb, "%s", verb);

    if (!is_fw_verb(verb)) {
        result(out, CMD_UNKNOWN, "unknown command '%s'", verb);
        return out->status;
    }
    bool takes_arg = strcmp(verb, "rate") == 0 || strcmp(verb, "vtbi") == 0 ||
                     strcmp(verb, "ids") == 0 || strcmp(verb, "loglevel") == 0;
    if (extra || (arg && !takes_arg)) {
        result(out, CMD_ERROR, "too many arguments");
        return out->status;
    }

    if (strcmp(verb, "start") == 0)
        return from_pump(out, p, pump_start(p));
    if (strcmp(verb, "pause") == 0)
        return from_pump(out, p, pump_pause(p));
    if (strcmp(verb, "stop") == 0)
        return from_pump(out, p, pump_stop(p));
    if (strcmp(verb, "clear") == 0)
        return from_pump(out, p, pump_clear_alarm(p));

    if (strcmp(verb, "rate") == 0 || strcmp(verb, "vtbi") == 0) {
        float v;
        if (!cmd_parse_float(arg, &v)) {
            result(out, CMD_ERROR, "usage: %s <number>", verb);
            return out->status;
        }
        pump_err_t e = (verb[0] == 'r') ? pump_set_rate(p, v) : pump_set_vtbi(p, v);
        return from_pump(out, p, e);
    }

    if (strcmp(verb, "ids") == 0) {
        if (arg && strcasecmp(arg, "on") == 0)
            pump_set_ids(p, true);
        else if (arg && strcasecmp(arg, "off") == 0)
            pump_set_ids(p, false);
        else {
            result(out, CMD_ERROR, "usage: ids on|off");
            return out->status;
        }
        result(out, CMD_OK, "ids=%s", p->ids_enforce ? "on" : "off");
        return out->status;
    }

    if (strcmp(verb, "loglevel") == 0) {
        log_level_t lvl;
        if (!arg || !logger_parse_level(arg, &lvl)) {
            result(out, CMD_ERROR, "usage: loglevel debug|info|warn|error");
            return out->status;
        }
        logger_set_level(p->log, lvl);
        result(out, CMD_OK, "loglevel=%s", logger_level_name(lvl));
        return out->status;
    }

    if (strcmp(verb, "status") == 0) {
        result(out, CMD_OK, "state=%s alarm=%s rate=%.2f vtbi=%.2f vol=%.3f duty=%.3f ids=%s",
               pump_state_name(p->state), pump_alarm_name(p->alarm), (double)p->target_rate,
               (double)p->vtbi, (double)p->volume_est, (double)p->duty, p->ids_enforce ? "on" : "off");
        return out->status;
    }

    return out->status; /* unreachable: verb table and handlers match */
}
