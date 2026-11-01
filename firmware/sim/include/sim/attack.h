/*
 * attack.h - adversary model sitting on the sensor bus between the plant's
 * sensors and the firmware (a man-in-the-middle on e.g. I2C/CAN/UART).
 *
 *   SPIKE    add a large transient to flow & pressure for N samples
 *   DRIFT    add a slowly growing bias (stealthy)
 *   REPLAY   loop a recorded window of genuine frames (including old seq)
 *   FREEZE   hold the reading constant, keep the counter moving
 *   DROPOUT  swallow frames entirely (jamming / DoS)
 *   NOISE    add heavy Gaussian noise
 *   TAMPER   label only - the harness writes the setpoint in RAM directly
 */
#ifndef SIM_ATTACK_H
#define SIM_ATTACK_H

#include <stdbool.h>
#include <stdint.h>

#include "fw/hal.h"
#include "sim/rng.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATK_HISTORY 128

typedef enum {
    ATK_NONE = 0,
    ATK_SPIKE,
    ATK_DRIFT,
    ATK_REPLAY,
    ATK_FREEZE,
    ATK_DROPOUT,
    ATK_NOISE,
    ATK_TAMPER,
    ATK_COUNT
} attack_type_t;

typedef struct {
    attack_type_t  type;
    int32_t        remaining;     /* samples left; -1 = until cleared */
    float          magnitude;     /* meaning depends on type */
    float          drift_offset;

    sensor_frame_t hist_flow[ATK_HISTORY];
    sensor_frame_t hist_press[ATK_HISTORY];
    uint16_t       hist_head;     /* next write index */
    uint16_t       hist_count;

    sensor_frame_t rep_flow[ATK_HISTORY];
    sensor_frame_t rep_press[ATK_HISTORY];
    uint16_t       rep_len;
    uint16_t       rep_idx;

    float          frozen_flow, frozen_press;

    rng_t          rng;
} attack_t;

void          attack_init(attack_t *a, uint32_t seed);

/* magnitude <= 0 or duration == 0 select the per-type default.
 * Returns false (and stays idle) if the attack cannot start, e.g. REPLAY
 * without enough recorded history. */
bool          attack_start(attack_t *a, attack_type_t type, float magnitude, int32_t duration);
void          attack_stop(attack_t *a);

/* Record genuine frames, then tamper with them in place. On DROPOUT the
 * *_ok flags are cleared. Advances the attack's duration counter. */
void          attack_apply(attack_t *a, sensor_frame_t *flow, bool *flow_ok,
                           sensor_frame_t *press, bool *press_ok);

const char   *attack_name(attack_type_t t);
bool          attack_parse(const char *s, attack_type_t *out);
float         attack_default_magnitude(attack_type_t t);
int32_t       attack_default_duration(attack_type_t t);

#ifdef __cplusplus
}
#endif

#endif /* SIM_ATTACK_H */
