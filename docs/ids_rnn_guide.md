# CAPS guide: setup, how the repo works, and the attack-detection RNN

This guide covers four things:

1. [What to install](#1-what-to-install)
2. [How the repo works end to end](#2-how-the-repo-works)
3. [RNN background, and the structure of this specific RNN](#3-rnn-background)
4. [How its memory use and accuracy were measured](#7-how-memory-was-measured), including what was *not* measured

For the pump firmware's design (state machine, control loop, alarms, rule-based monitor), see [iv_pump_sys.md](iv_pump_sys.md).

---

## 1. What to install

| # | What | Needed for | Version used here | Where to get it |
|---|---|---|---|---|
| 1 | **MSYS2** (UCRT64 environment) | Windows only: supplies the C toolchain below | – | <https://www.msys2.org> |
| 2 | **GCC** (C11 and C++17 compiler) | building the firmware, simulator and tests | 14.2.0 | MSYS2 package |
| 3 | **CMake** | build system | 3.31.5 (3.16 minimum) | MSYS2 package |
| 4 | **Ninja** | build backend | 1.12.1 | MSYS2 package |
| 5 | **GoogleTest / GMock** | unit tests | 1.15 | MSYS2 package (optional, see below) |
| 6 | **Node.js** | GUI server and attack console | 23.5.0 (18 minimum) | MSYS2 package or <https://nodejs.org> |
| 7 | **Python** | `ml/` only: dataset generation, training, export | 3.13.3 (3.10 minimum) | <https://www.python.org> |
| 8 | **numpy** | `ml/` only | 2.5.3 | `pip install numpy` |
| 9 | A web browser | the GUI | any current one | – |

Items 2–6 in one command, from the *MSYS2 UCRT64* shell:

```sh
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,gtest,nodejs}
```

On Linux or macOS, skip MSYS2 and install `gcc`/`clang`, `cmake`, `ninja`, `nodejs`, `python3` and `numpy` from your package manager.

**Optional**

| What | Why |
|---|---|
| Git | cloning and version control |
| VS Code | `.vscode/tasks.json` has build / test / gui / inject tasks |
| Arm GNU toolchain (`arm-none-eabi-gcc`, binutils, newlib) | measuring and running on the STM32: `pacman -S mingw-w64-ucrt-x86_64-arm-none-eabi-{gcc,binutils,newlib}` (installed: GCC 13.3.0) |
| STM32 Nucleo board + USB cable | on-target timing, stack and correctness check; flashing is drag-and-drop, no software needed |

**Not needed**

- `npm install`: the GUI has no dependencies.
- PyTorch or TensorFlow: the network is trained with numpy alone.
- GoogleTest, if you have internet at configure time: CMake downloads 1.15.2 by itself when the package is missing.

**Two Windows pitfalls on this machine**

- There are two Pythons. The MSYS2 one (`C:\msys64\ucrt64\bin\python`) has no numpy; the python.org one (3.13) does. The `ml/` scripts start with `#!/usr/bin/env python3`, and the `py` launcher honours that line by searching `PATH`, so plain `py ml/train.py` can pick the MSYS2 Python and fail with `No module named 'numpy'`. Run `py -3.13 ml/train.py` to force the right one.
- `ivpump_sim.exe` cannot be rebuilt while the GUI server is running, because the server holds the file open. Stop the server (Ctrl+C) before `cmake --build`.

### Run everything

```sh
# firmware, simulator, tests
cmake -S firmware -B firmware/build -G Ninja
cmake --build firmware/build
ctest --test-dir firmware/build --output-on-failure    # 75 C/C++ tests
cd gui && node --test && cd ..                         # 8 GUI tests

# GUI (http://127.0.0.1:8534) and the attack console in a second terminal
node gui/server.js
node gui/inject.js

# attack-detection RNN
py -3.13 ml/gen_dataset.py     # ~15 s, writes 131 MB to ml/data/
py -3.13 ml/train.py           # ~2-3 min, writes ml/out/
py -3.13 ml/export_c.py        # writes firmware/include/fw/ids_model.h and tests/ids_golden.h
cmake --build firmware/build && ctest --test-dir firmware/build   # checks C == Python
```

---

## 2. How the repo works

### The pieces

```text
firmware/src, include/fw   the pump firmware: portable C11, no malloc, talks only through hal_t
firmware/sim               test environment: plant physics, attacker, harness, main()
firmware/tests             GoogleTest suites
gui/                       Node server + browser page + attack console
ml/                        dataset generator, numpy GRU trainer, C exporter
logs/                      CSVs recorded by the GUI (git-ignored)
ml/data/                   generated training data (git-ignored)
```

### One control period (100 ms), in order

`sim_tick()` in [sim.c](../firmware/sim/src/sim.c) does this every period:

1. **Plant** (`plant.c`): the motor duty from the previous period drives the true flow through a first-order lag; pressure follows flow and any occlusion.
2. **Sensors**: the plant produces a flow frame and a pressure frame. Each is `{value, seq}`, where `seq` is a 16-bit counter that increases by one per frame.
3. **Attacker** (`attack.c`): sits between sensors and firmware. It records the genuine frames, then may alter them (spike, drift, replay, freeze, noise) or swallow them (dropout). Tamper is different: it overwrites the setpoint in the firmware's RAM.
4. **Firmware** (`pump_step()` in `pump.c`): reads the frames through the HAL, runs the rule-based monitor, checks alarms, computes the new duty, and integrates volume.
5. **Telemetry**: the harness prints one `TLM` line containing both what the pump saw and the ground truth it cannot see.

### From the TLM line to the two CSVs

The `TLM` line mixes two kinds of data, and the tooling separates them into two row-aligned files:

| File | Columns | Meaning |
|---|---|---|
| `*_telemetry.csv` | `tick_ms, state, alarm, target_rate, flow_meas, flow_seq, flow_ok, pressure_meas, pressure_seq, pressure_ok, volume_est, vtbi, duty` | What the pump produces or observes. **The model's only input.** |
| `*_labels.csv` | `tick_ms, attack, fault, anomaly, flow_true, pressure_true, volume_true` | Harness ground truth, plus the rule monitor's flags as a baseline. Training targets and evaluation only. |

The split is done in two places that must stay in sync: [protocol.js](../gui/lib/protocol.js) for GUI sessions and [common.py](../ml/common.py) / [gen_dataset.py](../ml/gen_dataset.py) for generated data. Because both write the same format, a GUI recording can be dropped into `ml/data/<split>/` and used directly.

### The ML loop

```text
gen_dataset.py ──► ml/data/{train,val,test}/run_*_{telemetry,labels}.csv
                         │
train.py ◄───────────────┘   features (common.py) → GRU (tinygru.py) → int8
   │
   ▼
ml/out/ids_gru.npz ──► export_c.py ──► fw/ids_model.h   (weights, as const arrays)
                                   └─► tests/ids_golden.h (inputs + Python's outputs)
                                              │
firmware/src/ids_rnn.c  ◄── compiled with ────┘── IdsRnnTest proves C output == Python output
```

The detector module is built into the firmware library and tested, but `pump_step()` does not call it yet.

---

## 3. RNN background

### Why a recurrent network

Most of these attacks cannot be seen in a single sample. A drifting sensor looks normal at every instant; what gives it away is that the error keeps growing. A replay looks like perfectly healthy data; what gives it away is that it was healthy data *from earlier*. Detecting them requires memory of the past.

A feed-forward network could get that memory from a sliding window of the last N samples, but on a microcontroller that means storing N × features values and re-processing all of them every period. A recurrent network instead keeps a small **hidden state** `h`, a vector that it updates once per sample:

```text
h_t = f(x_t, h_{t-1})        y_t = g(h_t)
```

Its cost per period is constant, its memory is just `h`, and it never needs a buffer of past inputs. That fits a control loop that already runs once per period.

### The plain RNN and its problem

The simplest choice is `h_t = tanh(W x_t + U h_{t-1} + b)`. Training it uses **backpropagation through time (BPTT)**: unroll the recurrence over T steps, treat it as a T-layer network with shared weights, and apply the chain rule backwards. The gradient reaching step `t−k` has been multiplied by `U` and by a tanh derivative `k` times. Those factors are usually below 1, so the gradient shrinks exponentially (the *vanishing gradient* problem), and the network cannot learn dependencies longer than a few steps. A drift that takes 50 samples to become obvious is out of reach.

### The GRU

A Gated Recurrent Unit fixes this by letting the network choose, per hidden unit and per step, how much of the old state to keep. With σ the logistic sigmoid:

```text
r  = σ(Wi_r·x + Wh_r·h + b_r)                reset gate
z  = σ(Wi_z·x + Wh_z·h + b_z)                update gate
n  = tanh(Wi_n·x + b_in + r ⊙ (Wh_n·h + b_hn))   candidate state
h' = (1 − z) ⊙ n + z ⊙ h                     new state
```

- **Update gate `z`** (0..1 per unit): at `z ≈ 1` the unit copies its old value forward unchanged. That copy path has derivative ≈ 1, so gradients survive across many steps. This is how the network can hold "the residual has been positive for a while".
- **Reset gate `r`**: at `r ≈ 0` the candidate ignores the old state, letting a unit start fresh when the context changes (for example, after a rate change).
- **Candidate `n`**: the proposed new content, squashed to −1..1.

A GRU has three weight blocks where an LSTM has four and keeps one state vector where an LSTM keeps two, so it is roughly 25 % smaller for the same hidden size. That is why it was chosen for a kilobyte budget.

---

## 4. This RNN's structure

```text
 telemetry row ─► feature extractor ─► GRU cell ─► linear ─► softmax ─► alert rule
 (pump data)      15 features          24 hidden   9 logits  9 probs    threshold + debounce
                  28 B of state        96 B state
```

### 4.1 Input: 15 features

Raw readings are poor inputs: a flow of 105 ml/h is fine at a 100 ml/h setpoint and alarming at 20. The extractor ([`FeatureState`](../ml/common.py) in Python, [`ids_features()`](../firmware/src/ids_rnn.c) in C, written to be line-for-line identical) turns each row into residuals that are already scaled to roughly ±1, using only constants. No dataset statistics are shipped.

The flow scale is `s = 0.20·target + 15` ml/h, the same band the rule monitor uses. Residuals are clipped to ±8.

| # | Feature | Formula | Mainly exposes |
|---|---|---|---|
| 0 | `running` | state is RUNNING | context |
| 1 | `e_target` | (flow − target·running) / s | spike, drift |
| 2 | `e_model` | (flow − previous duty × 1200) / s | drift: the controller cuts duty to fight the fake reading, so measured flow and the motor model diverge |
| 3 | `d_flow` | (flow − previous flow) / s | spike, noise |
| 4 | `e_press` | (pressure − 8 − 0.02·flow) / 5 | spike and drift (they bias pressure inconsistently with flow); occlusion (large and positive) |
| 5 | `d_press` | (pressure − previous pressure) / 2 | spike, noise |
| 6 | `flow_same` | flow bit-identical to the previous frame | freeze |
| 7 | `press_same` | pressure bit-identical to the previous frame | freeze |
| 8 | `stale` | a frame counter did not pass its highest value seen | replay |
| 9 | `seq_gap` | frames skipped since the last good one, / 8, capped at 1 | end of a dropout |
| 10 | `missing` | fraction of the two sensors that did not answer | dropout |
| 11 | `alarm` | any alarm active | context |
| 12 | `mem_alarm` | alarm is MEM_CORRUPT | tamper |
| 13 | `log_target` | ln(1 + target) / 7 | context: noise matters more at low rates |
| 14 | `d_target` | setpoint changed this period | benign rate change vs. attack |

The extractor's own memory is 28 bytes: the previous flow, pressure, duty and target, and two frame-counter high-water marks.

### 4.2 The network

| Tensor | Shape | Count | Role |
|---|---|---|---|
| `Wi` | 72 × 15 | 1,080 | input weights, rows stacked as gates r, z, n (24 each) |
| `Wh` | 72 × 24 | 1,728 | recurrent weights, same stacking |
| `bi` | 72 | 72 | biases `b_r`, `b_z`, `b_in` |
| `bhn` | 24 | 24 | `b_hn`, the bias inside the reset-gated term |
| `Wo` | 9 × 24 | 216 | output layer |
| `bo` | 9 | 9 | output bias |
| | | **3,129** | |

The output is nine probabilities via softmax: `NONE, OCCLUSION, SPIKE, DRIFT, REPLAY, FREEZE, DROPOUT, NOISE, TAMPER`.

`OCCLUSION` is a physical fault, not an attack. It has its own class so the network must learn to tell a real blocked line from a spoofed one; the pump's occlusion alarm already handles the real thing.

### 4.3 From probabilities to a decision

```text
P(attack) = 1 − P(NONE) − P(OCCLUSION)
alert     = P(attack) > 0.50 for 3 consecutive periods
attack_cls = the most probable of the seven attack classes
```

The 3-period debounce suppresses single-sample blips, and sets a floor of 0.2 s on detection latency (the alert rises on the third consecutive sample, two periods after the first).

### 4.4 Quantisation to int8

In float32 the 3,129 parameters would take 12.5 kB. Instead each weight matrix is stored as 8-bit integers plus a scale:

```text
scale = max|w| / 127        q = round(w / scale), clipped to ±127        w ≈ q × scale
```

One scale is shared by each gate block of `Wi` and `Wh`, and one by `Wo`: 7 scales in all. Biases stay float32 because there are only 105 of them. Only the *weights* are quantised; the arithmetic on the device is still float (`acc += (float)q[j] * x[j]`, then one multiply by the scale), so the target needs an FPU or soft-float.

---

## 5. How it was trained

### Data

[`gen_dataset.py`](../ml/gen_dataset.py) scripts the real simulator with seeded random scenarios, each 5 minutes (3,000 samples):

- Infusion rate drawn log-uniformly from 5–600 ml/h; 15 % of runs have IDS enforcement on; 8 % have no events at all.
- Then a sequence of events with clean gaps between them: 70 % attacks (the 7 types equally likely, with random magnitude and duration), 22 % benign operations (rate change, pause/resume, stop/start), 8 % occlusion faults.
- Benign operations matter as much as attacks: they produce large transients the detector must learn *not* to flag.

| Split | Runs | Samples | Seeds | Used for |
|---|---|---|---|---|
| train | 240 | 720,000 | 1000– | fitting weights |
| val | 30 | 90,000 | 101000– | picking the best epoch and the alert threshold |
| test | 60 | 180,000 | 201000– | the reported numbers, touched once at the end |

About 72 % of samples are `NONE`; the rarest class, `TAMPER`, is 0.2 %.

### Procedure ([`train.py`](../ml/train.py), [`tinygru.py`](../ml/tinygru.py))

- **Loss**: per-sample cross-entropy, weighted by `1/√frequency` per class (normalised to mean 1) so rare attacks are not drowned out. `TAMPER` gets weight 11.2, `NONE` 0.54.
- **Stateful truncated BPTT**: 24 runs are processed side by side in chunks of 100 samples. Gradients flow back at most 100 steps, but the hidden state is carried from chunk to chunk, exactly as the device will run it: continuously, from a zero state at boot. The first chunk starts at a random offset so chunk boundaries move between epochs.
- **Optimiser**: Adam, learning rate 4e-3 with cosine decay, gradient-norm clipping at 1.0, weight decay 1e-5.
- **Initialisation**: orthogonal recurrent weights; update-gate bias set to 1 so units start out remembering.
- **Phase 1**: 30 epochs in float32. The epoch with the best validation macro-F1 is kept.
- **Phase 2, quantisation-aware training**: 8 more epochs at a quarter of the learning rate. The forward pass uses the quantised-then-dequantised weights, while gradients update the underlying float weights (the straight-through estimator). The network thereby adapts to its own rounding error.
- The BPTT code is hand-written, so it was verified by a gradient check: analytic gradients match finite differences to a relative error of 1e-6 (`tinygru.grad_check()`).

---

## 6. Is the C code really the same model?

Two implementations of the same maths can silently disagree, so `export_c.py` writes golden vectors: the first 205 rows of test run `run_201000` (the first attack starts at row 85), with the raw inputs, the features Python computed, and the probabilities from Python's int8 model. `IdsRnnTest` feeds the same inputs to the C code and requires:

- every feature within 1e-4,
- every class probability within 2e-3 at every row,
- the alert flag to match (rows within 0.005 of the threshold are skipped as too close to call).

The tolerance exists because Python computes features in float64 and C in float32.

---

## 7. How memory was measured

### Flash for the parameters: 3,472 B (exact)

This is a byte count of the arrays that are stored, not an estimate. `tinygru.model_bytes()` sums the sizes of the exported arrays:

| Part | Calculation | Bytes |
|---|---|---|
| int8 weights | 1,080 + 1,728 + 216 | 3,024 |
| float32 scales | 7 × 4 | 28 |
| float32 biases | (72 + 24 + 9) × 4 | 420 |
| **Total** | | **3,472** |

It is enforced three times: `train.py` prints OK / OVER BUDGET against 6,144 B, `export_c.py` refuses to export above the budget, and `test_ids_rnn.cpp` has a `static_assert(IDS_MODEL_BYTES <= 6144)`.

### Flash for code and libraries: measured for Cortex-M4F

Built with `arm-none-eabi-gcc` 13.3.0 (`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard`, newlib-nano, `--gc-sections`). Two otherwise identical images are compared, one that runs the detector and one that doesn't, so the difference counts everything the detector causes to be linked. Reproduce with [firmware/mcu/measure_size.py](../firmware/mcu/measure_size.py).

| Part | `-Os` | `-O2` |
|---|---|---|
| Model parameters | 3,468 B | 3,468 B |
| Detector code (`ids_rnn.c`), including its own `exp` and `log` | 1,830 B | 2,182 B |
| C library code (libm, libc) | 0 B | 0 B |
| Probe loop growth | 24 B | 36 B |
| **Total flash** | **5,322 B** | **5,686 B** |

Both are inside the 6,144 B budget; `measure_size.py` exits with an error if the total goes over. The parameters are 3,468 B rather than 3,472 B because the compiler folded the single `Wo` scale into the code as an immediate.

**How it got under budget.** The first ARM build was 7,354 B, because newlib's `tanhf`, `expf` and `log1pf` added 2,228 B (plus 162 B for `memset`). They were replaced by two small functions inside `ids_rnn.c`:

| Function | Size (`-Os`) | Method | Measured error |
|---|---|---|---|
| `ids_expf` | 208 B | split x = k·ln2 + r, degree-6 polynomial for e^r, then set the exponent bits for 2^k | < 2e-6 relative over −87..88 (typically 4e-8) |
| `ids_logf` | 148 B | split x = 2^e·m, then ln(m) = 2·atanh((m−1)/(m+1)) as a 4-term series | < 2e-6 absolute over 1..2000 |

Sigmoid and tanh are computed from `ids_expf` (`1/(1+e^-a)` and `2/(1+e^-2a) − 1`), and `ids_rnn_init` zeroes its state with a loop. The detector now makes no C-library calls at all. Because these functions are accurate to float precision, the trained model was **not** retrained: over the 205 golden rows the C probabilities differ from Python's by at most 5.4e-7 (the test tolerance is 2e-3), so the accuracy results in section 8 still apply. Two unit tests (`ExpMatchesLibm`, `LogMatchesLibm`) compare the functions with the standard library across their whole input range.

### RAM

| What | Bytes | How it was obtained |
|---|---|---|
| Persistent state, `sizeof(ids_rnn_t)` | 176 | compiled and printed `sizeof`: feature state 28 + hidden state 96 + probabilities 36 + score, counters and class fields 16. A test asserts it stays ≤ 256. |
| Stack arrays inside `ids_rnn_step()` | 672 | by arithmetic: features 15 + two 72-element gate buffers + 9 logits = 168 floats |
| Whole stack frame of `ids_rnn_step()` | 720 | `-fstack-usage` for Cortex-M4F at `-Os` (728 at `-O2`); the functions it calls add at most 24 B. Peak stack measured on the board by `bench.c` (pattern fill) once a Nucleo is connected. |

On Cortex-M4F the detector's static RAM is 168 B; `ids_rnn_t` packs to 168 B there versus 176 B on x86-64. The two gate buffers (576 B) are a simplicity choice and could be reduced by restructuring the loop.

### Compute per period

By counting loop iterations: 3,024 multiply-accumulates (one per int8 weight), 81 calls to `ids_expf` (48 sigmoids, 24 tanh, 9 for the softmax), 73 float divisions and 1 `ids_logf`. Cycle counts on the STM32 are measured by `firmware/mcu/bench.c` with the DWT cycle counter; they are **not yet recorded here** because no board was connected when this was written. See [firmware/mcu/README.md](../firmware/mcu/README.md).

---

## 8. How accuracy was measured

All figures are on the **test split**: 60 runs the model never saw during training or tuning, 5 hours of simulated time, evaluated with the int8 weights and a hidden state that starts at zero and runs continuously through each recording.

### Per-sample metrics

Every 100 ms sample is classified by its most probable class and compared with the label.

- **Confusion matrix**: rows are the true class, normalised per row (in `ml/out/report.txt`).
- **Macro-F1 = 0.966**: F1 computed per class and averaged with equal weight, so the rare classes count as much as `NONE`. Plain accuracy would be misleading here, because always answering `NONE` would score 72 %.
- The float32 model scores 0.966 as well: quantisation cost nothing measurable.

Per-sample scores understate usefulness, though. The first samples of a slow drift are genuinely indistinguishable from normal, yet they are labelled `DRIFT`, so no detector can get them right. What matters operationally is whether each *attack* is caught and how fast.

### Event-level metrics

Definitions, as implemented in `event_metrics()`:

- An **episode** is a contiguous stretch of samples with the same attack label. The test split has 533.
- An episode is **detected** if the alert is on at any sample from its start to 20 samples (2 s) after its end.
- **Latency** is the time from the episode's first sample to the first alert sample. The table reports the median over detected episodes.
- **Type accuracy** is the fraction of detected episodes where the attack class named most often during the alert is the true one.
- A **false alert** is an alert turning on (a rising edge) outside every episode and its 2 s grace period. Alerts during an occlusion count as false. The rate is false alerts divided by the attack-free time, 3.5 h.

One leniency to be aware of: if an alert is still on from a previous episode when a new one starts, the new one counts as detected with zero latency. The scenario generator leaves at least 4 s between the end of one attack and the start of the next, so this should be rare.

### Choosing the threshold

The threshold was tuned on the validation split, never on test: it is the lowest value on a 0.05 grid whose validation false-alert rate is at most 1 per hour, which gave 0.50. On test the rate came out at 1.43 per hour. That is 5 alerts in 3.5 hours, so the figure has a wide uncertainty; a handful of events decides it.

### Results

| Attack | Episodes | RNN recall | RNN median latency | Type accuracy | Rule monitor recall | Rule latency |
|---|---|---|---|---|---|---|
| SPIKE | 71 | 0.958 | 0.2 s | 1.000 | 1.000 | 0.0 s |
| DRIFT | 72 | 1.000 | 0.7 s | 1.000 | 0.861 | 5.4 s |
| REPLAY | 71 | 1.000 | 0.2 s | 1.000 | 1.000 | 0.0 s |
| FREEZE | 81 | 1.000 | 0.2 s | 1.000 | 1.000 | 0.7 s |
| DROPOUT | 76 | 0.987 | 0.2 s | 1.000 | 1.000 | 0.0 s |
| NOISE | 87 | 1.000 | 0.3 s | 1.000 | 0.816 | 0.2 s |
| TAMPER | 75 | 1.000 | 0.2 s | 0.733 | 0.000 | – |
| **Overall** | 533 | **0.992** | | | 0.811 | |
| False alerts | | 5 (1.43/h) | | | 2 (0.57/h) | |

### The baseline

The rule monitor is scored with the same episode definitions, treating any `anomaly` flag as an alert with no debounce. That is why it shows 0.0 s latencies where the RNN's debounce costs 0.2 s. It scores zero on `TAMPER` because the monitor only inspects sensor frames; the firmware catches tampering separately through the `MEM_CORRUPT` alarm, which this baseline does not count.

### What these numbers do and do not show

- **The RNN wins on slow and statistical attacks** (drift, noise), where fixed thresholds are weakest, and it names the attack type.
- **The rule monitor is better on spikes and has fewer false alerts.** The RNN missed 3 of 71 spikes and 1 of 76 dropouts. These were not individually inspected; the generator does produce spikes as small as 15 ml/h for 2 samples, and a 2-sample event cannot satisfy a 3-sample debounce unless the alert outlasts it.
- **TAMPER detection is nearly free.** The network sees the `mem_alarm` feature, which the firmware's own redundancy check sets. Its 73 % type accuracy was not investigated.
- **Everything is simulated.** Training and test data come from the same plant model, noise model and attack implementations. The test split measures generalisation to new random scenarios, not to a real pump or to an attacker who adapts, for instance by drifting slowly enough to stay inside the noise.
- **Labels have some unavoidable noise.** An attack is labelled from its first sample even when it is not yet observable, and after a tamper is cleared the pump stays in alarm while the label returns to `NONE`.
- **One training run, one seed.** No repeated runs were done, so there is no variance estimate.

---

## 9. Where to look in the code

| Question | File |
|---|---|
| What exactly goes into the model? | [ml/common.py](../ml/common.py) (`FeatureState.step`) |
| How are scenarios randomised? | [ml/gen_dataset.py](../ml/gen_dataset.py) (`scenario`) |
| GRU forward, BPTT, quantisation | [ml/tinygru.py](../ml/tinygru.py) |
| Training loop and every metric | [ml/train.py](../ml/train.py) |
| The trained weights | [firmware/include/fw/ids_model.h](../firmware/include/fw/ids_model.h) (generated) |
| On-device inference | [firmware/src/ids_rnn.c](../firmware/src/ids_rnn.c) |
| C-vs-Python parity test | [firmware/tests/test_ids_rnn.cpp](../firmware/tests/test_ids_rnn.cpp) |
| Last training log and metrics | `ml/out/report.txt`, `ml/out/metrics.json` |
