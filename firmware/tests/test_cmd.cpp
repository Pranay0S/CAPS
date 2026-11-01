#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "fw/cmd.h"
#include "mock_hal.hpp"

using ::testing::HasSubstr;
using ::testing::NiceMock;

namespace {

class CmdTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        sensors.Wire(mock);
        hal = mock.MakeHal();
        logger_init(&lg, nullptr, nullptr, LOG_INFO);
        pump_init(&p, &hal, &lg);
    }

    cmd_status_t Run(const char *line) { return cmd_execute(&p, line, &r); }

    NiceMock<MockHal> mock;
    FakeSensors       sensors;
    hal_t             hal;
    logger_t          lg;
    pump_t            p;
    cmd_result_t      r;
};

TEST(CmdParseFloat, AcceptsOnlyCompleteFiniteNumbers)
{
    float v;
    EXPECT_TRUE(cmd_parse_float("12.5", &v));
    EXPECT_FLOAT_EQ(v, 12.5f);
    EXPECT_FALSE(cmd_parse_float("12abc", &v));
    EXPECT_FALSE(cmd_parse_float("", &v));
    EXPECT_FALSE(cmd_parse_float(nullptr, &v));
    EXPECT_FALSE(cmd_parse_float("nan", &v));
    EXPECT_FALSE(cmd_parse_float("inf", &v));
}

TEST_F(CmdTest, RateCommandUpdatesSetpoint)
{
    EXPECT_EQ(Run("rate 250"), CMD_OK);
    EXPECT_STREQ(r.verb, "rate");
    EXPECT_FLOAT_EQ(p.target_rate, 250.0f);
}

TEST_F(CmdTest, VerbIsCaseInsensitive)
{
    EXPECT_EQ(Run("START"), CMD_OK);
    EXPECT_EQ(p.state, PUMP_RUNNING);
}

TEST_F(CmdTest, BadArgumentsAreErrorsNotUnknown)
{
    EXPECT_EQ(Run("rate fast"), CMD_ERROR);
    EXPECT_EQ(Run("rate 5000"), CMD_ERROR);
    EXPECT_THAT(r.msg, HasSubstr("out of range"));
    EXPECT_EQ(Run("start now"), CMD_ERROR);
    EXPECT_EQ(Run("ids maybe"), CMD_ERROR);
}

TEST_F(CmdTest, UnknownVerbIsReportedAsUnknown)
{
    EXPECT_EQ(Run("attack spike"), CMD_UNKNOWN);
    EXPECT_EQ(Run("   "), CMD_UNKNOWN);
}

TEST_F(CmdTest, IdsToggle)
{
    EXPECT_EQ(Run("ids on"), CMD_OK);
    EXPECT_TRUE(p.ids_enforce);
    EXPECT_EQ(Run("ids off"), CMD_OK);
    EXPECT_FALSE(p.ids_enforce);
}

TEST_F(CmdTest, StatusReportsStateAndRate)
{
    Run("rate 42");
    EXPECT_EQ(Run("status"), CMD_OK);
    EXPECT_THAT(r.msg, HasSubstr("state=IDLE"));
    EXPECT_THAT(r.msg, HasSubstr("rate=42.00"));
}

TEST_F(CmdTest, LogLevelChangesLoggerFilter)
{
    EXPECT_EQ(Run("loglevel warn"), CMD_OK);
    EXPECT_EQ(lg.min_level, LOG_WARN);
}

} // namespace
