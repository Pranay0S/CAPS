#!/usr/bin/env python3
"""Train the tiny GRU attack detector, quantise it to int8 and evaluate it.

  python ml/train.py                     # train on ml/data/{train,val}, test on ml/data/test
  python ml/train.py --hidden 16         # smaller model
  python ml/train.py --eval-only         # re-evaluate ml/out/ids_gru.npz

Training is stateful truncated BPTT: B recordings are processed side by side
in chunks of --chunk samples and the hidden state is carried across chunks,
which matches how the firmware runs the network (one step per control period,
forever, from a zero state at boot). The last --qat-epochs epochs train
through int8 fake-quantisation so the deployed int8 model loses little
accuracy.

Detection rule evaluated here (and implemented in C):
  attack score  a = 1 - P(NONE) - P(OCCLUSION)
  ALERT when a > threshold for --debounce consecutive periods.
The threshold is picked on the validation set as the most sensitive one with
at most --max-fa false alerts per hour of attack-free operation.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tinygru as G  # noqa: E402
from common import BENIGN, CLASSES, FEATURES, N_FEATURES, Recording, cached_dir  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PERIOD_S = 0.1
GRACE = 20            # samples after an attack ends in which an alert still counts as a detection
BENIGN_IDX = np.array(sorted(BENIGN))


# ---------------------------------------------------------------------------
# batching


def stack(recs: list[Recording]) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Pad recordings to a common length -> x (T,B,F), y (T,B), mask (T,B)."""
    T = max(len(r.y) for r in recs)
    x = np.zeros((T, len(recs), N_FEATURES), np.float32)
    y = np.zeros((T, len(recs)), np.int64)
    m = np.zeros((T, len(recs)), bool)
    for b, r in enumerate(recs):
        x[: len(r.y), b], y[: len(r.y), b], m[: len(r.y), b] = r.x, r.y, True
    return x, y, m


def predict(p, recs: list[Recording], chunk: int = 600) -> list[np.ndarray]:
    """Streaming probabilities for each recording, starting from h = 0."""
    x, _, _ = stack(recs)
    h = np.zeros((len(recs), p["bhn"].shape[0]), np.float32)
    out = []
    for s in range(0, x.shape[0], chunk):
        logits, h, _ = G.forward(p, x[s:s + chunk], h)
        out.append(G.softmax(logits))
    probs = np.concatenate(out)
    return [probs[: len(r.y), b] for b, r in enumerate(recs)]


def train(p, train_recs, val_recs, weights, args, rng, qat: bool, epochs: int, lr: float, log):
    opt = G.Adam(p, lr=lr)
    best = (-1.0, {k: v.copy() for k, v in p.items()})
    for ep in range(epochs):
        t0 = time.time()
        order = rng.permutation(len(train_recs))
        lr_now = lr * 0.5 * (1 + np.cos(np.pi * ep / max(epochs, 1)))   # cosine decay
        opt.lr = lr_now
        losses = []
        for g0 in range(0, len(order), args.batch):
            x, y, m = stack([train_recs[i] for i in order[g0:g0 + args.batch]])
            h = np.zeros((x.shape[1], args.hidden), np.float32)
            # random phase so chunk boundaries don't always fall on the same samples
            start = int(rng.integers(0, args.chunk))
            if start:
                q = G.fake_quant(p) if qat else p
                _, h, _ = G.forward(q, x[:start], h)
            for s in range(start, x.shape[0], args.chunk):
                xs, ys, ms = x[s:s + args.chunk], y[s:s + args.chunk], m[s:s + args.chunk]
                q = G.fake_quant(p) if qat else p
                logits, h, cache = G.forward(q, xs, h)
                loss, d = G.weighted_ce(logits, ys, weights)
                d *= ms[..., None]
                grads = G.backward(q, cache, d)        # straight-through: apply to float weights
                opt.step(p, grads, clip=args.clip)
                losses.append(loss)
        q = G.fake_quant(p) if qat else p
        score = macro_f1(val_recs, predict(q, val_recs))
        tag = "qat" if qat else "fp32"
        log(f"[{tag}] epoch {ep + 1:3d}/{epochs}  loss {np.mean(losses):.4f}  val macro-F1 {score:.4f}  "
            f"lr {lr_now:.2e}  {time.time() - t0:.1f}s")
        if score > best[0]:
            best = (score, {k: v.copy() for k, v in p.items()})
    return best[1]


# ---------------------------------------------------------------------------
# metrics


def confusion(recs, probs) -> np.ndarray:
    C = len(CLASSES)
    cm = np.zeros((C, C), np.int64)
    for r, pr in zip(recs, probs):
        np.add.at(cm, (r.y, pr.argmax(1)), 1)
    return cm


def macro_f1(recs, probs) -> float:
    cm = confusion(recs, probs)
    tp = np.diag(cm).astype(float)
    prec = tp / np.maximum(cm.sum(0), 1)
    rec = tp / np.maximum(cm.sum(1), 1)
    f1 = 2 * prec * rec / np.maximum(prec + rec, 1e-9)
    present = cm.sum(1) > 0
    return float(f1[present].mean())


def debounce(flag: np.ndarray, k: int) -> np.ndarray:
    """True once `flag` has been true for k consecutive samples (C: counter)."""
    out = np.zeros_like(flag)
    run = 0
    for i, f in enumerate(flag):
        run = run + 1 if f else 0
        out[i] = run >= k
    return out


def episodes(y: np.ndarray) -> list[tuple[int, int, int]]:
    """Contiguous attack segments (start, end_exclusive, class)."""
    out, i = [], 0
    while i < len(y):
        if y[i] in BENIGN:
            i += 1
            continue
        j = i
        while j < len(y) and y[j] == y[i]:
            j += 1
        out.append((i, j, int(y[i])))
        i = j
    return out


def event_metrics(recs, alerts: list[np.ndarray], types: list[np.ndarray] | None = None) -> dict:
    per = {c: {"n": 0, "hit": 0, "lat": [], "type_ok": 0} for c in CLASSES if CLASSES.index(c) not in BENIGN}
    fa, benign_s = 0, 0.0
    for r, al, ty in zip(recs, alerts, types or [None] * len(recs)):
        covered = np.zeros(len(r.y), bool)            # attack segment + grace
        for s, e, c in episodes(r.y):
            covered[s:min(e + GRACE, len(r.y))] = True
            d = per[CLASSES[c]]
            d["n"] += 1
            hits = np.flatnonzero(al[s:min(e + GRACE, len(r.y))])
            if hits.size:
                d["hit"] += 1
                d["lat"].append(int(hits[0]))
                if ty is not None:
                    seg = ty[s:e][al[s:e]]
                    if seg.size and np.bincount(seg, minlength=len(CLASSES)).argmax() == c:
                        d["type_ok"] += 1
        rising = np.flatnonzero(al & ~np.r_[False, al[:-1]])
        fa += int((~covered[rising]).sum())
        benign_s += float((~covered).sum()) * PERIOD_S
    tot_n = sum(d["n"] for d in per.values())
    tot_hit = sum(d["hit"] for d in per.values())
    rows = {}
    for c, d in per.items():
        rows[c] = {
            "episodes": d["n"],
            "detected": d["hit"],
            "recall": d["hit"] / d["n"] if d["n"] else None,
            "median_latency_s": float(np.median(d["lat"])) * PERIOD_S if d["lat"] else None,
            "type_accuracy": d["type_ok"] / d["hit"] if (d["hit"] and types is not None) else None,
        }
    return {
        "per_attack": rows,
        "recall": tot_hit / max(tot_n, 1),
        "false_alerts": fa,
        "false_alerts_per_hour": fa / max(benign_s / 3600.0, 1e-9),
        "benign_hours": benign_s / 3600.0,
    }


def model_alerts(probs, thr: float, k: int):
    alerts, types = [], []
    for pr in probs:
        score = 1.0 - pr[:, BENIGN_IDX].sum(1)
        alerts.append(debounce(score > thr, k))
        att = pr.copy()
        att[:, BENIGN_IDX] = -1
        types.append(att.argmax(1))
    return alerts, types


def pick_threshold(recs, probs, k: int, max_fa: float) -> float:
    best = 0.95
    for thr in np.round(np.arange(0.95, 0.04, -0.05), 2):
        m = event_metrics(recs, model_alerts(probs, float(thr), k)[0])
        if m["false_alerts_per_hour"] <= max_fa:
            best = float(thr)
    return best


def format_report(title: str, m: dict) -> str:
    lines = [title, f"  {'attack':8s} {'episodes':>8s} {'detected':>8s} {'recall':>7s} {'latency':>8s} {'type-acc':>8s}"]
    for c, r in m["per_attack"].items():
        rec = f"{r['recall']:.3f}" if r["recall"] is not None else "-"
        lat = f"{r['median_latency_s']:.1f}s" if r["median_latency_s"] is not None else "-"
        ta = f"{r['type_accuracy']:.3f}" if r["type_accuracy"] is not None else "-"
        lines.append(f"  {c:8s} {r['episodes']:8d} {r['detected']:8d} {rec:>7s} {lat:>8s} {ta:>8s}")
    lines.append(f"  overall recall {m['recall']:.3f}   false alerts {m['false_alerts']} "
                 f"({m['false_alerts_per_hour']:.2f}/h over {m['benign_hours']:.1f} h attack-free)")
    return "\n".join(lines)


def format_confusion(cm: np.ndarray) -> str:
    w = 9
    head = " " * 11 + "".join(f"{c[:8]:>{w}s}" for c in CLASSES)
    rows = [head]
    for i, c in enumerate(CLASSES):
        tot = max(cm[i].sum(), 1)
        rows.append(f"  {c[:8]:8s} " + "".join(f"{cm[i, j] / tot:>{w}.3f}" for j in range(len(CLASSES))))
    return "\n".join(rows)


# ---------------------------------------------------------------------------


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--data", type=Path, default=ROOT / "ml" / "data")
    ap.add_argument("--out", type=Path, default=ROOT / "ml" / "out")
    ap.add_argument("--hidden", type=int, default=24)
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--qat-epochs", type=int, default=8)
    ap.add_argument("--batch", type=int, default=24)
    ap.add_argument("--chunk", type=int, default=100)
    ap.add_argument("--lr", type=float, default=4e-3)
    ap.add_argument("--clip", type=float, default=1.0)
    ap.add_argument("--debounce", type=int, default=3, help="consecutive periods before an alert")
    ap.add_argument("--max-fa", type=float, default=1.0, help="false alerts per hour allowed when picking the threshold")
    ap.add_argument("--budget", type=int, default=6144, help="flash budget for the model parameters, bytes")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--eval-only", action="store_true")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    log_lines: list[str] = []

    def log(s: str = "") -> None:
        print(s, flush=True)
        log_lines.append(s)

    train_recs = cached_dir(args.data / "train")
    val_recs = cached_dir(args.data / "val")
    test_recs = cached_dir(args.data / "test")
    log(f"data: {len(train_recs)} train / {len(val_recs)} val / {len(test_recs)} test recordings, "
        f"{sum(len(r.y) for r in train_recs)} training samples, {N_FEATURES} features")

    model_path = args.out / "ids_gru.npz"
    if args.eval_only:
        z = dict(np.load(model_path))
        q = {k: z[k] for k in ("Wi_q", "Wi_s", "Wh_q", "Wh_s", "Wo_q", "Wo_s", "bi", "bhn", "bo")}
        args.hidden = int(z["bhn"].shape[0])
    else:
        rng = np.random.default_rng(args.seed)
        y_all = np.concatenate([r.y for r in train_recs])
        freq = np.bincount(y_all, minlength=len(CLASSES)) / len(y_all)
        weights = np.where(freq > 0, 1.0 / np.sqrt(np.maximum(freq, 1e-6)), 0.0)
        weights = (weights / (weights[y_all].mean())).astype(np.float32)
        log("class weights: " + ", ".join(f"{c}={w:.2f}" for c, w in zip(CLASSES, weights)))

        p = G.init_params(N_FEATURES, args.hidden, len(CLASSES), rng)
        log(f"GRU: {N_FEATURES} in -> {args.hidden} hidden -> {len(CLASSES)} classes, {G.n_params(p)} parameters")
        p = train(p, train_recs, val_recs, weights, args, rng, False, args.epochs, args.lr, log)
        if args.qat_epochs:
            p = train(p, train_recs, val_recs, weights, args, rng, True, args.qat_epochs, args.lr * 0.25, log)
        q = G.quantize(p)
        fp_probs = predict(p, test_recs)

    size = G.model_bytes(q)
    deq = G.dequantize(q)
    val_probs = predict(deq, val_recs)
    thr = pick_threshold(val_recs, val_probs, args.debounce, args.max_fa)

    probs = predict(deq, test_recs)
    alerts, types = model_alerts(probs, thr, args.debounce)
    m_int8 = event_metrics(test_recs, alerts, types)
    baseline = event_metrics(test_recs, [r.rule_flags for r in test_recs])
    cm = confusion(test_recs, probs)

    log()
    log(f"model flash: {size['total']} B  (int8 weights {size['int8_weights']} B + scales {size['scales']} B "
        f"+ float biases {size['float_biases']} B)   budget {args.budget} B -> "
        f"{'OK' if size['total'] <= args.budget else 'OVER BUDGET'}")
    H, C = args.hidden, len(CLASSES)
    log(f"RAM at run time: persistent state {28 + 4 * H + 4 * C + 16} B (sizeof(ids_rnn_t): features 28 + hidden "
        f"{4 * H} + probabilities {4 * C} + flags 16); stack arrays per step "
        f"{4 * (N_FEATURES + 6 * H + C)} B (x, two 3H gate buffers, logits)")
    log(f"alert rule: P(attack) > {thr:.2f} for {args.debounce} consecutive periods "
        f"(threshold chosen on val, <= {args.max_fa}/h false alerts)")
    log()
    log(f"per-sample macro-F1 (test): int8 {macro_f1(test_recs, probs):.4f}" +
        ("" if args.eval_only else f"   fp32 {macro_f1(test_recs, fp_probs):.4f}"))
    log("per-sample confusion (rows = truth, row-normalised):")
    log(format_confusion(cm))
    log()
    log(format_report("IDS-RNN int8, event level (test):", m_int8))
    log()
    log(format_report("Baseline: rule-based monitor (anomaly != NONE), event level (test):", baseline))

    if not args.eval_only:
        np.savez(model_path, **q, classes=np.array(CLASSES), features=np.array(FEATURES),
                 threshold=np.float32(thr), debounce=np.int32(args.debounce))
        log(f"\nsaved {model_path}")
    (args.out / "report.txt").write_text("\n".join(log_lines) + "\n", encoding="utf-8")
    (args.out / "metrics.json").write_text(json.dumps({
        "model_bytes": size, "hidden": args.hidden, "threshold": thr, "debounce": args.debounce,
        "test_macro_f1_int8": macro_f1(test_recs, probs), "ids_rnn": m_int8, "rule_monitor": baseline,
    }, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
