#include <gtest/gtest.h>

#include "fw/fw_config.h"
#include "fw/monitor.h"

namespace {

class MonitorTest : public ::testing::Test {
protected:
    void SetUp() override { monitor_init(&m); }

    // Feed one consistent sample; flow matches the motor model for ctx.duty.
    uint32_t Feed(float flow, float press = 10.0f, int seq_step = 1)
    {
        seq = static_cast<uint16_t>(seq + seq_step);
        sensor_frame_t f{ flow, seq }, p{ press, seq };
        return monitor_check(&m, &f, &p, &ctx, &f_stale, &p_stale);
    }

    void FeedSteady(int n)
    {
        for (int i = 0; i < n; ++i)
            ASSERT_EQ(Feed(100.0f + (i % 2 ? 0.5f : -0.5f)), ANOM_NONE) << "sample " << i;
    }

    monitor_t     m;
    monitor_ctx_t ctx{ true, 100.0f, 100.0f / PUMP_NOMINAL_MAX_FLOW };
    uint16_t      seq = 0;
    bool          f_stale = false, p_stale = false;
};

TEST_F(MonitorTest, CleanSignalRaisesNothing)
{
    FeedSteady(50);
}

TEST_F(MonitorTest, MissingFrameIsDropout)
{
    sensor_frame_t p{ 10.0f, 1 };
    EXPECT_EQ(monitor_check(&m, nullptr, &p, &ctx, nullptr, nullptr), static_cast<uint32_t>(ANOM_DROPOUT));
}

TEST_F(MonitorTest, ReplayedSequenceIsFlaggedAndMarkedStale)
{
    FeedSteady(10);
    EXPECT_TRUE(Feed(100.0f, 10.0f, -5) & ANOM_SEQ);
    EXPECT_TRUE(f_stale);
    EXPECT_TRUE(p_stale);
}

TEST_F(MonitorTest, RepeatedSequenceIsFlagged)
{
    FeedSteady(3);
    EXPECT_TRUE(Feed(100.0f, 10.0f, 0) & ANOM_SEQ);
}

TEST_F(MonitorTest, ForwardGapAfterMissedFramesIsAccepted)
{
    FeedSteady(5);
    EXPECT_FALSE(Feed(100.0f, 10.0f, 4) & ANOM_SEQ);
}

TEST_F(MonitorTest, FlowSpikeIsRateOfChangeAnomaly)
{
    FeedSteady(25);
    EXPECT_TRUE(Feed(170.0f) & ANOM_ROC_FLOW);
}

TEST_F(MonitorTest, PressureSpikeIsRateOfChangeAnomaly)
{
    FeedSteady(25);
    EXPECT_TRUE(Feed(100.0f, 25.0f) & ANOM_ROC_PRESS);
}

TEST_F(MonitorTest, SettleWindowSuppressesTransientAfterSetpointChange)
{
    FeedSteady(25);
    monitor_settle(&m);
    EXPECT_FALSE(Feed(170.0f) & ANOM_ROC_FLOW);
}

TEST_F(MonitorTest, FrozenReadingWhileRunningIsStuck)
{
    FeedSteady(25);
    uint32_t flags = 0;
    for (int i = 0; i <= MON_STUCK_SAMPLES; ++i)
        flags = Feed(101.25f);
    EXPECT_TRUE(flags & ANOM_STUCK);
}

TEST_F(MonitorTest, ConstantReadingWhileIdleIsNotStuck)
{
    ctx.running = false;
    for (int i = 0; i < 3 * MON_STUCK_SAMPLES; ++i)
        EXPECT_EQ(Feed(0.0f), ANOM_NONE);
}

TEST_F(MonitorTest, PersistentModelMismatchIsFlaggedOnlyAfterPersistence)
{
    FeedSteady(25);
    // 40 ml/h above what the motor duty can produce - but no single big step.
    float v = 100.0f;
    uint32_t flags = 0;
    int first_flag = -1;
    for (int i = 0; i < 40; ++i) {
        v = std::min(v + 5.0f, 140.0f);
        flags = Feed(v);
        if ((flags & ANOM_MISMATCH) && first_flag < 0)
            first_flag = i;
    }
    ASSERT_GE(first_flag, 0);
    EXPECT_GE(first_flag, MON_MISMATCH_SAMPLES - 1);
    EXPECT_FALSE(flags & ANOM_ROC_FLOW);
}

TEST(MonitorFlags, FormatsAsPipeSeparatedNames)
{
    char buf[64];
    EXPECT_STREQ(monitor_flags_str(ANOM_NONE, buf, sizeof buf), "NONE");
    EXPECT_STREQ(monitor_flags_str(ANOM_ROC_FLOW | ANOM_SEQ, buf, sizeof buf), "ROC_FLOW|SEQ");
}

} // namespace
