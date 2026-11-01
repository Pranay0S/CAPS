#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "fw/fw_config.h"
#include "fw/pump.h"
#include "mock_hal.hpp"

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::FloatNear;
using ::testing::Gt;
using ::testing::NiceMock;

namespace {

class PumpTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        sensors.Wire(mock);
        hal = mock.MakeHal();
        logger_init(&lg, nullptr, nullptr, LOG_DEBUG);
        pump_init(&p, &hal, &lg);
    }

    // Advance one 100 ms period with the flow sensor tracking the motor.
    void Step(int n = 1)
    {
        for (int i = 0; i < n; ++i) {
            sensors.now += 100;
            pump_step(&p);
            logger_flush(&lg);
        }
    }

    NiceMock<MockHal> mock;
    FakeSensors       sensors;
    hal_t             hal;
    logger_t          lg;
    pump_t            p;
};

TEST(PumpInit, DrivesOutputsToSafeState)
{
    NiceMock<MockHal> mock;
    EXPECT_CALL(mock, SetMotorDuty(0.0f));
    EXPECT_CALL(mock, SetAlarmOutput(false));
    hal_t hal = mock.MakeHal();
    logger_t lg;
    logger_init(&lg, nullptr, nullptr, LOG_INFO);
    pump_t p;
    pump_init(&p, &hal, &lg);
    EXPECT_EQ(p.state, PUMP_IDLE);
    EXPECT_EQ(p.alarm, ALARM_NONE);
}

TEST_F(PumpTest, MotorStaysOffWhileIdle)
{
    EXPECT_CALL(mock, SetMotorDuty(0.0f)).Times(3);
    Step(3);
}

TEST_F(PumpTest, StartAppliesFeedForwardDuty)
{
    ASSERT_EQ(pump_set_rate(&p, 120.0f), PUMP_OK);
    ASSERT_EQ(pump_start(&p), PUMP_OK);
    sensors.flow = 120.0f; // already at target -> pure feed-forward
    EXPECT_CALL(mock, SetMotorDuty(FloatNear(120.0f / PUMP_NOMINAL_MAX_FLOW, 1e-3f)));
    Step();
    EXPECT_EQ(p.state, PUMP_RUNNING);
}

TEST_F(PumpTest, ClosedLoopRaisesDutyWhenUnderDelivering)
{
    pump_start(&p);
    sensors.flow = 50.0f; // well below 100 ml/h target
    float ff = 100.0f / PUMP_NOMINAL_MAX_FLOW;
    EXPECT_CALL(mock, SetMotorDuty(Gt(ff)));
    Step();
}

TEST_F(PumpTest, RejectsRateOutsideLimits)
{
    EXPECT_EQ(pump_set_rate(&p, 0.0f), PUMP_ERR_RANGE);
    EXPECT_EQ(pump_set_rate(&p, PUMP_RATE_MAX_MLH + 1.0f), PUMP_ERR_RANGE);
    EXPECT_EQ(pump_set_rate(&p, -5.0f), PUMP_ERR_RANGE);
    EXPECT_FLOAT_EQ(p.target_rate, 100.0f);
}

TEST_F(PumpTest, PauseAndResume)
{
    pump_start(&p);
    sensors.flow = 100.0f;
    Step(5);
    EXPECT_EQ(pump_pause(&p), PUMP_OK);
    EXPECT_CALL(mock, SetMotorDuty(0.0f));
    Step();
    EXPECT_EQ(pump_start(&p), PUMP_OK);
    EXPECT_EQ(p.state, PUMP_RUNNING);
}

TEST_F(PumpTest, HighPressureRaisesOcclusionAlarmAndStopsMotor)
{
    pump_start(&p);
    sensors.flow = 100.0f;
    Step(5);

    sensors.pressure = PUMP_OCCLUSION_KPA + 5.0f;
    EXPECT_CALL(mock, SetAlarmOutput(true));
    EXPECT_CALL(mock, SetMotorDuty(0.0f));
    Step();
    EXPECT_EQ(p.state, PUMP_ALARM);
    EXPECT_EQ(p.alarm, ALARM_OCCLUSION);
    EXPECT_EQ(pump_start(&p), PUMP_ERR_STATE);
}

TEST_F(PumpTest, ConsecutiveMissedReadsRaiseSensorTimeout)
{
    pump_start(&p);
    sensors.flow = 100.0f;
    Step(3);
    sensors.ok = false;
    Step(PUMP_MAX_MISSED_READS - 1);
    EXPECT_EQ(p.state, PUMP_RUNNING) << "a few missed reads are tolerated";
    Step();
    EXPECT_EQ(p.alarm, ALARM_SENSOR_TIMEOUT);
}

TEST_F(PumpTest, StopsWhenVtbiDelivered)
{
    ASSERT_EQ(pump_set_rate(&p, 360.0f), PUMP_OK); // 0.01 ml per 100 ms
    ASSERT_EQ(pump_set_vtbi(&p, 0.05f), PUMP_OK);
    pump_start(&p);
    sensors.flow = 360.0f;
    Step(4); // first step has dt=0, so 0.03 ml so far
    EXPECT_EQ(p.state, PUMP_RUNNING);
    Step(3);
    EXPECT_EQ(p.state, PUMP_COMPLETE);
    EXPECT_GE(p.volume_est, 0.05f);
    EXPECT_EQ(pump_start(&p), PUMP_ERR_STATE) << "needs a new VTBI first";
    EXPECT_EQ(pump_set_vtbi(&p, 1.0f), PUMP_OK);
    EXPECT_EQ(p.state, PUMP_IDLE);
}

TEST_F(PumpTest, IdsOffOnlyLogsReplayedFrames)
{
    pump_start(&p);
    sensors.flow = 100.0f;
    Step(25);
    sensors.seq -= 10; // replay old counters
    Step();
    EXPECT_TRUE(p.anomalies & ANOM_SEQ);
    EXPECT_EQ(p.state, PUMP_RUNNING);
}

TEST_F(PumpTest, IdsOnAlarmsOnReplayedFrames)
{
    pump_set_ids(&p, true);
    pump_start(&p);
    sensors.flow = 100.0f;
    Step(25);
    sensors.seq -= 10;
    Step();
    EXPECT_EQ(p.state, PUMP_ALARM);
    EXPECT_EQ(p.alarm, ALARM_SENSOR_FAULT);
}

TEST_F(PumpTest, SetpointCorruptionIsDetectedAndClearResetsToSafeRate)
{
    pump_start(&p);
    sensors.flow = 100.0f;
    Step(3);
    p.target_rate = 900.0f; // bypass the API, as RAM tampering would
    Step();
    EXPECT_EQ(p.alarm, ALARM_MEM_CORRUPT);

    EXPECT_CALL(mock, SetAlarmOutput(false));
    EXPECT_EQ(pump_clear_alarm(&p), PUMP_OK);
    EXPECT_FLOAT_EQ(p.target_rate, PUMP_RATE_MIN_MLH);
    Step();
    EXPECT_EQ(p.state, PUMP_IDLE) << "check passes again after reset";
}

TEST_F(PumpTest, ClearOnlyValidInAlarm)
{
    EXPECT_EQ(pump_clear_alarm(&p), PUMP_ERR_STATE);
}

TEST_F(PumpTest, StopIsRefusedUntilAlarmIsCleared)
{
    pump_start(&p);
    sensors.pressure = 99.0f;
    Step();
    ASSERT_EQ(p.state, PUMP_ALARM);
    EXPECT_EQ(pump_stop(&p), PUMP_ERR_STATE);
}

} // namespace
