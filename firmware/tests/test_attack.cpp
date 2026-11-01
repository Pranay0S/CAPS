#include <gtest/gtest.h>

#include "sim/attack.h"

namespace {

class AttackTest : public ::testing::Test {
protected:
    void SetUp() override { attack_init(&a, 7); }

    // Push one genuine frame through the attacker; returns what the firmware sees.
    void Apply(float flow = 100.0f, float press = 10.0f)
    {
        ++seq;
        f = { flow, seq };
        p = { press, seq };
        f_ok = p_ok = true;
        attack_apply(&a, &f, &f_ok, &p, &p_ok);
    }

    void Warmup(int n)
    {
        for (int i = 0; i < n; ++i)
            Apply(100.0f + i);
    }

    attack_t       a;
    sensor_frame_t f{}, p{};
    bool           f_ok = true, p_ok = true;
    uint16_t       seq = 0;
};

TEST_F(AttackTest, NoAttackPassesFramesThrough)
{
    Apply(123.0f, 11.0f);
    EXPECT_FLOAT_EQ(f.value, 123.0f);
    EXPECT_FLOAT_EQ(p.value, 11.0f);
    EXPECT_TRUE(f_ok && p_ok);
}

TEST_F(AttackTest, SpikeLastsRequestedSamplesThenEnds)
{
    ASSERT_TRUE(attack_start(&a, ATK_SPIKE, 50.0f, 3));
    for (int i = 0; i < 3; ++i) {
        Apply();
        EXPECT_GE(f.value, 100.0f + 0.8f * 50.0f);
        EXPECT_LE(f.value, 100.0f + 1.2f * 50.0f);
    }
    EXPECT_EQ(a.type, ATK_NONE);
    Apply();
    EXPECT_FLOAT_EQ(f.value, 100.0f);
}

TEST_F(AttackTest, DefaultsApplyWhenParametersOmitted)
{
    ASSERT_TRUE(attack_start(&a, ATK_SPIKE, 0.0f, 0));
    EXPECT_FLOAT_EQ(a.magnitude, attack_default_magnitude(ATK_SPIKE));
    EXPECT_EQ(a.remaining, attack_default_duration(ATK_SPIKE));
}

TEST_F(AttackTest, DriftBiasGrowsEachSample)
{
    ASSERT_TRUE(attack_start(&a, ATK_DRIFT, 0.5f, -1));
    Apply();
    EXPECT_FLOAT_EQ(f.value, 100.5f);
    Apply();
    EXPECT_FLOAT_EQ(f.value, 101.0f);
    Apply();
    EXPECT_FLOAT_EQ(f.value, 101.5f);
}

TEST_F(AttackTest, ReplayNeedsRecordedHistory)
{
    Warmup(3);
    EXPECT_FALSE(attack_start(&a, ATK_REPLAY, 10.0f, -1));
    EXPECT_EQ(a.type, ATK_NONE);
}

TEST_F(AttackTest, ReplayLoopsOldFramesIncludingTheirCounters)
{
    Warmup(20); // values 100..119, seq 1..20
    ASSERT_TRUE(attack_start(&a, ATK_REPLAY, 5.0f, -1));
    for (int loop = 0; loop < 2; ++loop) {
        for (int i = 0; i < 5; ++i) {
            Apply(999.0f);
            EXPECT_FLOAT_EQ(f.value, 115.0f + i);
            EXPECT_EQ(f.seq, 16 + i);
        }
    }
}

TEST_F(AttackTest, FreezeHoldsValueButCounterAdvances)
{
    Warmup(5); // last genuine value 104
    ASSERT_TRUE(attack_start(&a, ATK_FREEZE, 0.0f, -1));
    Apply(150.0f);
    EXPECT_FLOAT_EQ(f.value, 104.0f);
    uint16_t s1 = f.seq;
    Apply(160.0f);
    EXPECT_FLOAT_EQ(f.value, 104.0f);
    EXPECT_EQ(f.seq, s1 + 1);
}

TEST_F(AttackTest, DropoutSwallowsFrames)
{
    ASSERT_TRUE(attack_start(&a, ATK_DROPOUT, 0.0f, 2));
    Apply();
    EXPECT_FALSE(f_ok);
    EXPECT_FALSE(p_ok);
    Apply();
    Apply();
    EXPECT_TRUE(f_ok);
}

TEST_F(AttackTest, StopRestoresPassThrough)
{
    attack_start(&a, ATK_DRIFT, 1.0f, -1);
    Apply();
    attack_stop(&a);
    Apply(100.0f);
    EXPECT_FLOAT_EQ(f.value, 100.0f);
}

TEST(AttackNames, RoundTrip)
{
    for (int i = 0; i < ATK_COUNT; ++i) {
        attack_type_t t;
        ASSERT_TRUE(attack_parse(attack_name(static_cast<attack_type_t>(i)), &t));
        EXPECT_EQ(t, i);
    }
    attack_type_t t;
    EXPECT_TRUE(attack_parse("spike", &t));
    EXPECT_FALSE(attack_parse("laser", &t));
}

} // namespace
