/*
 * hal.h - hardware abstraction layer for the IV pump firmware.
 *
 * The firmware never touches hardware directly; every peripheral access goes
 * through this table of function pointers. On a real board these would wrap
 * ADC/UART/PWM drivers. In this repo they are provided either by the virtual
 * plant (sim/) or by a GMock object (tests/).
 */
#ifndef FW_HAL_H
#define FW_HAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_OK = 0,
    HAL_TIMEOUT,   /* sensor did not answer this cycle */
    HAL_ERROR
} hal_status_t;

/* One reading as delivered by a smart sensor over the sensor bus.
 * `seq` is a rolling frame counter stamped by the sensor; the firmware uses
 * it to reject stale or replayed frames. */
typedef struct {
    float    value;
    uint16_t seq;
} sensor_frame_t;

typedef struct {
    hal_status_t (*read_flow)(void *ctx, sensor_frame_t *out);
    hal_status_t (*read_pressure)(void *ctx, sensor_frame_t *out);
    void         (*set_motor_duty)(void *ctx, float duty);   /* 0.0 .. 1.0 */
    void         (*set_alarm_output)(void *ctx, bool on);    /* buzzer/LED */
    uint32_t     (*millis)(void *ctx);
    void         *ctx;
} hal_t;

#ifdef __cplusplus
}
#endif

#endif /* FW_HAL_H */
