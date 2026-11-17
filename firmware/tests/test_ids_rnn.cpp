// IDS-RNN: C inference must reproduce the Python-trained int8 model.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "fw/ids_rnn.h"
#include "ids_golden.h"

static_assert(IDS_MODEL_BYTES <= 6144, "IDS model exceeds its 6 kB flash budget");

namespace {

ids_input_t Running(float flow, uint16_t seq)
{
    ids_input_t in{};
    in.running     = true;
    in.target_rate = 100.0f;
    in.flow        = flow;
    in.flow_seq    = seq;
    in.flow_ok     = true;
    in.pressure    = 10.0f + 0.01f * flow;
    in.press_seq   = seq;
    in.press_ok    = true;
    in.duty        = 100.0f / 1200.0f;
    return in;
}

}  // namespace

TEST(IdsRnnTest, FeaturesMatchPython)
{
    ids_feat_t f{};
    float x[IDS_N_FEATURES];
    for (int t = 0; t < IDS_GOLDEN_N; ++t) {
        ids_features(&f, &IDS_GOLDEN[t].in, x);
        for (int i = 0; i < IDS_N_FEATURES; ++i)
            ASSERT_NEAR(x[i], IDS_GOLDEN[t].feat[i], 1e-4f + 1e-4f * std::fabs(IDS_GOLDEN[t].feat[i]))
                << "row " << t << " feature " << i;
    }
}

TEST(IdsRnnTest, ProbabilitiesMatchPythonInt8Model)
{
    ids_rnn_t d;
    ids_rnn_init(&d);
    int above = 0;
    for (int t = 0; t < IDS_GOLDEN_N; ++t) {
        ids_rnn_step(&d, &IDS_GOLDEN[t].in);
        float sum = 0.0f;
        for (int c = 0; c < IDS_N_CLASSES; ++c) {
            ASSERT_NEAR(d.prob[c], IDS_GOLDEN[t].prob[c], 2e-3f) << "row " << t << " class " << ids_class_name((ids_class_t)c);
            sum += d.prob[c];
        }
        ASSERT_NEAR(sum, 1.0f, 1e-4f);

        float att = 1.0f - IDS_GOLDEN[t].prob[IDS_CLASS_NONE] - IDS_GOLDEN[t].prob[IDS_CLASS_OCCLUSION];
        if (std::fabs(att - IDS_THRESHOLD) < 5e-3f)
            continue;  // too close to call; rounding may legitimately flip it
        above = att > IDS_THRESHOLD ? above + 1 : 0;
        EXPECT_EQ(d.alert, above >= IDS_DEBOUNCE) << "row " << t;
    }
}

TEST(IdsRnnTest, ReplayedFrameCounterIsStale)
{
    ids_feat_t f{};
    float x[IDS_N_FEATURES];
    for (uint16_t s = 100; s < 110; ++s) {
        ids_input_t in = Running(100.0f + s, s);
        ids_features(&f, &in, x);
        EXPECT_EQ(x[8], 0.0f);
    }
    ids_input_t old = Running(100.0f, 60);  // a frame from the past
    ids_features(&f, &old, x);
    EXPECT_EQ(x[8], 1.0f);
}

TEST(IdsRnnTest, CounterWrapIsFresh)
{
    ids_feat_t f{};
    float x[IDS_N_FEATURES];
    ids_input_t a = Running(100.0f, 0xFFFF);
    ids_input_t b = Running(101.0f, 0x0000);
    ids_features(&f, &a, x);
    ids_features(&f, &b, x);
    EXPECT_EQ(x[8], 0.0f);
    EXPECT_EQ(x[9], 0.0f);
}

TEST(IdsRnnTest, DropoutSetsMissingAndKeepsSeqState)
{
    ids_feat_t f{};
    float x[IDS_N_FEATURES];
    ids_input_t in = Running(100.0f, 10);
    ids_features(&f, &in, x);
    in.flow_ok = in.press_ok = false;
    ids_features(&f, &in, x);
    EXPECT_EQ(x[10], 1.0f);
    EXPECT_EQ(x[8], 0.0f);  // a failed read is not a stale frame
    in       = Running(100.0f, 30);  // 19 frames were swallowed
    ids_features(&f, &in, x);
    EXPECT_EQ(x[10], 0.0f);
    EXPECT_EQ(x[9], 1.0f);
}

TEST(IdsRnnTest, FrozenReadingsAreFlaggedAsIdentical)
{
    ids_feat_t f{};
    float x[IDS_N_FEATURES];
    ids_input_t in = Running(97.25f, 1);
    ids_features(&f, &in, x);
    in.flow_seq = in.press_seq = 2;  // counter moves, value does not
    ids_features(&f, &in, x);
    EXPECT_EQ(x[6], 1.0f);
    EXPECT_EQ(x[7], 1.0f);
}

TEST(IdsRnnTest, ExpMatchesLibm)
{
    double worst = 0.0;
    for (int i = 0; i < 174990; ++i) {
        float x = -87.0f + 0.001f * (float)i;   // -87 .. 87.99 (clamped at 88)
        double ref = std::exp((double)x);
        worst = std::max(worst, std::fabs((double)ids_expf(x) - ref) / ref);
    }
    EXPECT_LT(worst, 2e-6);
    EXPECT_EQ(ids_expf(-1000.0f), 0.0f);
    EXPECT_TRUE(std::isfinite(ids_expf(1000.0f)));   // clamped, never inf
    EXPECT_FLOAT_EQ(ids_expf(0.0f), 1.0f);
}

TEST(IdsRnnTest, LogMatchesLibm)
{
    double worst = 0.0;
    for (float x = 1.0f; x < 2000.0f; x *= 1.0007f) {   // 1 + target covers 1 .. 1000
        double ref = std::log((double)x);
        worst = std::max(worst, std::fabs((double)ids_logf(x) - ref));
    }
    EXPECT_LT(worst, 2e-6);
    EXPECT_FLOAT_EQ(ids_logf(1.0f), 0.0f);
}

TEST(IdsRnnTest, StateIsSmall)
{
    EXPECT_LE(sizeof(ids_rnn_t), 256u);
}
