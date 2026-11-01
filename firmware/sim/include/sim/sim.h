/*
 * sim.h - test-environment harness. Owns the virtual plant, the attacker on
 * the sensor bus, the firmware instance and the logger, and advances them in
 * lock-step one control period at a time.
 *
 * Output (one record per line, via the logger sink):
 *   TLM,tick_ms,state,alarm,target_rate,flow_meas,flow_true,pressure_meas,
 *       pressure_true,volume_est,volume_true,vtbi,duty,anomaly,attack
 *   LOG,tick_ms,LEVEL,MODULE,message
 *   ACK,tick_ms,verb,message      ERR,tick_ms,verb,message
 *
 * Input (sim_command): every firmware console command (see fw/cmd.h) plus
 *   attack <spike|drift|replay|freeze|dropout|noise> [magnitude] [samples]
 *   attack tamper <rate>        attack clear
 *   fault occlusion on|off      help      quit
 */
#ifndef SIM_SIM_H
#define SIM_SIM_H

#include <stdbool.h>
#include <stdint.h>

#include "fw/hal.h"
#include "fw/logger.h"
#include "fw/pump.h"
#include "sim/attack.h"
#include "sim/plant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SIM_TLM_HEADER \
    "tick_ms,state,alarm,target_rate,flow_meas,flow_true,pressure_meas,pressure_true," \
    "volume_est,volume_true,vtbi,duty,anomaly,attack"

typedef struct {
    uint32_t    period_ms;
    uint32_t    seed;
    log_sink_fn sink;
    void       *sink_ctx;
} sim_config_t;

typedef struct {
    plant_t        plant;
    attack_t       attack;
    pump_t         pump;
    logger_t       log;
    hal_t          hal;

    uint32_t       millis;
    uint32_t       period_ms;

    /* Sensor bus as seen by the firmware this period (post-attack). */
    sensor_frame_t bus_flow, bus_press;
    bool           bus_flow_ok, bus_press_ok;

    float          motor_duty;
    bool           alarm_output;
    attack_type_t  prev_attack;
    bool           quit;
} sim_t;

void sim_init(sim_t *s, const sim_config_t *cfg);
void sim_tick(sim_t *s);
void sim_command(sim_t *s, const char *line);

#ifdef __cplusplus
}
#endif

#endif /* SIM_SIM_H */
