# IDS-RNN: tiny on-device attack detector

An int8 GRU that reads the pump's own telemetry, one sample per 100 ms control period, and flags sensor-bus and memory attacks so the firmware can take control. It is trained in numpy (no deep-learning framework needed) and exported to C (`firmware/include/fw/ids_rnn.h`).

| | |
|---|---|
| Inputs | 15 features computed from the **telemetry CSV only** (pump-generated data) |
| Network | GRU, 24 hidden units, then a linear layer to 9 classes: `NONE OCCLUSION SPIKE DRIFT REPLAY FREEZE DROPOUT NOISE TAMPER` |
| Parameters | 3,129, stored as int8 with 7 per-gate scales and float biases: **3,472 B of flash** |
| Total on Cortex-M4F | **5,322 B of flash** at `-Os`: parameters plus 1,830 B of code, with no C-library code (the detector has its own small `exp` and `log`), measured with `firmware/mcu/measure_size.py` |
| RAM | 168 B of persistent state on Cortex-M4F plus a 720 B stack frame per step (compiler-reported; peak to be confirmed on the board) |
| Compute per step | about 3k int8 multiply-accumulates and 81 calls to a small built-in `exp` |

`OCCLUSION` is a real physical fault, not an attack. It has its own class so the detector learns *not* to flag it. The pump's occlusion alarm already handles it.

## Pipeline

```sh
cmake --build firmware/build              # simulator with the current TLM format
python ml/gen_dataset.py                  # 240/30/60 runs x 5 min -> ml/data/{train,val,test}
python ml/train.py                        # ~2 min on a laptop CPU -> ml/out/ids_gru.npz, report.txt
python ml/export_c.py                     # -> firmware/include/fw/ids_model.h, firmware/tests/ids_golden.h
cmake --build firmware/build && ctest --test-dir firmware/build   # IdsRnnTest checks C == Python
```

The only requirement is Python 3.10+ with numpy. For how the network works and how memory and accuracy were measured, see [docs/ids_rnn_guide.md](../docs/ids_rnn_guide.md).

| File | Role |
|---|---|
| `common.py` | CSV data contract, class list, **streaming feature extractor** (mirrored by `ids_rnn.c`) |
| `gen_dataset.py` | Random, seeded scenarios run through the real firmware simulator: benign rate changes, pauses and stops, occlusions, and every attack with random size and duration |
| `tinygru.py` | GRU forward pass, BPTT (gradient-checked), Adam, int8 quantisation, fake-quant for QAT |
| `train.py` | Stateful truncated BPTT, class-weighted loss, QAT fine-tuning, threshold selection, event-level evaluation against the rule-based monitor |
| `export_c.py` | Writes the weights header and golden test vectors; fails if the model is over the 6 kB budget |

GUI sessions can be used as extra data. Copy `logs/session_*_telemetry.csv` and `session_*_labels.csv` into any `ml/data/<split>/` folder.

## Data contract

- **Inputs** come only from `*_telemetry.csv`: `state, alarm, target_rate, flow_meas, flow_seq, flow_ok, pressure_meas, pressure_seq, pressure_ok, duty`.
- **Targets** come from `*_labels.csv`: `attack` if one is active, else `OCCLUSION` if `fault` is set, else `NONE`. `anomaly` (the rule-based monitor) is used only as a baseline. `*_true` columns are never used.

Features are scaled to O(1) with fixed constants, so no normalisation statistics need to ship. They are: motor-model and setpoint residuals, rates of change, pressure/flow consistency, bit-identical repeats (freeze), frame-counter staleness against a high-water mark (replay), skipped frames, missing reads (dropout), and alarm state (`MEM_CORRUPT` for tamper).

## Results (test split: 60 unseen runs, 5 h)

Alert rule: `P(attack) = 1 - P(NONE) - P(OCCLUSION) > 0.50` for 3 consecutive periods. The threshold was chosen on the validation set (at most 1 false alert/h).

| Attack | Episodes | IDS-RNN recall | Latency (median) | Rule monitor recall | Rule latency |
|---|---|---|---|---|---|
| SPIKE | 71 | 0.958 | 0.2 s | 1.000 | 0.0 s |
| DRIFT | 72 | **1.000** | **0.7 s** | 0.861 | 5.4 s |
| REPLAY | 71 | 1.000 | 0.2 s | 1.000 | 0.0 s |
| FREEZE | 81 | 1.000 | 0.2 s | 1.000 | 0.7 s |
| DROPOUT | 76 | 0.987 | 0.2 s | 1.000 | 0.0 s |
| NOISE | 87 | **1.000** | 0.3 s | 0.816 | 0.2 s |
| TAMPER | 75 | **1.000** | 0.2 s | 0.000 | – |
| **Overall** | 533 | **0.992** | | 0.811 | |
| False alerts | | 1.43 / h | | 0.57 / h | |

- Per-sample macro-F1 is 0.966 for int8 and 0.966 for fp32, so quantisation costs nothing measurable.
- When it fires, the model names the right attack type 100 % of the time for all classes except TAMPER (73 %). After a tamper, the pump sits in `MEM_CORRUPT`, which looks the same whatever caused it.
- The missed SPIKEs are the smallest ones (around 15 ml/h for 2 samples at high rates), which are hard to tell apart from sensor noise.
- The full report, with the confusion matrix, is in `out/report.txt`.

These numbers come from the simulator's plant and attack models. A real pump and a smarter attacker (for example, one that keeps drift under the residual noise) will be harder. Use the GUI to stress-test it with your own attacks.

## Using it in firmware

```c
#include "fw/ids_rnn.h"

static ids_rnn_t ids;            /* ids_rnn_init(&ids) at boot */

/* at the end of pump_step(), once this period's duty is computed: */
ids_input_t in = {
    .running = p->state == PUMP_RUNNING, .alarm = p->alarm != ALARM_NONE,
    .mem_alarm = p->alarm == ALARM_MEM_CORRUPT, .target_rate = p->target_rate,
    .flow = p->flow_meas, .flow_seq = p->flow_seq, .flow_ok = p->flow_ok,
    .pressure = p->pressure_meas, .press_seq = p->press_seq, .press_ok = p->press_ok,
    .duty = p->duty,   /* the duty commanded this period, as logged */
};
ids_rnn_step(&ids, &in);
if (ids.alert) { /* ids.attack_cls says which attack: fail safe / distrust sensor */ }
```

The detector is built and tested but **not yet wired into `pump_step()`**. Deciding what "take control" means is a design choice: stop with `ALARM_SENSOR_FAULT`, or fall back to open-loop feed-forward control while a sensor is untrusted. Note that `duty` must be the value *after* this period's control update, which is what the telemetry logs.
