/*
 * plant.h - virtual physics of the pump, IV line and smart sensors.
 *
 * Motor duty drives true flow through a first-order lag. Line pressure rises
 * with flow and with a downstream occlusion. Sensors report noisy samples
 * stamped with a rolling sequence counter.
 */
#ifndef SIM_PLANT_H
#define SIM_PLANT_H

#include <stdbool.h>
#include <stdint.h>

#include "fw/hal.h"
#include "sim/rng.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PLANT_MAX_FLOW       1200.0f  /* ml/h at 100% duty (matches firmware model) */
#define PLANT_TAU_S          0.4f     /* motor + line time constant */
#define PLANT_BASE_KPA       8.0f
#define PLANT_KPA_PER_MLH    0.02f
#define PLANT_OCCL_KPA       60.0f    /* added pressure at full occlusion */
#define PLANT_OCCL_RAMP_S    3.0f     /* time for occlusion to fully develop */
#define PLANT_FLOW_NOISE     0.8f     /* sensor sigma, ml/h */
#define PLANT_PRESS_NOISE    0.3f     /* sensor sigma, kPa */

typedef struct {
    float    flow;        /* true delivered flow, ml/h */
    float    pressure;    /* true line pressure, kPa */
    float    volume;      /* true delivered volume, ml */
    float    occlusion;   /* 0 = clear, 1 = fully occluded */
    bool     occl_fault;
    uint16_t seq;
    rng_t    rng;
} plant_t;

void plant_init(plant_t *pl, uint32_t seed);
void plant_set_occlusion(plant_t *pl, bool on);
void plant_step(plant_t *pl, float duty, float dt_s);
void plant_sample(plant_t *pl, sensor_frame_t *flow, sensor_frame_t *press);

#ifdef __cplusplus
}
#endif

#endif /* SIM_PLANT_H */
