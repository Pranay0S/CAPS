# CAPS IV Pump Testbench

A virtual test environment for IV-pump firmware, built for the ESE 534 Cyber-Physical Systems project.

- **`firmware/`**: embedded-style C11 firmware (state machine, closed-loop flow control, safety alarms, sensor plausibility monitor, ring-buffered logger), running against a simulated plant and an attacker on the sensor bus. It is unit-tested with **GoogleTest / GMock**.
- **`gui/`**: a dependency-free **Node.js** server plus browser GUI. It runs the firmware simulator, charts it live, records every log line to **CSV**, and provides an **attack terminal** in the page. A standalone attack console, `inject.js`, runs in a second OS terminal.

> Educational/research use only. This is not a medical device and must not be used to control one.

```text
 browser GUI ──SSE/HTTP──┐                    ┌── stdin:  commands / attacks
                         ├── gui/server.js ───┤
 node gui/inject.js ─HTTP┘     │              └── stdout: TLM / LOG / ACK / ERR lines
   (2nd terminal)              │                        │
                               ▼                        ▼
                     logs/session_*.csv        firmware/build/ivpump_sim
                                                 ┌──────────────────────────┐
                                                 │ plant ─► attacker ─► HAL │
                                                 │            (sensor bus)  │
                                                 │        firmware (fw/)    │
                                                 └──────────────────────────┘
```

---

## Prerequisites

| Tool | Tested with | Notes |
|---|---|---|
| C/C++ compiler | GCC 14 (MSYS2 UCRT64) | any C11 / C++17 compiler |
| CMake + Ninja | 3.31 / 1.12 | |
| GoogleTest/GMock | 1.15 | uses the system package; otherwise CMake downloads it automatically |
| Node.js | 23.5 | 18 or newer; **no npm install needed** |
| Python + numpy | 3.13 / 2.5 | only for `ml/` (training the attack detector); Python 3.10 or newer |

On Windows with MSYS2:

```sh
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,gtest,nodejs}
```

## Quick start

```sh
# 1. build firmware simulator + tests
cmake -S firmware -B firmware/build -G Ninja
cmake --build firmware/build

# 2. run the unit tests
ctest --test-dir firmware/build --output-on-failure

# 3. start the GUI (spawns the simulator)
node gui/server.js            # → http://127.0.0.1:8534

# 4. (optional) attack console in a second terminal
node gui/inject.js
```

VS Code users can run the **build**, **test**, **gui** and **inject** tasks from `.vscode/tasks.json`.

## Using the GUI

| Area | What it does |
|---|---|
| Status strip | Pump state, alarm, **attack ground truth** (what the attacker is doing) and **monitor flags** (what the firmware detected) |
| Tiles | Target rate, measured vs **true** flow, pressure, estimated vs true volume, motor duty |
| Charts | Last 60 s of flow and pressure. Shaded bands mark active attacks; hover for values |
| Pump console | Start/pause/stop/clear, rate, VTBI, IDS enforce, occlusion fault |
| Event log | Every firmware/harness log line, command, ACK/ERR, filterable by level |
| Attack terminal | Type commands (`help`, Tab-complete, ↑/↓ history) or use the quick-attack chips |
| **Telemetry / Labels / Events / All logs CSV** | Download the current session as CSV (telemetry = pump data only, labels = ground truth) |

Every session is also written to `logs/` as it runs (`session_<timestamp>_telemetry.csv`, `_labels.csv` and `_events.csv`), so nothing is lost if the browser closes. **Restart sim** starts a new session.

## Attack terminal / `inject.js`

Both terminals send the same commands. `inject.js` also works one-shot, which makes it easy to script:

```sh
node gui/inject.js                     # interactive, shows live ACK/ERR/WARN
node gui/inject.js attack spike 80 10  # one-shot
```

| Command | Effect |
|---|---|
| `attack spike [mag=60] [samples=6]` | transient added to flow (and 0.3× to pressure) |
| `attack drift [rate=0.4] [samples]` | bias that grows by `rate` ml/h every sample (stealthy) |
| `attack replay [window=50] [samples]` | loops recorded genuine frames, including their old sequence numbers |
| `attack freeze [-] [samples]` | holds the last reading; frame counter keeps moving |
| `attack dropout [-] [samples]` | swallows frames (jamming / DoS) |
| `attack noise [sigma=25] [samples]` | heavy Gaussian noise |
| `attack tamper <rate>` | overwrites the setpoint in firmware RAM, bypassing the API |
| `attack clear` | stops the active attack |
| `fault occlusion on\|off` | physical downstream occlusion (a fault, not an attack) |
| `start` `pause` `stop` `clear` `status` | pump console |
| `rate <ml/h>` `vtbi <ml>` | programming (0.1–999 ml/h; VTBI 0 = unlimited) |
| `ids on\|off` | **off** (default): anomalies are logged only. **on**: anomalies raise `SENSOR_FAULT` and stop the pump |
| `loglevel debug\|info\|warn\|error` | firmware log verbosity |

Omitting `samples` means the attack runs until `attack clear`.

## CSV formats

Each session writes three files to `logs/`. The telemetry file contains **only data the pump itself generates**; anything only the test harness knows goes to a separate, row-aligned labels file.

**Telemetry** (`*_telemetry.csv`): one row per 100 ms control period, exactly what the firmware could log on the device. This is the model input.

```
tick_ms,state,alarm,target_rate,flow_meas,flow_seq,flow_ok,pressure_meas,pressure_seq,pressure_ok,
volume_est,vtbi,duty
```

- `*_meas` / `*_seq` / `*_ok`: the last sensor frame as the firmware received it (post-attack): value, frame counter, and whether the sensor answered this period.

**Labels** (`*_labels.csv`): ground truth for training and evaluation only. Never use it as a model input.

```
tick_ms,attack,fault,anomaly,flow_true,pressure_true,volume_true
```

- `attack`: the attack applied to that period's frames. `fault`: `OCCLUSION` while the physical fault is injected.
- `*_true`: the plant's real state, which the pump cannot see.
- `anomaly`: flags from the firmware's rule-based monitor (`ROC_FLOW|SEQ|...` or `NONE`), kept here as a baseline detector.

**Events** (`*_events.csv`): `host_time,tick_ms,type,level,module,message`. `type` is one of `LOG`, `ACK`, `ERR`, `CMD` (with `module` = `gui`/`cli`) or `SYS`.

**All logs**: every record merged in arrival order with a `record_type` column. It is a debug dump that includes ground truth, so don't train on it.

## Attack-detection RNN (`ml/`)

A tiny int8 GRU learns to flag each attack type from the telemetry CSV alone, and runs in the firmware (`fw/ids_rnn.h`). On a Cortex-M4F it takes 5.3 kB of flash in total (3.5 kB of int8 parameters, 1.8 kB of code, no C-library code) and 168 B of RAM; see [firmware/mcu/README.md](firmware/mcu/README.md) for the STM32 measurements. Pipeline: generate labelled runs with the simulator, train in numpy, export to C, and check C/Python parity in the unit tests. See [ml/README.md](ml/README.md).

## Headless / scripted runs

The simulator is a normal CLI. Commands prefixed with `@<tick_ms>` are scheduled, so datasets are reproducible by seed:

```sh
printf '@0 start\n@5000 attack spike\n@20000 attack drift\n@40000 attack clear\n' \
  | firmware/build/ivpump_sim --fast --ticks 600 --seed 42 --header > run.log
```

## Tests

```sh
ctest --test-dir firmware/build --output-on-failure   # C/C++ tests
cd gui && node --test                                  # protocol / CSV tests
```

| Suite | Covers |
|---|---|
| `LoggerTest` | CSV record format, comma sanitising, level filter, ring overflow reporting |
| `MonitorTest` | sequence/replay check, rate-of-change, stuck sensor, model mismatch, settle window |
| `PumpTest` (GMock HAL) | state machine, feed-forward + PI control, occlusion / timeout / VTBI / IDS / RAM-tamper alarms |
| `CmdTest` | console parser, argument validation |
| `AttackTest` | each attack's effect on sensor frames |
| `SimTest` | closed loop with no mocks: convergence, each attack detected, determinism by seed |
| `IdsRnnTest` | C feature extraction and int8 GRU match the Python model on golden vectors; seq/dropout/freeze features |

## Repository layout

```text
firmware/
  include/fw/      firmware headers: hal, pump, monitor, logger, cmd, fw_config
  src/             firmware sources (portable C11, no malloc), incl. ids_rnn.c
  sim/             test environment: plant model, attacker, harness, main()
  tests/           GoogleTest / GMock suites
gui/
  server.js        spawns the sim, SSE stream, CSV recording/export
  inject.js        standalone attack console (second terminal)
  lib/             line-protocol parser, session/CSV writer
  public/          browser GUI (vanilla JS, canvas charts)
ml/                   attack-detection RNN: dataset generator, numpy GRU trainer, C exporter
docs/iv_pump_sys.md   design notes
```

See [docs/iv_pump_sys.md](docs/iv_pump_sys.md) for the design details, and [docs/ids_rnn_guide.md](docs/ids_rnn_guide.md) for the full install list, how the RNN works, and how its memory and accuracy were measured.
