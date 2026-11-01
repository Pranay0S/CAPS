// mock_hal.hpp - GMock stand-in for the firmware HAL.
//
// The HAL is a C table of function pointers; MakeHal() fills it with
// captureless lambdas that forward to a MockHal instance through ctx.
#pragma once

#include <gmock/gmock.h>

#include "fw/hal.h"

class MockHal {
public:
    MOCK_METHOD(hal_status_t, ReadFlow, (sensor_frame_t *));
    MOCK_METHOD(hal_status_t, ReadPressure, (sensor_frame_t *));
    MOCK_METHOD(void, SetMotorDuty, (float));
    MOCK_METHOD(void, SetAlarmOutput, (bool));
    MOCK_METHOD(uint32_t, Millis, ());

    hal_t MakeHal()
    {
        hal_t h{};
        h.read_flow = [](void *c, sensor_frame_t *o) { return static_cast<MockHal *>(c)->ReadFlow(o); };
        h.read_pressure = [](void *c, sensor_frame_t *o) { return static_cast<MockHal *>(c)->ReadPressure(o); };
        h.set_motor_duty = [](void *c, float d) { static_cast<MockHal *>(c)->SetMotorDuty(d); };
        h.set_alarm_output = [](void *c, bool on) { static_cast<MockHal *>(c)->SetAlarmOutput(on); };
        h.millis = [](void *c) { return static_cast<MockHal *>(c)->Millis(); };
        h.ctx = this;
        return h;
    }
};

// Scripted sensor source with an auto-incrementing frame counter, so tests
// can describe readings as plain values.
struct FakeSensors {
    float    flow     = 0.0f;
    float    pressure = 10.0f;
    uint16_t seq      = 0;
    bool     ok       = true;
    uint32_t now      = 0;

    void Wire(MockHal &m)
    {
        using ::testing::_;
        using ::testing::Invoke;
        ON_CALL(m, Millis()).WillByDefault(Invoke([this] { return now; }));
        ON_CALL(m, ReadFlow(_)).WillByDefault(Invoke([this](sensor_frame_t *o) {
            if (!ok) return HAL_TIMEOUT;
            *o = { flow, ++seq };
            return HAL_OK;
        }));
        ON_CALL(m, ReadPressure(_)).WillByDefault(Invoke([this](sensor_frame_t *o) {
            if (!ok) return HAL_TIMEOUT;
            *o = { pressure, seq };
            return HAL_OK;
        }));
    }
};
