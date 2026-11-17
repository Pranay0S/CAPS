#include "fw/ids_rnn.h"

#include "fw/fw_config.h"

/* Feature constants - must match ml/common.py. */
#define F_CLIP          8.0f
#define LINE_BASE_KPA   8.0f
#define LINE_KPA_PER_ML 0.02f

/* ---- Minimal math ---------------------------------------------------------
 * The detector needs exp (sigmoid, tanh, softmax) and one log (a feature).
 * newlib's tanhf + expf + log1pf cost ~2.2 kB of flash on Cortex-M4F; these
 * two routines cost a small fraction of that and are accurate to ~1e-6
 * relative (the float32 limit is ~6e-8), far inside the 2e-3 tolerance to
 * which the C model is checked against Python. No libm, no errno. */

typedef union {
    float    f;
    uint32_t u;
} f32_bits_t;

/* e^x: x = k*ln2 + r with |r| <= ln2/2, e^r by a degree-6 Taylor polynomial
 * (truncation error < 1.3e-7), then scaled by 2^k through the exponent bits. */
float ids_expf(float x)
{
    if (x < -87.0f)
        return 0.0f;
    if (!(x < 88.0f))
        x = 88.0f;   /* also catches NaN; e^88 still fits in a float */
    float kf = x * 1.44269504f;
    int   k  = (int)(kf + (kf < 0.0f ? -0.5f : 0.5f));
    /* ln2 split in two parts so k*ln2 is subtracted without rounding loss */
    float r = (x - (float)k * 0.693359375f) + (float)k * 2.12194440e-4f;
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (1.66666667e-1f + r * (4.16666667e-2f +
              r * (8.33333333e-3f + r * 1.38888889e-3f)))));
    f32_bits_t s;
    s.u = (uint32_t)(k + 127) << 23;   /* 2^k, k in [-126, 127] */
    return p * s.f;
}

/* ln(x) for normal positive x: x = 2^e * m with m in [sqrt(1/2), sqrt(2)),
 * ln(m) = 2*atanh(s), s = (m-1)/(m+1), |s| <= 0.172, series to s^7
 * (truncation error < 1e-7). */
float ids_logf(float x)
{
    f32_bits_t v;
    v.f = x;
    int e = (int)(v.u >> 23) - 127;
    v.u   = (v.u & 0x007FFFFFu) | 0x3F800000u;   /* mantissa as a float in [1, 2) */
    if (v.f > 1.41421356f) {
        v.f *= 0.5f;
        e++;
    }
    float s  = (v.f - 1.0f) / (v.f + 1.0f);
    float s2 = s * s;
    return (float)e * 0.693147181f +
           2.0f * s * (1.0f + s2 * (3.33333333e-1f + s2 * (0.2f + s2 * 1.42857143e-1f)));
}

static float clip8(float v)
{
    return v > F_CLIP ? F_CLIP : (v < -F_CLIP ? -F_CLIP : v);
}

static float sigmoidf(float a)
{
    return 1.0f / (1.0f + ids_expf(-a));
}

/* tanh(a) = 2*sigmoid(2a) - 1 */
static float tanh_(float a)
{
    return 2.0f / (1.0f + ids_expf(-2.0f * a)) - 1.0f;
}

/* Updates a frame-counter high-water mark. Returns true if the frame is stale
 * (counter did not move forward); *step gets the forward distance. */
static bool seq_update(bool *have, uint16_t *hw, uint16_t seq, uint16_t *step)
{
    if (!*have) {
        *have = true;
        *hw   = seq;
        *step = 1;
        return false;
    }
    uint16_t d = (uint16_t)(seq - *hw);
    if (d == 0 || d >= 0x8000u) {
        *step = 0;
        return true;
    }
    *hw   = seq;
    *step = d;
    return false;
}

void ids_features(ids_feat_t *f, const ids_input_t *in, float out[IDS_N_FEATURES])
{
    float run = in->running ? 1.0f : 0.0f;
    float s   = MON_ROC_FLOW_FRAC * in->target_rate + MON_ROC_FLOW_BASE;

    bool     stale = false;
    uint16_t gap = 0, d;
    if (in->flow_ok) {
        stale |= seq_update(&f->have_fseq, &f->fseq_hw, in->flow_seq, &d);
        if (d > gap) gap = d;
    }
    if (in->press_ok) {
        stale |= seq_update(&f->have_pseq, &f->pseq_hw, in->press_seq, &d);
        if (d > gap) gap = d;
    }
    float seq_gap = gap > 1 ? (float)(gap - 1) / 8.0f : 0.0f;
    if (seq_gap > 1.0f)
        seq_gap = 1.0f;

    float d_flow = 0.0f, d_press = 0.0f, f_same = 0.0f, p_same = 0.0f, d_tgt = 0.0f;
    if (f->have_prev) {
        d_flow  = clip8((in->flow - f->flow_prev) / s);
        d_press = clip8((in->pressure - f->press_prev) / 2.0f);
        f_same  = (in->flow_ok && in->flow == f->flow_prev) ? 1.0f : 0.0f;
        p_same  = (in->press_ok && in->pressure == f->press_prev) ? 1.0f : 0.0f;
        d_tgt   = in->target_rate != f->target_prev ? 1.0f : 0.0f;
    }

    out[0]  = run;
    out[1]  = clip8((in->flow - in->target_rate * run) / s);
    out[2]  = clip8((in->flow - f->duty_prev * PUMP_NOMINAL_MAX_FLOW) / s);
    out[3]  = d_flow;
    out[4]  = clip8((in->pressure - LINE_BASE_KPA - LINE_KPA_PER_ML * in->flow) / 5.0f);
    out[5]  = d_press;
    out[6]  = f_same;
    out[7]  = p_same;
    out[8]  = stale ? 1.0f : 0.0f;
    out[9]  = seq_gap;
    out[10] = ((in->flow_ok ? 0.0f : 1.0f) + (in->press_ok ? 0.0f : 1.0f)) * 0.5f;
    out[11] = in->alarm ? 1.0f : 0.0f;
    out[12] = in->mem_alarm ? 1.0f : 0.0f;
    out[13] = ids_logf(1.0f + (in->target_rate > 0.0f ? in->target_rate : 0.0f)) / 7.0f;
    out[14] = d_tgt;

    f->have_prev   = true;
    f->flow_prev   = in->flow;
    f->press_prev  = in->pressure;
    f->duty_prev   = in->duty;
    f->target_prev = in->target_rate;
}

void ids_rnn_init(ids_rnn_t *d)
{
    /* Byte loop instead of memset: keeps the detector free of libc. */
    volatile uint8_t *b = (volatile uint8_t *)d;
    for (unsigned i = 0; i < sizeof(*d); ++i)
        b[i] = 0;
    d->cls        = IDS_CLASS_NONE;
    d->attack_cls = IDS_CLASS_NONE;
}

/* y[i] = scale * sum_j W[row0 + i][j] * x[j]  for i in [0, rows) */
static void matvec_q(const int8_t *w, int rows, int cols, float scale, const float *x, float *y)
{
    for (int i = 0; i < rows; ++i) {
        const int8_t *row = w + i * cols;
        float acc = 0.0f;
        for (int j = 0; j < cols; ++j)
            acc += (float)row[j] * x[j];
        y[i] = acc * scale;
    }
}

ids_class_t ids_rnn_step(ids_rnn_t *d, const ids_input_t *in)
{
    enum { H = IDS_HIDDEN, F = IDS_N_FEATURES, C = IDS_N_CLASSES };
    float x[F], xi[3 * H], hh[3 * H];

    ids_features(&d->feat, in, x);

    for (int g = 0; g < 3; ++g) {
        matvec_q(IDS_WI + g * H * F, H, F, IDS_WI_SCALE[g], x, xi + g * H);
        matvec_q(IDS_WH + g * H * H, H, H, IDS_WH_SCALE[g], d->h, hh + g * H);
    }
    for (int i = 0; i < H; ++i) {
        float r = sigmoidf(xi[i] + IDS_BI[i] + hh[i]);
        float z = sigmoidf(xi[H + i] + IDS_BI[H + i] + hh[H + i]);
        float n = tanh_(xi[2 * H + i] + IDS_BI[2 * H + i] + r * (hh[2 * H + i] + IDS_BHN[i]));
        d->h[i] = (1.0f - z) * n + z * d->h[i];
    }

    float logits[C], mx = 0.0f;
    matvec_q(IDS_WO, C, H, IDS_WO_SCALE, d->h, logits);
    for (int c = 0; c < C; ++c) {
        logits[c] += IDS_BO[c];
        if (c == 0 || logits[c] > mx)
            mx = logits[c];
    }
    float sum = 0.0f;
    for (int c = 0; c < C; ++c) {
        d->prob[c] = ids_expf(logits[c] - mx);
        sum += d->prob[c];
    }
    int best = 0, best_att = -1;
    for (int c = 0; c < C; ++c) {
        d->prob[c] /= sum;
        if (d->prob[c] > d->prob[best])
            best = c;
        if (c != IDS_CLASS_NONE && c != IDS_CLASS_OCCLUSION && (best_att < 0 || d->prob[c] > d->prob[best_att]))
            best_att = c;
    }
    d->cls         = (ids_class_t)best;
    d->attack_cls  = (ids_class_t)best_att;
    d->attack_prob = 1.0f - d->prob[IDS_CLASS_NONE] - d->prob[IDS_CLASS_OCCLUSION];

    if (d->attack_prob > IDS_THRESHOLD) {
        if (d->above < 255)
            d->above++;
    } else {
        d->above = 0;
    }
    d->alert = d->above >= IDS_DEBOUNCE;
    return d->cls;
}

const char *ids_class_name(ids_class_t c)
{
    static const char *const NAMES[IDS_N_CLASSES] = IDS_CLASS_NAMES;
    return (unsigned)c < IDS_N_CLASSES ? NAMES[c] : "?";
}
