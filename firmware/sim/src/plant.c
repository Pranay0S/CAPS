#include "sim/plant.h"

#include <string.h>

void plant_init(plant_t *pl, uint32_t seed)
{
    memset(pl, 0, sizeof(*pl));
    pl->pressure = PLANT_BASE_KPA;
    rng_seed(&pl->rng, seed);
}

void plant_set_occlusion(plant_t *pl, bool on)
{
    pl->occl_fault = on;
}

void plant_step(plant_t *pl, float duty, float dt_s)
{
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;

    float occl_step = dt_s / PLANT_OCCL_RAMP_S;
    if (pl->occl_fault)
        pl->occlusion = pl->occlusion + occl_step > 1.0f ? 1.0f : pl->occlusion + occl_step;
    else
        pl->occlusion = pl->occlusion - occl_step < 0.0f ? 0.0f : pl->occlusion - occl_step;

    float demand = duty * PLANT_MAX_FLOW * (1.0f - 0.95f * pl->occlusion);
    float alpha  = dt_s / (PLANT_TAU_S + dt_s);
    pl->flow    += alpha * (demand - pl->flow);
    if (pl->flow < 0.0f)
        pl->flow = 0.0f;

    pl->pressure = PLANT_BASE_KPA + PLANT_KPA_PER_MLH * pl->flow +
                   PLANT_OCCL_KPA * pl->occlusion * (duty > 0.0f ? 1.0f : 0.5f);
    pl->volume  += pl->flow * dt_s / 3600.0f;
}

void plant_sample(plant_t *pl, sensor_frame_t *flow, sensor_frame_t *press)
{
    pl->seq++;
    float f = pl->flow + PLANT_FLOW_NOISE * rng_gauss(&pl->rng);
    flow->value  = f < 0.0f ? 0.0f : f;
    flow->seq    = pl->seq;
    press->value = pl->pressure + PLANT_PRESS_NOISE * rng_gauss(&pl->rng);
    press->seq   = pl->seq;
}
