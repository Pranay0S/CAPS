/*
 * monitor.h - sensor plausibility monitor.
 *
 * A deliberately simple, rule-based on-device intrusion/fault detector. It
 * looks at each sensor frame and returns a bitmask of anomalies. The pump
 * decides what to do with them (log only, or alarm when IDS enforcement is on).
 */
#ifndef FW_MONITOR_H
#define FW_MONITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw/hal.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    ANOM_NONE       = 0,
    ANOM_ROC_FLOW   = 1u << 0,  /* flow changed faster than physically plausible */
    ANOM_ROC_PRESS  = 1u << 1,  /* pressure changed faster than plausible */
    ANOM_STUCK      = 1u << 2,  /* flow reading frozen while motor is running */
    ANOM_SEQ        = 1u << 3,  /* stale / replayed frame counter */
    ANOM_MISMATCH   = 1u << 4,  /* measured flow disagrees with motor model */
    ANOM_DROPOUT    = 1u << 5   /* sensor did not answer */
};

/* Anomalies that indicate the sensor data cannot be trusted. */
#define ANOM_TRUST_MASK (ANOM_ROC_FLOW | ANOM_ROC_PRESS | ANOM_STUCK | ANOM_SEQ | ANOM_MISMATCH)

typedef struct {
    bool     running;        /* motor commanded on */
    float    target_rate;    /* ml/h */
    float    duty;           /* duty applied during the last period */
} monitor_ctx_t;

typedef struct {
    bool     have_flow_seq, have_press_seq;
    uint16_t last_flow_seq, last_press_seq;
    bool     have_prev_flow, have_prev_press;
    float    prev_flow, prev_press;
    uint8_t  stuck_count;
    uint8_t  mismatch_count;
    uint8_t  settle;
} monitor_t;

void     monitor_init(monitor_t *m);

/* Call when the setpoint or run state changes so transients aren't flagged. */
void     monitor_settle(monitor_t *m);

/* flow / press may be NULL when the read failed. A frame flagged ANOM_SEQ is
 * reported via *flow_stale / *press_stale (either may be NULL). */
uint32_t monitor_check(monitor_t *m,
                       const sensor_frame_t *flow, const sensor_frame_t *press,
                       const monitor_ctx_t *ctx,
                       bool *flow_stale, bool *press_stale);

/* Writes "ROC_FLOW|SEQ" style text, or "NONE". Returns buf. */
const char *monitor_flags_str(uint32_t flags, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* FW_MONITOR_H */
