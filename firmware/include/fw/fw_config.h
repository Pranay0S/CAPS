/*
 * fw_config.h - compile-time tuning for the virtual IV pump firmware.
 *
 * Units used throughout the firmware:
 *   flow      ml/h
 *   pressure  kPa (line pressure downstream of the pump)
 *   volume    ml
 *   time      ms (ticks), s (dt)
 */
#ifndef FW_CONFIG_H
#define FW_CONFIG_H

/* ---- Programmable limits ---------------------------------------------- */
#define PUMP_RATE_MIN_MLH        0.1f
#define PUMP_RATE_MAX_MLH        999.0f
#define PUMP_VTBI_MAX_ML         9999.0f   /* 0 = no volume limit */

/* ---- Motor / control loop --------------------------------------------- */
#define PUMP_NOMINAL_MAX_FLOW    1200.0f   /* ml/h delivered at 100% duty */
#define PUMP_KP                  0.00025f  /* duty per (ml/h) error */
#define PUMP_KI                  0.0005f   /* duty per (ml/h * s) error */
#define PUMP_INTEG_LIMIT         400.0f    /* anti-windup clamp, ml/h * s */

/* ---- Safety thresholds ------------------------------------------------ */
#define PUMP_OCCLUSION_KPA       40.0f     /* downstream occlusion alarm */
#define PUMP_MAX_MISSED_READS    5         /* consecutive -> SENSOR_TIMEOUT */

/* ---- Sensor plausibility monitor (lightweight on-device IDS) ---------- */
#define MON_SETTLE_SAMPLES       20        /* ignore ROC/mismatch after setpoint change */
#define MON_ROC_FLOW_BASE        15.0f     /* ml/h per sample */
#define MON_ROC_FLOW_FRAC        0.20f     /* + fraction of target */
#define MON_ROC_PRESS_KPA        5.0f      /* kPa per sample */
#define MON_STUCK_SAMPLES        8         /* identical readings while running */
#define MON_MISMATCH_FRAC        0.25f     /* |meas - model| / target */
#define MON_MISMATCH_MIN         10.0f     /* ml/h floor for the band above */
#define MON_MISMATCH_SAMPLES     15        /* persistence before flagging */

/* ---- Logger ----------------------------------------------------------- */
#define LOG_LINE_MAX             192
#define LOG_RING_LINES           64

#endif /* FW_CONFIG_H */
