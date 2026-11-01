// Closed-loop tests: real firmware + virtual plant + attacker, no mocks.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include "sim/sim.h"

using ::testing::HasSubstr;
using ::testing::StartsWith;

namespace {

std::vector<std::string> Split(const std::string &s, char sep = ',')
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, sep))
        out.push_back(item);
    return out;
}

class SimTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        sim_config_t cfg{ 100, 1234, &SimTest::Sink, this };
        sim_init(&s, &cfg);
    }

    static void Sink(void *ctx, const char *line) { static_cast<SimTest *>(ctx)->lines.emplace_back(line); }

    void Cmd(const std::string &c) { sim_command(&s, c.c_str()); }
    void Run(int ticks)
    {
        for (int i = 0; i < ticks; ++i)
            sim_tick(&s);
    }

    std::string LastOf(const std::string &prefix) const
    {
        for (auto it = lines.rbegin(); it != lines.rend(); ++it)
            if (it->rfind(prefix, 0) == 0)
                return *it;
        return {};
    }

    bool AnyTlmAnomaly(const std::string &name, size_t from = 0) const
    {
        for (size_t i = from; i < lines.size(); ++i)
            if (lines[i].rfind("TLM,", 0) == 0 && Split(lines[i])[13].find(name) != std::string::npos)
                return true;
        return false;
    }

    void StartAndSettle(const char *rate = "100")
    {
        Cmd(std::string("rate ") + rate);
        Cmd("start");
        Run(50);
    }

    sim_t                    s;
    std::vector<std::string> lines;
};

TEST_F(SimTest, BootIsLogged)
{
    ASSERT_FALSE(lines.empty());
    EXPECT_THAT(lines[0], StartsWith("LOG,0,INFO,SIM,boot"));
}

TEST_F(SimTest, TelemetryRecordMatchesHeader)
{
    Run(1);
    auto cols = Split(SIM_TLM_HEADER);
    auto tlm  = Split(LastOf("TLM,"));
    ASSERT_EQ(tlm.size(), cols.size() + 1) << "type prefix + one field per column";
    EXPECT_EQ(tlm[1], "100");
    EXPECT_EQ(tlm[2], "IDLE");
}

TEST_F(SimTest, CommandsAreAcknowledged)
{
    Cmd("rate 80");
    EXPECT_THAT(LastOf("ACK,"), HasSubstr(",rate,"));
    Cmd("rate -1");
    EXPECT_THAT(LastOf("ERR,"), HasSubstr(",rate,"));
    Cmd("frobnicate");
    EXPECT_THAT(LastOf("ERR,"), HasSubstr("unknown command"));
}

TEST_F(SimTest, NormalInfusionTracksTargetAndVolume)
{
    StartAndSettle("200");
    size_t from = lines.size();
    Run(300); // 30 s
    EXPECT_NEAR(s.plant.flow, 200.0f, 5.0f);
    EXPECT_NEAR(s.pump.volume_est, s.plant.volume, 0.02f);
    EXPECT_EQ(s.pump.state, PUMP_RUNNING);
    EXPECT_FALSE(AnyTlmAnomaly("ROC", from));
    EXPECT_FALSE(AnyTlmAnomaly("MISMATCH", from));
    EXPECT_FALSE(AnyTlmAnomaly("STUCK", from));
    EXPECT_FALSE(AnyTlmAnomaly("SEQ", from));
}

TEST_F(SimTest, SpikeAttackIsFlaggedAndLabelled)
{
    StartAndSettle();
    size_t from = lines.size();
    Cmd("attack spike");
    Run(1);
    EXPECT_EQ(Split(LastOf("TLM,"))[14], "SPIKE");
    Run(10);
    EXPECT_TRUE(AnyTlmAnomaly("ROC_FLOW", from));
    EXPECT_EQ(s.attack.type, ATK_NONE) << "spike is self-limiting";
}

TEST_F(SimTest, SpikeMakesFirmwareUnderDeliver)
{
    StartAndSettle();
    Cmd("attack spike 200 20");
    float min_true = 1e9f;
    for (int i = 0; i < 20; ++i) {
        sim_tick(&s);
        min_true = std::min(min_true, s.plant.flow);
    }
    EXPECT_LT(min_true, 60.0f) << "spoofed high flow drives real delivery down";
}

TEST_F(SimTest, ReplayIsCaughtBySequenceCheck)
{
    StartAndSettle();
    size_t from = lines.size();
    Cmd("attack replay 30");
    Run(5);
    EXPECT_TRUE(AnyTlmAnomaly("SEQ", from));
}

TEST_F(SimTest, FreezeIsCaughtAsStuckSensor)
{
    StartAndSettle();
    size_t from = lines.size();
    Cmd("attack freeze");
    Run(15);
    EXPECT_TRUE(AnyTlmAnomaly("STUCK", from));
}

TEST_F(SimTest, DriftWithIdsEnforcedEventuallyAlarms)
{
    Cmd("ids on");
    StartAndSettle();
    Cmd("attack drift 1.0");
    Run(200);
    EXPECT_EQ(s.pump.state, PUMP_ALARM);
    EXPECT_EQ(s.pump.alarm, ALARM_SENSOR_FAULT);
}

TEST_F(SimTest, DropoutTriggersSensorTimeout)
{
    StartAndSettle();
    Cmd("attack dropout");
    Run(10);
    EXPECT_EQ(s.pump.alarm, ALARM_SENSOR_TIMEOUT);
    EXPECT_FLOAT_EQ(s.motor_duty, 0.0f);
}

TEST_F(SimTest, OcclusionFaultRaisesAlarmAndStopsFlow)
{
    StartAndSettle();
    Cmd("fault occlusion on");
    Run(60);
    EXPECT_EQ(s.pump.alarm, ALARM_OCCLUSION);
    EXPECT_TRUE(s.alarm_output);
    Run(20);
    EXPECT_LT(s.plant.flow, 5.0f);
}

TEST_F(SimTest, TamperIsCaughtByRedundantSetpoint)
{
    StartAndSettle();
    Cmd("attack tamper 900");
    EXPECT_THAT(LastOf("ACK,"), HasSubstr("overwritten"));
    Run(1);
    EXPECT_EQ(s.pump.alarm, ALARM_MEM_CORRUPT);
    EXPECT_EQ(Split(LastOf("TLM,"))[14], "TAMPER");
}

TEST_F(SimTest, AttackStartAndEndAreLoggedAsGroundTruth)
{
    StartAndSettle();
    Cmd("attack drift");
    EXPECT_THAT(LastOf("LOG,"), HasSubstr("SIM,attack DRIFT started"));
    Cmd("attack clear");
    EXPECT_THAT(LastOf("LOG,"), HasSubstr("SIM,attack DRIFT ended"));
}

TEST_F(SimTest, SameSeedGivesIdenticalRun)
{
    StartAndSettle();
    Run(20);
    std::vector<std::string> first = lines;

    lines.clear();
    sim_config_t cfg{ 100, 1234, &SimTest::Sink, this };
    sim_init(&s, &cfg);
    StartAndSettle();
    Run(20);
    EXPECT_EQ(first, lines);
}

TEST_F(SimTest, QuitSetsFlag)
{
    Cmd("quit");
    EXPECT_TRUE(s.quit);
}

} // namespace
