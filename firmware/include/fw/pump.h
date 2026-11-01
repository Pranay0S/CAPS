/*
 * pump.h - IV pump controller: state machine, closed-loop flow control,
 * safety alarms and volume accounting.
 *
 *            start               VTBI reached
 *   IDLE ───────────► RUNNING ───────────────► COMPLETE
 *    ▲   ◄─────────── │  ▲  │
 *    │      stop      │  │  │ pause / start
 *    │                ▼  │  ▼
 *    │   clear      ALARM  PAUSED
 *    └────────────────┘
 */
#ifndef FW_PUMP_H
#define FW_PUMP_H

#include <stdbool.h>
#include <stdint.h>

#include "fw/hal.h"
#include "fw/logger.h"
#include "fw/monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PUMP_IDLE = 0,
    PUMP_RUNNING,
    PUMP_PAUSED,
    PUMP_ALARM,
    PUMP_COMPLETE
} pump_state_t;

typedef enum {
    ALARM_NONE = 0,
    ALARM_OCCLUSION,       /* downstream pressure above limit */
    ALARM_SENSOR_TIMEOUT,  /* sensors stopped answering */
    ALARM_SENSOR_FAULT,    /* monitor flagged untrusted data (IDS enforce) */
    ALARM_MEM_CORRUPT      /* setpoint failed its redundancy check */
} pump_alarm_t;

typedef enum {
    PUMP_OK = 0,
    PUMP_ERR_RANGE,
    PUMP_ERR_STATE
} pump_err_t;

typedef struct {
    const hal_t  *hal;
    logger_t     *log;

    pump_state_t  state;
    pump_alarm_t  alarm;

    /* Setpoint stored twice (value + bitwise complement) - a common safety
     * firmware pattern for catching RAM corruption or tampering. */
    float         target_rate;
    uint32_t      target_rate_chk;

    float         vtbi;          /* ml, 0 = unlimited */
    float         volume_est;    /* ml, integrated from measured flow */

    float         duty;
    float         integ;

    float         flow_meas;     /* last accepted readings */
    float         pressure_meas;

    uint32_t      last_ms;
    bool          have_last_ms;
    uint8_t       missed_reads;

    bool          ids_enforce;   /* true: monitor anomalies raise ALARM_SENSOR_FAULT */
    monitor_t     mon;
    uint32_t      anomalies;     /* flags from the most recent step */
} pump_t;

void         pump_init(pump_t *p, const hal_t *hal, logger_t *log);

pump_err_t   pump_start(pump_t *p);
pump_err_t   pump_pause(pump_t *p);
pump_err_t   pump_stop(pump_t *p);
pump_err_t   pump_set_rate(pump_t *p, float rate_mlh);
pump_err_t   pump_set_vtbi(pump_t *p, float vtbi_ml);
pump_err_t   pump_clear_alarm(pump_t *p);
void         pump_set_ids(pump_t *p, bool enforce);

/* One control-loop iteration. Call at a fixed period. */
void         pump_step(pump_t *p);

const char  *pump_state_name(pump_state_t s);
const char  *pump_alarm_name(pump_alarm_t a);

#ifdef __cplusplus
}
#endif

#endif /* FW_PUMP_H */
