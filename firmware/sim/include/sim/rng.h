/* rng.h - small deterministic PRNG so simulation runs are reproducible by seed. */
#ifndef SIM_RNG_H
#define SIM_RNG_H

#include <math.h>
#include <stdint.h>

typedef struct {
    uint32_t state;
} rng_t;

static inline void rng_seed(rng_t *r, uint32_t seed)
{
    r->state = seed ? seed : 0x9E3779B9u;
}

/* xorshift32 */
static inline uint32_t rng_next(rng_t *r)
{
    uint32_t x = r->state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->state = x;
    return x;
}

/* Uniform in [0, 1). */
static inline float rng_uniform(rng_t *r)
{
    return (float)(rng_next(r) >> 8) * (1.0f / 16777216.0f);
}

static inline float rng_range(rng_t *r, float lo, float hi)
{
    return lo + (hi - lo) * rng_uniform(r);
}

/* Standard normal via Box-Muller. */
static inline float rng_gauss(rng_t *r)
{
    float u1 = rng_uniform(r);
    float u2 = rng_uniform(r);
    if (u1 < 1e-7f)
        u1 = 1e-7f;
    return sqrtf(-2.0f * logf(u1)) * cosf(6.28318530718f * u2);
}

#endif /* SIM_RNG_H */
