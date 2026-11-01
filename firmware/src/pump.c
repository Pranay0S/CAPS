#include "fw/pump.h"

#include <string.h>

#include "fw/fw_config.h"

#define MOD "PUMP"

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t float_bits(float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static void store_rate(pump_t *p, float rate)
{
    p->target_rate     = rate;
    p->target_rate_chk = ~float_bits(rate);
}

static bool rate_intact(const pump_t *p)
{
    return p->target_rate_chk == ~float_bits(p->target_rate);
}

static uint32_t now_ms(const pump_t *p)
{
    return p->hal->millis(p->hal->ctx);
}

static void motor_off(pump_t *p)
{
    p->duty  = 0.0f;
    p->integ = 0.0f;
}

static void raise_alarm(pump_t *p, pump_alarm_t alarm, uint32_t now)
{
    if (p->state == PUMP_ALARM && p->alarm == alarm)
        return;
    p->state = PUMP_ALARM;
    p->alarm = alarm;
    motor_off(p);
    p->hal->set_alarm_output(p->hal->ctx, true);
    logger_event(p->log, now, LOG_ERROR, MOD, "ALARM %s - motor stopped", pump_alarm_name(alarm));
}

static void log_new_anomalies(pump_t *p, uint32_t flags, uint32_t now)
{
    uint32_t rising = flags & ~p->anomalies;
    if (!rising)
        return;
    char buf[96];
    monitor_flags_str(rising, buf, sizeof buf);
    logger_event(p->log, now, LOG_WARN, "MON", "anomaly %s (flow=%.2f press=%.2f)",
                 buf, (double)p->flow_meas, (double)p->pressure_meas);
}

void pump_init(pump_t *p, const hal_t *hal, logger_t *log)
{
    memset(p, 0, sizeof(*p));
    p->hal = hal;
    p->log = log;
    store_rate(p, 100.0f);
    monitor_init(&p->mon);
    hal->set_motor_duty(hal->ctx, 0.0f);
    hal->set_alarm_output(hal->ctx, false);
}

pump_err_t pump_start(pump_t *p)
{
    if (p->state == PUMP_RUNNING)
        return PUMP_OK;
    if (p->state == PUMP_ALARM || p->state == PUMP_COMPLETE)
        return PUMP_ERR_STATE;
    p->state = PUMP_RUNNING;
    p->integ = 0.0f;
    monitor_settle(&p->mon);
    logger_event(p->log, now_ms(p), LOG_INFO, MOD, "infusion started at %.2f ml/h", (double)p->target_rate);
    return PUMP_OK;
}

pump_err_t pump_pause(pump_t *p)
{
    if (p->state != PUMP_RUNNING)
        return PUMP_ERR_STATE;
    p->state = PUMP_PAUSED;
    motor_off(p);
    logger_event(p->log, now_ms(p), LOG_INFO, MOD, "infusion paused at %.3f ml", (double)p->volume_est);
    return PUMP_OK;
}

pump_err_t pump_stop(pump_t *p)
{
    if (p->state == PUMP_ALARM)
        return PUMP_ERR_STATE;
    p->state = PUMP_IDLE;
    motor_off(p);
    logger_event(p->log, now_ms(p), LOG_INFO, MOD, "infusion stopped at %.3f ml", (double)p->volume_est);
    return PUMP_OK;
}

pump_err_t pump_set_rate(pump_t *p, float rate_mlh)
{
    if (!(rate_mlh >= PUMP_RATE_MIN_MLH && rate_mlh <= PUMP_RATE_MAX_MLH)) {
        logger_event(p->log, now_ms(p), LOG_WARN, MOD, "rejected rate %.2f ml/h (limits %.1f-%.1f)",
                     (double)rate_mlh, (double)PUMP_RATE_MIN_MLH, (double)PUMP_RATE_MAX_MLH);
        return PUMP_ERR_RANGE;
    }
    store_rate(p, rate_mlh);
    monitor_settle(&p->mon);
    logger_event(p->log, now_ms(p), LOG_INFO, MOD, "rate set to %.2f ml/h", (double)rate_mlh);
    return PUMP_OK;
}

pump_err_t pump_set_vtbi(pump_t *p, float vtbi_ml)
{
    if (!(vtbi_ml >= 0.0f && vtbi_ml <= PUMP_VTBI_MAX_ML))
        return PUMP_ERR_RANGE;
    if (p->state == PUMP_RUNNING || p->state == PUMP_ALARM)
        return PUMP_ERR_STATE;
    p->vtbi       = vtbi_ml;
    p->volume_est = 0.0f;
    if (p->state == PUMP_COMPLETE)
        p->state = PUMP_IDLE;
    logger_event(p->log, now_ms(p), LOG_INFO, MOD, "VTBI set to %.2f ml (volume counter reset)", (double)vtbi_ml);
    return PUMP_OK;
}

pump_err_t pump_clear_alarm(pump_t *p)
{
    if (p->state != PUMP_ALARM)
        return PUMP_ERR_STATE;
    uint32_t now = now_ms(p);
    if (p->alarm == ALARM_MEM_CORRUPT) {
        store_rate(p, PUMP_RATE_MIN_MLH);
        logger_event(p->log, now, LOG_WARN, MOD, "setpoint reset to %.1f ml/h - reprogram rate",
                     (double)PUMP_RATE_MIN_MLH);
    }
    logger_event(p->log, now, LOG_INFO, MOD, "alarm %s cleared", pump_alarm_name(p->alarm));
    p->state        = PUMP_IDLE;
    p->alarm        = ALARM_NONE;
    p->missed_reads = 0;
    monitor_settle(&p->mon);
    p->hal->set_alarm_output(p->hal->ctx, false);
    return PUMP_OK;
}

void pump_set_ids(pump_t *p, bool enforce)
{
    p->ids_enforce = enforce;
    logger_event(p->log, now_ms(p), LOG_INFO, MOD, "IDS %s", enforce ? "enforcing (alarms)" : "monitor-only");
}

void pump_step(pump_t *p)
{
    const hal_t *h = p->hal;
    uint32_t now = h->millis(h->ctx);
    float dt = p->have_last_ms ? (float)(uint32_t)(now - p->last_ms) / 1000.0f : 0.0f;
    p->last_ms      = now;
    p->have_last_ms = true;

    if (!rate_intact(p))
        raise_alarm(p, ALARM_MEM_CORRUPT, now);

    sensor_frame_t ff, pf;
    bool f_ok = h->read_flow(h->ctx, &ff) == HAL_OK;
    bool p_ok = h->read_pressure(h->ctx, &pf) == HAL_OK;

    monitor_ctx_t mc = { p->state == PUMP_RUNNING, p->target_rate, p->duty };
    uint32_t flags = monitor_check(&p->mon, f_ok ? &ff : NULL, p_ok ? &pf : NULL, &mc, NULL, NULL);
    if (f_ok)
        p->flow_meas = ff.value;
    if (p_ok)
        p->pressure_meas = pf.value;
    log_new_anomalies(p, flags, now);
    p->anomalies = flags;

    if (f_ok && p_ok)
        p->missed_reads = 0;
    else if (p->missed_reads < 255)
        p->missed_reads++;

    if (p->state == PUMP_RUNNING) {
        if (p->missed_reads >= PUMP_MAX_MISSED_READS)
            raise_alarm(p, ALARM_SENSOR_TIMEOUT, now);
        else if (p_ok && pf.value > PUMP_OCCLUSION_KPA)
            raise_alarm(p, ALARM_OCCLUSION, now);
        else if (p->ids_enforce && (flags & ANOM_TRUST_MASK))
            raise_alarm(p, ALARM_SENSOR_FAULT, now);
    }

    if (p->state == PUMP_RUNNING) {
        if (f_ok) {
            float e  = p->target_rate - p->flow_meas;
            p->integ = clampf(p->integ + e * dt, -PUMP_INTEG_LIMIT, PUMP_INTEG_LIMIT);
            float ff_duty = p->target_rate / PUMP_NOMINAL_MAX_FLOW;
            p->duty  = clampf(ff_duty + PUMP_KP * e + PUMP_KI * p->integ, 0.0f, 1.0f);
        }
        /* On a missed read the last duty is held and volume uses the last good flow. */
        p->volume_est += clampf(p->flow_meas, 0.0f, PUMP_NOMINAL_MAX_FLOW) * dt / 3600.0f;

        if (p->vtbi > 0.0f && p->volume_est >= p->vtbi) {
            p->state = PUMP_COMPLETE;
            motor_off(p);
            logger_event(p->log, now, LOG_INFO, MOD, "VTBI complete: %.3f ml delivered", (double)p->volume_est);
        }
    } else {
        p->duty = 0.0f;
    }

    h->set_motor_duty(h->ctx, p->duty);
}

const char *pump_state_name(pump_state_t s)
{
    switch (s) {
    case PUMP_IDLE:     return "IDLE";
    case PUMP_RUNNING:  return "RUNNING";
    case PUMP_PAUSED:   return "PAUSED";
    case PUMP_ALARM:    return "ALARM";
    case PUMP_COMPLETE: return "COMPLETE";
    }
    return "?";
}

const char *pump_alarm_name(pump_alarm_t a)
{
    switch (a) {
    case ALARM_NONE:           return "NONE";
    case ALARM_OCCLUSION:      return "OCCLUSION";
    case ALARM_SENSOR_TIMEOUT: return "SENSOR_TIMEOUT";
    case ALARM_SENSOR_FAULT:   return "SENSOR_FAULT";
    case ALARM_MEM_CORRUPT:    return "MEM_CORRUPT";
    }
    return "?";
}
