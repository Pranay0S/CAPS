"""Shared definitions for the on-device attack detector (IDS-RNN).

Data contract
-------------
A recording is two row-aligned CSV files, exactly as gui/server.js writes them:

  *_telemetry.csv  PUMP_COLUMNS  - only what the pump firmware produces/observes.
  *_labels.csv     LABEL_COLUMNS - harness ground truth, used as training targets.

Model inputs are computed from the telemetry file *only*. `FeatureState` is a
streaming implementation (one row at a time, O(1) state) that is mirrored line
for line by firmware/src/ids_rnn.c, so the network sees identical inputs on the
device.
"""
from __future__ import annotations

import csv
import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np

# Keep in sync with PUMP_COLUMNS / LABEL_COLUMNS in gui/lib/protocol.js.
PUMP_COLUMNS = [
    "tick_ms", "state", "alarm", "target_rate", "flow_meas", "flow_seq", "flow_ok",
    "pressure_meas", "pressure_seq", "pressure_ok", "volume_est", "vtbi", "duty",
]
LABEL_COLUMNS = [
    "tick_ms", "attack", "fault", "anomaly", "flow_true", "pressure_true", "volume_true",
]

# Output classes. NONE and OCCLUSION (a genuine physical fault) are "benign":
# the pump's own safety logic handles occlusion, and the detector must not
# mistake it for an attack. Everything else is an attack the firmware should
# act on.
CLASSES = ["NONE", "OCCLUSION", "SPIKE", "DRIFT", "REPLAY", "FREEZE", "DROPOUT", "NOISE", "TAMPER"]
BENIGN = {0, 1}
CLASS_INDEX = {c: i for i, c in enumerate(CLASSES)}

# Firmware constants the feature extractor relies on (fw/fw_config.h).
NOMINAL_MAX_FLOW = 1200.0   # PUMP_NOMINAL_MAX_FLOW, ml/h at 100 % duty
ROC_FLOW_BASE = 15.0        # MON_ROC_FLOW_BASE
ROC_FLOW_FRAC = 0.20        # MON_ROC_FLOW_FRAC
LINE_BASE_KPA = 8.0         # nominal line pressure at zero flow
LINE_KPA_PER_MLH = 0.02     # nominal pressure rise with flow
FEAT_CLIP = 8.0

FEATURES = [
    "running",      # state == RUNNING
    "e_target",     # (flow - expected) / s   expected = target if running else 0
    "e_model",      # (flow - duty_prev * 1200) / s   motor-model residual
    "d_flow",       # (flow - flow_prev) / s
    "e_press",      # (press - 8 - 0.02 * flow) / 5   pressure/flow consistency
    "d_press",      # (press - press_prev) / 2
    "flow_same",    # flow bit-identical to the previous frame
    "press_same",   # pressure bit-identical to the previous frame
    "stale",        # a frame counter did not move past its high-water mark
    "seq_gap",      # frames skipped since the last good frame (0..1)
    "missing",      # fraction of sensors that did not answer
    "alarm",        # pump in any alarm
    "mem_alarm",    # alarm == MEM_CORRUPT
    "log_target",   # log(1 + target) / 7
    "d_target",     # setpoint changed this period
]
N_FEATURES = len(FEATURES)

STATE_RUNNING = "RUNNING"


def _clip(v: float) -> float:
    return FEAT_CLIP if v > FEAT_CLIP else (-FEAT_CLIP if v < -FEAT_CLIP else v)


class _SeqTracker:
    """High-water mark of a 16-bit rolling frame counter."""

    __slots__ = ("have", "hw")

    def __init__(self) -> None:
        self.have = False
        self.hw = 0

    def update(self, seq: int) -> tuple[float, int]:
        """Returns (stale, forward_step). forward_step is 0 when stale."""
        if not self.have:
            self.have, self.hw = True, seq
            return 0.0, 1
        d = (seq - self.hw) & 0xFFFF
        if d == 0 or d >= 0x8000:
            return 1.0, 0
        self.hw = seq
        return 0.0, d


class FeatureState:
    """Streaming feature extractor. Mirror of ids_feat_t / ids_features() in C."""

    def __init__(self) -> None:
        self.have_prev = False
        self.flow_prev = 0.0
        self.press_prev = 0.0
        self.duty_prev = 0.0
        self.target_prev = 0.0
        self.fseq = _SeqTracker()
        self.pseq = _SeqTracker()

    def step(self, state: str, alarm: str, target: float, flow: float, flow_seq: int, flow_ok: bool,
             press: float, press_seq: int, press_ok: bool, duty: float) -> list[float]:
        running = 1.0 if state == STATE_RUNNING else 0.0
        s = ROC_FLOW_FRAC * target + ROC_FLOW_BASE

        stale, gap = 0.0, 0
        if flow_ok:
            st, d = self.fseq.update(flow_seq)
            stale, gap = max(stale, st), max(gap, d)
        if press_ok:
            st, d = self.pseq.update(press_seq)
            stale, gap = max(stale, st), max(gap, d)
        seq_gap = min(max(gap - 1, 0) / 8.0, 1.0)

        if self.have_prev:
            d_flow = _clip((flow - self.flow_prev) / s)
            d_press = _clip((press - self.press_prev) / 2.0)
            flow_same = 1.0 if (flow_ok and flow == self.flow_prev) else 0.0
            press_same = 1.0 if (press_ok and press == self.press_prev) else 0.0
            d_target = 1.0 if target != self.target_prev else 0.0
        else:
            d_flow = d_press = flow_same = press_same = d_target = 0.0

        f = [
            running,
            _clip((flow - target * running) / s),
            _clip((flow - self.duty_prev * NOMINAL_MAX_FLOW) / s),
            d_flow,
            _clip((press - LINE_BASE_KPA - LINE_KPA_PER_MLH * flow) / 5.0),
            d_press,
            flow_same,
            press_same,
            stale,
            seq_gap,
            ((0.0 if flow_ok else 1.0) + (0.0 if press_ok else 1.0)) * 0.5,
            0.0 if alarm == "NONE" else 1.0,
            1.0 if alarm == "MEM_CORRUPT" else 0.0,
            math.log1p(max(target, 0.0)) / 7.0,
            d_target,
        ]
        self.have_prev = True
        self.flow_prev, self.press_prev = flow, press
        self.duty_prev, self.target_prev = duty, target
        return f


# ---------------------------------------------------------------------------
# Recording I/O


@dataclass
class Recording:
    name: str
    tick_ms: np.ndarray        # (T,)
    x: np.ndarray              # (T, N_FEATURES) float32
    y: np.ndarray              # (T,) int64 class index
    rule_flags: np.ndarray     # (T,) bool  rule-based monitor fired (baseline)


def label_of(attack: str, fault: str) -> int:
    if attack and attack != "NONE":
        return CLASS_INDEX[attack]
    if fault == "OCCLUSION":
        return CLASS_INDEX["OCCLUSION"]
    return 0


def _read_rows(path: Path) -> list[dict]:
    with open(path, newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def features_from_rows(rows: list[dict]) -> np.ndarray:
    fs = FeatureState()
    out = np.empty((len(rows), N_FEATURES), np.float32)
    for i, r in enumerate(rows):
        out[i] = fs.step(
            r["state"], r["alarm"], float(r["target_rate"]),
            float(r["flow_meas"]), int(r["flow_seq"]), r["flow_ok"] == "1",
            float(r["pressure_meas"]), int(r["pressure_seq"]), r["pressure_ok"] == "1",
            float(r["duty"]),
        )
    return out


def load_recording(telemetry_csv: str | Path) -> Recording:
    """Load a *_telemetry.csv and its sibling *_labels.csv."""
    tpath = Path(telemetry_csv)
    lpath = tpath.with_name(tpath.name.replace("_telemetry.csv", "_labels.csv"))
    trows = _read_rows(tpath)
    missing = [c for c in PUMP_COLUMNS if trows and c not in trows[0]]
    if missing:
        raise ValueError(f"{tpath.name}: missing pump columns {missing} (recorded by an older simulator?)")
    lrows = _read_rows(lpath)
    if len(lrows) != len(trows):
        raise ValueError(f"{tpath.name}: {len(trows)} telemetry rows but {len(lrows)} label rows")
    x = features_from_rows(trows)
    y = np.array([label_of(r["attack"], r.get("fault", "NONE")) for r in lrows], np.int64)
    rule = np.array([r.get("anomaly", "NONE") != "NONE" for r in lrows], bool)
    ticks = np.array([int(r["tick_ms"]) for r in trows], np.int64)
    return Recording(tpath.name.replace("_telemetry.csv", ""), ticks, x, y, rule)


def load_dir(directory: str | Path) -> list[Recording]:
    paths = sorted(Path(directory).glob("*_telemetry.csv"))
    if not paths:
        raise FileNotFoundError(f"no *_telemetry.csv files in {directory}")
    return [load_recording(p) for p in paths]


def cached_dir(directory: str | Path) -> list[Recording]:
    """load_dir with an .npz cache next to the CSVs (features are slow-ish in pure Python)."""
    directory = Path(directory)
    cache = directory / "_features.npz"
    newest = max(p.stat().st_mtime for p in directory.glob("*.csv"))
    if cache.exists() and cache.stat().st_mtime > newest:
        z = np.load(cache, allow_pickle=False)
        if list(z["features"]) == FEATURES:
            recs = []
            for i, name in enumerate(z["names"]):
                sl = slice(z["offsets"][i], z["offsets"][i + 1])
                recs.append(Recording(str(name), z["tick_ms"][sl], z["x"][sl], z["y"][sl], z["rule"][sl]))
            return recs
    recs = load_dir(directory)
    offsets = np.cumsum([0] + [len(r.y) for r in recs])
    np.savez_compressed(
        cache, features=np.array(FEATURES), names=np.array([r.name for r in recs]), offsets=offsets,
        tick_ms=np.concatenate([r.tick_ms for r in recs]), x=np.concatenate([r.x for r in recs]),
        y=np.concatenate([r.y for r in recs]), rule=np.concatenate([r.rule_flags for r in recs]),
    )
    return recs
