#!/usr/bin/env python3
"""Generate labelled IV-pump recordings by scripting the firmware simulator.

Each run is a random but reproducible scenario: an infusion at a random rate
with benign operations (rate changes, pauses, stops), physical occlusion
faults, and attacks of random type, size and duration. The simulator is run in
--fast mode and its stream is split exactly like the GUI does it:

  <out>/<split>/run_<seed>_telemetry.csv   pump-generated columns only
  <out>/<split>/run_<seed>_labels.csv      ground truth (attack, fault, ...)

  python ml/gen_dataset.py                       # default sizes
  python ml/gen_dataset.py --train 300 --minutes 5
"""
from __future__ import annotations

import argparse
import csv
import io
import math
import os
import random
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import LABEL_COLUMNS, PUMP_COLUMNS  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
EXE = "ivpump_sim.exe" if os.name == "nt" else "ivpump_sim"
PERIOD_MS = 100


def log_uniform(rng: random.Random, lo: float, hi: float) -> float:
    return math.exp(rng.uniform(math.log(lo), math.log(hi)))


def attack_command(rng: random.Random, kind: str, history: int) -> tuple[str, int]:
    """Returns (command, duration in samples)."""
    if kind == "spike":
        n = rng.randint(2, 20)
        return f"attack spike {rng.uniform(15, 150):.1f} {n}", n
    if kind == "drift":
        n = rng.randint(40, 300)
        return f"attack drift {log_uniform(rng, 0.1, 2.0):.3f} {n}", n
    if kind == "replay":
        n = rng.randint(30, 300)
        window = rng.randint(10, min(120, history))
        return f"attack replay {window} {n}", n
    if kind == "freeze":
        n = rng.randint(15, 200)
        return f"attack freeze 0 {n}", n
    if kind == "dropout":
        n = rng.randint(2, 40)
        return f"attack dropout 0 {n}", n
    if kind == "noise":
        n = rng.randint(15, 200)
        return f"attack noise {log_uniform(rng, 4, 60):.1f} {n}", n
    raise ValueError(kind)


ATTACKS = ["spike", "drift", "replay", "freeze", "dropout", "noise", "tamper"]


def scenario(seed: int, ticks: int) -> list[str]:
    """Build a scheduled command script ("@<ms> <cmd>")."""
    rng = random.Random(seed)
    cmds: list[tuple[int, str]] = []

    def at(tick: int, cmd: str) -> None:
        cmds.append((tick * PERIOD_MS, cmd))

    rate = log_uniform(rng, 5, 600)
    if rng.random() < 0.15:
        at(0, "ids on")
    at(0, f"rate {rate:.1f}")
    t = rng.randint(1, 40)
    at(t, "start")
    clean_run = rng.random() < 0.08   # some runs with no events at all, for false-alarm rates
    t += rng.randint(60, 200)         # let the attacker record some history first

    while t < ticks - 50:
        if clean_run:
            break
        roll = rng.random()
        if roll < 0.22:
            # benign operation (negative examples for the detector)
            op = rng.choice(["rate", "rate", "pause", "stop"])
            if op == "rate":
                rate = log_uniform(rng, 5, 600) if rng.random() < 0.6 else rate * rng.uniform(0.5, 1.6)
                rate = min(max(rate, 0.5), 999.0)
                at(t, f"rate {rate:.1f}")
                t += rng.randint(20, 150)
            else:
                at(t, op)
                t += rng.randint(10, 120)
                at(t, "start")
                t += rng.randint(20, 120)
        elif roll < 0.30:
            # physical occlusion fault, then recovery
            n = rng.randint(20, 150)
            at(t, "fault occlusion on")
            at(t + n, "fault occlusion off")
            t += n + rng.randint(30, 50)      # pressure decays over ~3 s
            at(t, "clear")
            at(t + 1, "start")
            t += rng.randint(30, 150)
        else:
            kind = rng.choice(ATTACKS)
            if kind == "tamper":
                n = rng.randint(1, 8)
                at(t, f"attack tamper {log_uniform(rng, 1, 999):.1f}")
                at(t + n, "attack clear")
                t += n + rng.randint(1, 20)
                at(t, "clear")
                at(t + 1, f"rate {rate:.1f}")   # clearing MEM_CORRUPT resets the rate
                at(t + 2, "start")
            else:
                cmd, n = attack_command(rng, kind, history=t)
                at(t, cmd)
                t += n + rng.randint(5, 30)
                at(t, "clear")                   # recover from any alarm the attack caused
                at(t + 1, "start")
            t += rng.randint(40, 250)

    cmds.sort(key=lambda c: c[0])
    return [f"@{ms} {c}" for ms, c in cmds]


def run_sim(exe: Path, seed: int, ticks: int, script: list[str]) -> tuple[list[str], list[list[str]]]:
    out = subprocess.run(
        [str(exe), "--fast", "--header", "--ticks", str(ticks), "--seed", str(seed)],
        input="\n".join(script) + "\n", capture_output=True, text=True, check=True,
    ).stdout
    header, rows = None, []
    for line in out.splitlines():
        if line.startswith("#TLM,"):
            header = line[5:].split(",")
        elif line.startswith("TLM,"):
            rows.append(line[4:].split(","))
    if header is None:
        raise RuntimeError("simulator did not print a #TLM header")
    return header, rows


def write_split(header: list[str], rows: list[list[str]], stem: Path) -> None:
    idx = {c: i for i, c in enumerate(header)}
    missing = [c for c in PUMP_COLUMNS + LABEL_COLUMNS if c not in idx]
    if missing:
        raise RuntimeError(f"simulator stream lacks {missing}; rebuild firmware")
    for suffix, cols in (("telemetry", PUMP_COLUMNS), ("labels", LABEL_COLUMNS)):
        buf = io.StringIO()
        w = csv.writer(buf, lineterminator="\r\n")
        w.writerow(cols)
        for r in rows:
            w.writerow([r[idx[c]] for c in cols])
        stem.with_name(f"{stem.name}_{suffix}.csv").write_text(buf.getvalue(), encoding="utf-8")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", type=Path, default=ROOT / "firmware" / "build" / EXE)
    ap.add_argument("--out", type=Path, default=ROOT / "ml" / "data")
    ap.add_argument("--train", type=int, default=240)
    ap.add_argument("--val", type=int, default=30)
    ap.add_argument("--test", type=int, default=60)
    ap.add_argument("--minutes", type=float, default=5.0, help="length of each run")
    ap.add_argument("--seed", type=int, default=1000, help="base seed; splits use disjoint seed ranges")
    args = ap.parse_args()

    if not args.bin.exists():
        sys.exit(f"simulator not found: {args.bin}\nbuild it: cmake -S firmware -B firmware/build -G Ninja && "
                 "cmake --build firmware/build")
    ticks = int(args.minutes * 60_000 / PERIOD_MS)
    base = {"train": args.seed, "val": args.seed + 100_000, "test": args.seed + 200_000}
    for split, count in (("train", args.train), ("val", args.val), ("test", args.test)):
        d = args.out / split
        d.mkdir(parents=True, exist_ok=True)
        for old in d.glob("*.csv"):
            old.unlink()
        for k in range(count):
            seed = base[split] + k
            header, rows = run_sim(args.bin, seed, ticks, scenario(seed, ticks))
            write_split(header, rows, d / f"run_{seed}")
        print(f"{split:5s}: {count} runs x {ticks} samples -> {d}")


if __name__ == "__main__":
    main()
