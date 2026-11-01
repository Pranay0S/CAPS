# IV Pump Testbench: Design Notes

## 1. Purpose

An infusion (IV) pump delivers fluid at a programmed rate. Its firmware closes a control loop on flow, watches line pressure for occlusions, and stops the motor and raises an alarm when something looks unsafe.

This project recreates a **simplified** version of that firmware in embedded-style C and runs it in a virtual environment. The environment lets us:

- exercise the firmware with unit tests (GoogleTest / GMock),
- inject cyber attacks on the sensor bus and in RAM, and see their **physical** effect,
- record labelled CSV datasets (ground-truth attack vs. what the firmware detected) for anomaly-detection / TinyML work.

It is not a clinical device model.

---

## 2. Architecture

```text
┌──────────────────────────── ivpump_sim process ────────────────────────────┐
│                                                                             │
│  plant.c                attack.c                    firmware (fw/)          │
│  ┌─────────────┐  frames ┌───────────────┐ frames  ┌──────────────────────┐ │
│  │ motor + line│────────►│ adversary on  │────────►│ HAL ► pump.c         │ │
│  │ sensors     │         │ sensor bus    │         │        ├ monitor.c   │ │
│  │ (true state)│◄────────┤               │         │        ├ logger.c    │ │
│  └─────────────┘  duty   └───────────────┘         │ cmd.c ◄┘             │ │
│        ▲                                            └──────────┬───────────┘ │
│        └────────────── motor duty (HAL set_motor_duty) ◄───────┘             │
│                                                                             │
│  sim.c: harness; ticks everything in lock-step, owns the line protocol     │
│  main.c: stdin reader thread, fixed-period loop, stdout sink               │
└─────────────────────────────────────────────────────────────────────────────┘
          ▲ stdin: commands                     │ stdout: TLM / LOG / ACK / ERR
          │                                     ▼
       gui/server.js ── SSE ──► browser GUI   logs/session_*.csv
          ▲
          └── HTTP ── gui/inject.js (second terminal)
```

### Layering rule

`firmware/src` + `firmware/include/fw` are the **firmware**. They only talk to the outside world through `hal_t` (function pointers) and never allocate memory, so the same code could be compiled for an MCU with a real HAL.
Everything under `firmware/sim` is **test environment** and would not ship.

---

## 3. Firmware

### 3.1 HAL (`fw/hal.h`)

| Function | Real hardware analogue |
|---|---|
| `read_flow`, `read_pressure` | smart sensor on I²C/CAN; returns `{value, seq}` or `HAL_TIMEOUT` |
| `set_motor_duty` | PWM to the pump motor driver (0..1) |
| `set_alarm_output` | buzzer / alarm LED |
| `millis` | system tick |

Each sensor frame carries a rolling **sequence counter** stamped by the sensor. The firmware uses it to reject stale or replayed frames.

### 3.2 Pump state machine (`pump.c`)

```text
          start                VTBI reached
  IDLE ───────────► RUNNING ───────────────► COMPLETE
   ▲  ◄───────────  │  ▲  │                    │ vtbi <ml>
   │     stop       │  │  │ pause / start      ▼
   │   clear      ALARM  PAUSED               IDLE
   └────────────────┘
```

- `start` is refused in `ALARM` and `COMPLETE`. `stop` is refused in `ALARM` (clear it first).
- Rate limits: 0.1–999 ml/h. VTBI: 0–9999 ml (0 = unlimited). Setting a VTBI resets the volume counter.

### 3.3 Control loop

Runs every period (default 100 ms):

```text
duty = target / 1200            (feed-forward from the nominal motor model)
     + Kp · (target − flow_meas)
     + Ki · ∫(target − flow_meas) dt   (clamped anti-windup)
```

On a missed sensor read the last duty is held. Volume is integrated from **measured** flow, so a spoofed sensor makes the pump's own volume estimate wrong too. The telemetry shows `volume_est` next to `volume_true` for that reason.

### 3.4 Safety alarms

Every alarm stops the motor and turns on the alarm output.

| Alarm | Trigger |
|---|---|
| `OCCLUSION` | measured pressure > 40 kPa while running |
| `SENSOR_TIMEOUT` | 5 consecutive missed reads while running |
| `SENSOR_FAULT` | monitor anomaly while **IDS enforce** is on |
| `MEM_CORRUPT` | setpoint fails its redundancy check (stored as value + bitwise complement). Clearing it resets the rate to the 0.1 ml/h minimum |

### 3.5 Sensor plausibility monitor (`monitor.c`)

This is a lightweight rule-based IDS. It returns a bitmask per sample:

| Flag | Rule | Typically catches |
|---|---|---|
| `SEQ` | frame counter did not move forward | replay |
| `ROC_FLOW` / `ROC_PRESS` | jump larger than 15 ml/h + 20 % of target, or 5 kPa | spike, noise |
| `STUCK` | 8 identical flow readings while running | freeze |
| `MISMATCH` | measured flow differs from `duty × 1200` by > 25 % for 15 samples | drift, slow tampering |
| `DROPOUT` | a sensor did not answer | jamming |

After a start or rate change, ROC and mismatch checks are suppressed for 20 samples (the settle window), so normal transients aren't flagged. An attack launched inside that window can therefore slip past the ROC check, which is a deliberate weakness worth studying.

New flags are logged as `WARN` (`MON` module) on their rising edge.

### 3.6 Logger (`logger.c`)

- A fixed RAM ring of 64 × 192-byte lines. No `malloc`, no blocking.
- `logger_event()` formats `LOG,<tick>,<LEVEL>,<MODULE>,<msg>`. Commas in text are replaced with `;` so every record is exactly one CSV row.
- `logger_flush()` drains the ring to a sink, as a UART TX task would. On overflow, lines are dropped and a `ring overflow: N lines dropped` warning is emitted on the next flush.

### 3.7 Console (`cmd.c`)

The pump's text service port: `start pause stop clear status rate vtbi ids loglevel`. Arguments are strictly validated (`rate 12abc`, `rate nan` and extra arguments are all rejected).

---

## 4. Test environment (`firmware/sim`)

### 4.1 Plant model (`plant.c`)

- **Flow:** first-order lag (τ = 0.4 s) toward `duty × 1200 × (1 − 0.95·occlusion)` ml/h.
- **Pressure:** `8 + 0.02·flow + 60·occlusion` kPa.
- **Occlusion fault:** ramps from 0 to 1 over 3 s when injected, and decays when removed.
- **Sensors:** Gaussian noise (σ = 0.8 ml/h, 0.3 kPa) and a shared incrementing frame counter.
- A seeded xorshift PRNG makes every run reproducible.

### 4.2 Attacker (`attack.c`)

A man-in-the-middle on the sensor bus. It records every genuine frame, then modifies the frames before the firmware reads them.

| Attack | Effect on frames | Physical consequence (IDS off) |
|---|---|---|
| SPIKE | +mag flow, +0.3·mag pressure for N samples | controller cuts duty → **under-infusion**; large spikes can spoof an occlusion alarm |
| DRIFT | bias grows each sample | slow, sustained under-infusion; pressure bias eventually trips a false occlusion |
| REPLAY | loops an old window, including the old `seq` | controller acts on stale data |
| FREEZE | constant value, fresh `seq` | controller blind to real changes |
| DROPOUT | frames swallowed | `SENSOR_TIMEOUT` alarm (denial of therapy) |
| NOISE | heavy Gaussian noise | erratic duty |
| TAMPER | harness writes `pump.target_rate` directly | `MEM_CORRUPT` alarm on next cycle |

### 4.3 Harness (`sim.c`) and process (`main.c`)

Each tick runs `plant_step → plant_sample → attack_apply → pump_step → emit TLM → flush log`.
`main.c` runs the tick on a wall-clock period (or `--fast`) and feeds stdin commands through a mutex-protected queue from a reader thread. Lines of the form `@<tick_ms> cmd` are scheduled for scripted runs.

---

## 5. Line protocol

| Prefix | Fields |
|---|---|
| `#TLM` | column names (sent once with `--header`) |
| `TLM` | `tick_ms,state,alarm,target_rate,flow_meas,flow_true,pressure_meas,pressure_true,volume_est,volume_true,vtbi,duty,anomaly,attack` |
| `LOG` | `tick_ms,level,module,message` (module `SIM` = ground-truth harness events) |
| `ACK` / `ERR` | `tick_ms,verb,message` |

`gui/lib/protocol.js` parses these lines. `gui/lib/session.js` adds a host timestamp and writes them to CSV.

---

## 6. GUI and recording (`gui/`)

- `server.js` uses only Node built-ins. It spawns `ivpump_sim`, streams parsed records to browsers over Server-Sent Events, forwards `POST /api/cmd` to the sim's stdin, and records every record, including the commands themselves (`CMD`, tagged `gui` or `cli`).
- CSV export endpoints: `/api/export/telemetry.csv`, `/api/export/events.csv`, `/api/export/all.csv`. Files are also appended live under `logs/`.
- `inject.js` is the secondary terminal. It uses the same HTTP API, subscribes to the same event stream, and prints ACK/ERR and warnings as they happen.

---

## 7. Extending

- **New attack:** add an `attack_type_t` value, a name, defaults, and a `case` in `attack_apply()`, plus an `AttackTest` and a `SimTest` showing whether the monitor catches it.
- **New detector:** add an `ANOM_*` bit and a rule in `monitor_check()`. It appears automatically in the `anomaly` CSV column.
- **ML model in the loop:** the telemetry CSV is the training set (`attack` as label). An on-device model could replace or augment `monitor_check()` behind the same interface.
