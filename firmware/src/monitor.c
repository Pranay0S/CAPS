#include "fw/monitor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fw/fw_config.h"

void monitor_init(monitor_t *m)
{
    memset(m, 0, sizeof(*m));
}

void monitor_settle(monitor_t *m)
{
    m->settle         = MON_SETTLE_SAMPLES;
    m->mismatch_count = 0;
    m->stuck_count    = 0;
}

/* A frame is fresh if its counter moved forward. Forward gaps (missed frames)
 * are fine; zero or backward movement means stale or replayed data. */
static bool seq_fresh(bool *have, uint16_t *last, uint16_t seq)
{
    if (!*have) {
        *have = true;
        *last = seq;
        return true;
    }
    uint16_t d = (uint16_t)(seq - *last);
    if (d == 0 || d > 0x8000u)
        return false;
    *last = seq;
    return true;
}

uint32_t monitor_check(monitor_t *m,
                       const sensor_frame_t *flow, const sensor_frame_t *press,
                       const monitor_ctx_t *ctx,
                       bool *flow_stale, bool *press_stale)
{
    uint32_t flags = ANOM_NONE;
    bool f_stale = false, p_stale = false;
    bool settling = m->settle > 0;

    if (!flow || !press)
        flags |= ANOM_DROPOUT;

    if (flow && !seq_fresh(&m->have_flow_seq, &m->last_flow_seq, flow->seq)) {
        flags |= ANOM_SEQ;
        f_stale = true;
    }
    if (press && !seq_fresh(&m->have_press_seq, &m->last_press_seq, press->seq)) {
        flags |= ANOM_SEQ;
        p_stale = true;
    }

    if (flow) {
        if (ctx->running && m->have_prev_flow) {
            float roc_limit = MON_ROC_FLOW_BASE + MON_ROC_FLOW_FRAC * ctx->target_rate;
            if (!settling && fabsf(flow->value - m->prev_flow) > roc_limit)
                flags |= ANOM_ROC_FLOW;

            if (flow->value == m->prev_flow) {
                if (m->stuck_count < 255)
                    m->stuck_count++;
            } else {
                m->stuck_count = 0;
            }
            if (m->stuck_count >= MON_STUCK_SAMPLES)
                flags |= ANOM_STUCK;
        } else {
            m->stuck_count = 0;
        }

        if (ctx->running && !settling) {
            float model = ctx->duty * PUMP_NOMINAL_MAX_FLOW;
            float band  = fmaxf(MON_MISMATCH_MIN, MON_MISMATCH_FRAC * ctx->target_rate);
            if (fabsf(flow->value - model) > band) {
                if (m->mismatch_count < 255)
                    m->mismatch_count++;
            } else {
                m->mismatch_count = 0;
            }
            if (m->mismatch_count >= MON_MISMATCH_SAMPLES)
                flags |= ANOM_MISMATCH;
        } else {
            m->mismatch_count = 0;
        }

        if (!f_stale) {
            m->prev_flow      = flow->value;
            m->have_prev_flow = true;
        }
    }

    if (press) {
        if (ctx->running && !settling && m->have_prev_press &&
            fabsf(press->value - m->prev_press) > MON_ROC_PRESS_KPA)
            flags |= ANOM_ROC_PRESS;
        if (!p_stale) {
            m->prev_press      = press->value;
            m->have_prev_press = true;
        }
    }

    if (m->settle > 0)
        m->settle--;

    if (flow_stale)
        *flow_stale = f_stale;
    if (press_stale)
        *press_stale = p_stale;
    return flags;
}

const char *monitor_flags_str(uint32_t flags, char *buf, size_t len)
{
    static const struct { uint32_t bit; const char *name; } NAMES[] = {
        { ANOM_ROC_FLOW,  "ROC_FLOW"  },
        { ANOM_ROC_PRESS, "ROC_PRESS" },
        { ANOM_STUCK,     "STUCK"     },
        { ANOM_SEQ,       "SEQ"       },
        { ANOM_MISMATCH,  "MISMATCH"  },
        { ANOM_DROPOUT,   "DROPOUT"   },
    };

    if (len == 0)
        return buf;
    buf[0] = '\0';
    if (flags == ANOM_NONE) {
        snprintf(buf, len, "NONE");
        return buf;
    }
    size_t used = 0;
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; ++i) {
        if (!(flags & NAMES[i].bit))
            continue;
        int n = snprintf(buf + used, len - used, "%s%s", used ? "|" : "", NAMES[i].name);
        if (n < 0 || (size_t)n >= len - used)
            break;
        used += (size_t)n;
    }
    return buf;
}
