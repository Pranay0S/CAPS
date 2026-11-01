#include "sim/sim.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "fw/cmd.h"
#include "fw/monitor.h"

#define MOD "SIM"

/* ---- HAL implementation backed by the virtual plant ------------------- */

static hal_status_t hal_read_flow(void *ctx, sensor_frame_t *out)
{
    sim_t *s = ctx;
    if (!s->bus_flow_ok)
        return HAL_TIMEOUT;
    *out = s->bus_flow;
    return HAL_OK;
}

static hal_status_t hal_read_pressure(void *ctx, sensor_frame_t *out)
{
    sim_t *s = ctx;
    if (!s->bus_press_ok)
        return HAL_TIMEOUT;
    *out = s->bus_press;
    return HAL_OK;
}

static void hal_set_motor_duty(void *ctx, float duty)
{
    ((sim_t *)ctx)->motor_duty = duty;
}

static void hal_set_alarm_output(void *ctx, bool on)
{
    ((sim_t *)ctx)->alarm_output = on;
}

static uint32_t hal_millis(void *ctx)
{
    return ((sim_t *)ctx)->millis;
}

/* ---- helpers ------------------------------------------------------------ */

static void respond(sim_t *s, bool ok, const char *verb, const char *msg)
{
    char v[24], m[LOG_LINE_MAX], line[LOG_LINE_MAX];
    snprintf(v, sizeof v, "%s", verb);
    snprintf(m, sizeof m, "%s", msg);
    logger_sanitize(v);
    logger_sanitize(m);
    snprintf(line, sizeof line, "%s,%lu,%s,%s", ok ? "ACK" : "ERR", (unsigned long)s->millis, v, m);
    logger_raw(&s->log, line);
}

static void emit_telemetry(sim_t *s)
{
    const pump_t *p = &s->pump;
    char anom[64], line[LOG_LINE_MAX];
    monitor_flags_str(p->anomalies, anom, sizeof anom);
    snprintf(line, sizeof line,
             "TLM,%lu,%s,%s,%.2f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.2f,%.4f,%s,%s",
             (unsigned long)s->millis, pump_state_name(p->state), pump_alarm_name(p->alarm),
             (double)p->target_rate, (double)p->flow_meas, (double)s->plant.flow,
             (double)p->pressure_meas, (double)s->plant.pressure,
             (double)p->volume_est, (double)s->plant.volume, (double)p->vtbi,
             (double)p->duty, anom, attack_name(s->attack.type));
    logger_raw(&s->log, line);
}

static void note_attack_transition(sim_t *s)
{
    if (s->attack.type == s->prev_attack)
        return;
    if (s->attack.type == ATK_NONE)
        logger_event(&s->log, s->millis, LOG_INFO, MOD, "attack %s ended", attack_name(s->prev_attack));
    else
        logger_event(&s->log, s->millis, LOG_INFO, MOD, "attack %s started (magnitude=%.2f samples=%ld)",
                     attack_name(s->attack.type), (double)s->attack.magnitude, (long)s->attack.remaining);
    s->prev_attack = s->attack.type;
}

/* ---- public ------------------------------------------------------------- */

void sim_init(sim_t *s, const sim_config_t *cfg)
{
    memset(s, 0, sizeof(*s));
    s->period_ms = cfg->period_ms ? cfg->period_ms : 100;

    logger_init(&s->log, cfg->sink, cfg->sink_ctx, LOG_INFO);
    plant_init(&s->plant, cfg->seed);
    attack_init(&s->attack, cfg->seed * 2654435761u + 1u);

    s->hal = (hal_t){
        .read_flow        = hal_read_flow,
        .read_pressure    = hal_read_pressure,
        .set_motor_duty   = hal_set_motor_duty,
        .set_alarm_output = hal_set_alarm_output,
        .millis           = hal_millis,
        .ctx              = s,
    };
    pump_init(&s->pump, &s->hal, &s->log);

    logger_event(&s->log, 0, LOG_INFO, MOD, "boot: period=%lums seed=%lu",
                 (unsigned long)s->period_ms, (unsigned long)cfg->seed);
    logger_flush(&s->log);
}

void sim_tick(sim_t *s)
{
    float dt = (float)s->period_ms / 1000.0f;

    plant_step(&s->plant, s->motor_duty, dt);
    s->millis += s->period_ms;

    plant_sample(&s->plant, &s->bus_flow, &s->bus_press);
    s->bus_flow_ok = s->bus_press_ok = true;
    attack_apply(&s->attack, &s->bus_flow, &s->bus_flow_ok, &s->bus_press, &s->bus_press_ok);
    note_attack_transition(s);

    pump_step(&s->pump);

    emit_telemetry(s);
    logger_flush(&s->log);
}

static void handle_attack(sim_t *s, char *save)
{
    char *kind = strtok_r(NULL, " \t", &save);
    char *mag  = strtok_r(NULL, " \t", &save);
    char *dur  = strtok_r(NULL, " \t", &save);
    char msg[CMD_MSG_MAX];

    if (!kind) {
        respond(s, false, "attack", "usage: attack <spike|drift|replay|freeze|dropout|noise|tamper|clear> [magnitude] [samples]");
        return;
    }
    if (strcasecmp(kind, "clear") == 0 || strcasecmp(kind, "stop") == 0 || strcasecmp(kind, "none") == 0) {
        attack_stop(&s->attack);
        note_attack_transition(s);
        respond(s, true, "attack", "cleared");
        return;
    }

    attack_type_t type;
    if (!attack_parse(kind, &type) || type == ATK_NONE) {
        snprintf(msg, sizeof msg, "unknown attack '%s'", kind);
        respond(s, false, "attack", msg);
        return;
    }

    float magnitude = 0.0f, duration = 0.0f;
    if ((mag && !cmd_parse_float(mag, &magnitude)) || (dur && !cmd_parse_float(dur, &duration))) {
        respond(s, false, "attack", "magnitude/samples must be numbers");
        return;
    }

    if (type == ATK_TAMPER) {
        if (!mag) {
            respond(s, false, "attack", "usage: attack tamper <rate>");
            return;
        }
        /* Overwrite the setpoint in firmware RAM, bypassing the API and its
         * range check - the redundant copy is left untouched. */
        s->pump.target_rate = magnitude;
        s->attack.type      = ATK_TAMPER;
        s->attack.magnitude = magnitude;
        s->attack.remaining = -1;
        note_attack_transition(s);
        snprintf(msg, sizeof msg, "setpoint RAM overwritten with %.2f", (double)magnitude);
        respond(s, true, "attack", msg);
        return;
    }

    if (!attack_start(&s->attack, type, magnitude, (int32_t)duration)) {
        respond(s, false, "attack", "cannot start (not enough recorded history yet?)");
        return;
    }
    note_attack_transition(s);
    snprintf(msg, sizeof msg, "%s armed", attack_name(type));
    respond(s, true, "attack", msg);
}

static void handle_fault(sim_t *s, char *save)
{
    char *kind = strtok_r(NULL, " \t", &save);
    char *arg  = strtok_r(NULL, " \t", &save);
    if (!kind || strcasecmp(kind, "occlusion") != 0 || !arg ||
        (strcasecmp(arg, "on") != 0 && strcasecmp(arg, "off") != 0)) {
        respond(s, false, "fault", "usage: fault occlusion on|off");
        return;
    }
    bool on = strcasecmp(arg, "on") == 0;
    plant_set_occlusion(&s->plant, on);
    logger_event(&s->log, s->millis, LOG_INFO, MOD, "fault occlusion %s", on ? "injected" : "removed");
    respond(s, true, "fault", on ? "occlusion on" : "occlusion off");
}

void sim_command(sim_t *s, const char *line)
{
    char buf[CMD_MSG_MAX];
    snprintf(buf, sizeof buf, "%s", line);
    buf[strcspn(buf, "\r\n")] = '\0';

    char *save = NULL;
    char tmp[CMD_MSG_MAX];
    snprintf(tmp, sizeof tmp, "%s", buf);
    char *verb = strtok_r(tmp, " \t", &save);
    if (!verb)
        return;
    for (char *c = verb; *c; ++c)
        *c = (char)tolower((unsigned char)*c);

    if (strcmp(verb, "attack") == 0) {
        handle_attack(s, save);
    } else if (strcmp(verb, "fault") == 0) {
        handle_fault(s, save);
    } else if (strcmp(verb, "quit") == 0 || strcmp(verb, "exit") == 0) {
        s->quit = true;
        respond(s, true, "quit", "bye");
    } else if (strcmp(verb, "help") == 0) {
        respond(s, true, "help",
                "start pause stop clear status / rate N / vtbi N / ids on|off / loglevel L / "
                "attack TYPE [mag] [n] / attack clear / fault occlusion on|off / quit");
    } else {
        cmd_result_t r;
        cmd_status_t st = cmd_execute(&s->pump, buf, &r);
        respond(s, st == CMD_OK, r.verb[0] ? r.verb : verb, r.msg);
    }
    logger_flush(&s->log);
}
