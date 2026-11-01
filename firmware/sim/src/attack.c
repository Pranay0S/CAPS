#include "sim/attack.h"

#include <string.h>
#include <strings.h>

static const char *const NAMES[ATK_COUNT] = {
    "NONE", "SPIKE", "DRIFT", "REPLAY", "FREEZE", "DROPOUT", "NOISE", "TAMPER"
};

const char *attack_name(attack_type_t t)
{
    return (t >= 0 && t < ATK_COUNT) ? NAMES[t] : "?";
}

bool attack_parse(const char *s, attack_type_t *out)
{
    for (int i = 0; i < ATK_COUNT; ++i) {
        if (strcasecmp(s, NAMES[i]) == 0) {
            *out = (attack_type_t)i;
            return true;
        }
    }
    return false;
}

float attack_default_magnitude(attack_type_t t)
{
    switch (t) {
    case ATK_SPIKE:  return 60.0f;  /* ml/h added to flow */
    case ATK_DRIFT:  return 0.4f;   /* ml/h of bias added per sample */
    case ATK_REPLAY: return 50.0f;  /* window length, samples */
    case ATK_NOISE:  return 25.0f;  /* ml/h sigma */
    default:         return 0.0f;
    }
}

int32_t attack_default_duration(attack_type_t t)
{
    return t == ATK_SPIKE ? 6 : -1;
}

void attack_init(attack_t *a, uint32_t seed)
{
    memset(a, 0, sizeof(*a));
    rng_seed(&a->rng, seed);
}

static uint16_t hist_index(const attack_t *a, uint16_t age)
{
    /* age 0 = most recent */
    return (uint16_t)((a->hist_head + ATK_HISTORY - 1u - age) % ATK_HISTORY);
}

bool attack_start(attack_t *a, attack_type_t type, float magnitude, int32_t duration)
{
    if (type <= ATK_NONE || type >= ATK_COUNT)
        return false;
    if (magnitude <= 0.0f)
        magnitude = attack_default_magnitude(type);
    if (duration == 0)
        duration = attack_default_duration(type);

    if (type == ATK_REPLAY) {
        uint16_t window = (uint16_t)(magnitude < 5.0f ? 5.0f : magnitude > ATK_HISTORY ? ATK_HISTORY : magnitude);
        if (a->hist_count < window)
            return false;
        for (uint16_t i = 0; i < window; ++i) {
            uint16_t src = hist_index(a, (uint16_t)(window - 1u - i)); /* oldest first */
            a->rep_flow[i]  = a->hist_flow[src];
            a->rep_press[i] = a->hist_press[src];
        }
        a->rep_len = window;
        a->rep_idx = 0;
        magnitude  = window;
    }

    if (type == ATK_FREEZE) {
        if (a->hist_count == 0)
            return false;
        uint16_t last   = hist_index(a, 0);
        a->frozen_flow  = a->hist_flow[last].value;
        a->frozen_press = a->hist_press[last].value;
    }

    a->type         = type;
    a->magnitude    = magnitude;
    a->remaining    = duration;
    a->drift_offset = 0.0f;
    return true;
}

void attack_stop(attack_t *a)
{
    a->type         = ATK_NONE;
    a->remaining    = 0;
    a->drift_offset = 0.0f;
}

void attack_apply(attack_t *a, sensor_frame_t *flow, bool *flow_ok,
                  sensor_frame_t *press, bool *press_ok)
{
    /* The adversary eavesdrops on everything genuine first. */
    if (*flow_ok && *press_ok) {
        a->hist_flow[a->hist_head]  = *flow;
        a->hist_press[a->hist_head] = *press;
        a->hist_head = (uint16_t)((a->hist_head + 1u) % ATK_HISTORY);
        if (a->hist_count < ATK_HISTORY)
            a->hist_count++;
    }

    switch (a->type) {
    case ATK_SPIKE:
        flow->value  += a->magnitude * rng_range(&a->rng, 0.8f, 1.2f);
        press->value += a->magnitude * 0.3f * rng_range(&a->rng, 0.8f, 1.2f);
        break;
    case ATK_DRIFT:
        a->drift_offset += a->magnitude;
        flow->value  += a->drift_offset;
        press->value += a->drift_offset * 0.15f;
        break;
    case ATK_REPLAY:
        *flow  = a->rep_flow[a->rep_idx];
        *press = a->rep_press[a->rep_idx];
        a->rep_idx = (uint16_t)((a->rep_idx + 1u) % a->rep_len);
        break;
    case ATK_FREEZE:
        flow->value  = a->frozen_flow;
        press->value = a->frozen_press;
        break;
    case ATK_DROPOUT:
        *flow_ok  = false;
        *press_ok = false;
        break;
    case ATK_NOISE:
        flow->value  += a->magnitude * rng_gauss(&a->rng);
        press->value += a->magnitude * 0.1f * rng_gauss(&a->rng);
        if (flow->value < 0.0f)
            flow->value = 0.0f;
        break;
    case ATK_NONE:
    case ATK_TAMPER:
    case ATK_COUNT:
        return;
    }

    if (a->remaining > 0 && --a->remaining == 0)
        attack_stop(a);
}
