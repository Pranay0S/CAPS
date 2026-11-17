"""A tiny GRU classifier in pure numpy: forward, BPTT, Adam, int8 quantisation.

No deep-learning framework is needed, and the forward pass is written exactly
as the C inference code (firmware/src/ids_rnn.c) computes it:

    r  = sigmoid(Wi_r x + Wh_r h + b_r)
    z  = sigmoid(Wi_z x + Wh_z h + b_z)
    n  = tanh   (Wi_n x + b_in + r * (Wh_n h + b_hn))
    h' = (1 - z) * n + z * h
    logits = Wo h' + bo

Parameter layout (gates stacked r, z, n):
    Wi (3H, F)  Wh (3H, H)  bi (3H,)  bhn (H,)  Wo (C, H)  bo (C,)

Quantisation: every weight matrix is int8 with one float scale per gate block
(Wi_r, Wi_z, Wi_n, Wh_r, Wh_z, Wh_n, Wo -> 7 scales). Biases stay float32.
"""
from __future__ import annotations

import numpy as np

WEIGHTS = ("Wi", "Wh", "Wo")
PARAMS = ("Wi", "Wh", "bi", "bhn", "Wo", "bo")


def sigmoid(a: np.ndarray) -> np.ndarray:
    return 0.5 * (1.0 + np.tanh(0.5 * a))   # overflow-safe


def init_params(n_in: int, hidden: int, n_out: int, rng: np.random.Generator) -> dict[str, np.ndarray]:
    k = 1.0 / np.sqrt(hidden)
    p = {
        "Wi": rng.uniform(-k, k, (3 * hidden, n_in)),
        "Wh": np.concatenate([np.linalg.qr(rng.normal(size=(hidden, hidden)))[0] for _ in range(3)]),
        "bi": np.zeros(3 * hidden),
        "bhn": np.zeros(hidden),
        "Wo": rng.uniform(-k, k, (n_out, hidden)),
        "bo": np.zeros(n_out),
    }
    p["bi"][hidden:2 * hidden] = 1.0   # bias update gate towards remembering
    return {name: v.astype(np.float32) for name, v in p.items()}


def n_params(p: dict[str, np.ndarray]) -> int:
    return sum(v.size for v in p.values())


# ---------------------------------------------------------------------------
# Quantisation


def blocks(name: str, w: np.ndarray) -> list[np.ndarray]:
    """Row blocks that share one scale: the 3 gates of Wi/Wh, all of Wo."""
    if name in ("Wi", "Wh"):
        return np.split(w, 3, axis=0)
    return [w]


def quantize(p: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    """Returns {name_q: int8 matrix, name_s: per-block float32 scales} + float biases."""
    q: dict[str, np.ndarray] = {}
    for name in WEIGHTS:
        qs, scales = [], []
        for b in blocks(name, p[name]):
            s = max(float(np.abs(b).max()), 1e-8) / 127.0
            qs.append(np.clip(np.round(b / s), -127, 127).astype(np.int8))
            scales.append(s)
        q[name + "_q"] = np.concatenate(qs, axis=0)
        q[name + "_s"] = np.array(scales, np.float32)
    for name in ("bi", "bhn", "bo"):
        q[name] = p[name].astype(np.float32)
    return q


def dequantize(q: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    p = {}
    for name in WEIGHTS:
        parts = np.split(q[name + "_q"].astype(np.float32), len(q[name + "_s"]), axis=0)
        p[name] = np.concatenate([b * s for b, s in zip(parts, q[name + "_s"])], axis=0)
    for name in ("bi", "bhn", "bo"):
        p[name] = q[name].astype(np.float32)
    return p


def fake_quant(p: dict[str, np.ndarray]) -> dict[str, np.ndarray]:
    """Quantise-dequantise weights (straight-through estimator during training)."""
    return dequantize(quantize(p))


def model_bytes(q: dict[str, np.ndarray]) -> dict[str, int]:
    """Flash footprint of the quantised parameters."""
    w = sum(q[n + "_q"].nbytes for n in WEIGHTS)
    s = sum(q[n + "_s"].nbytes for n in WEIGHTS)
    b = sum(q[n].nbytes for n in ("bi", "bhn", "bo"))
    return {"int8_weights": w, "scales": s, "float_biases": b, "total": w + s + b}


# ---------------------------------------------------------------------------
# Forward / backward over a chunk of T steps for a batch of B streams


def forward(p, x, h0):
    """x (T,B,F), h0 (B,H) -> logits (T,B,C), hT, cache."""
    T, B, _ = x.shape
    H = h0.shape[1]
    Wi, Wh, bi, bhn, Wo, bo = (p[k] for k in PARAMS)
    xi = x @ Wi.T + bi                          # (T,B,3H) input part, all steps at once
    dt = np.result_type(x, Wh)
    hs = np.empty((T + 1, B, H), dt)
    hs[0] = h0
    r = np.empty((T, B, H), dt)
    z = np.empty_like(r)
    n = np.empty_like(r)
    hn = np.empty_like(r)
    for t in range(T):
        h = hs[t]
        ah = h @ Wh.T                            # (B,3H)
        r[t] = sigmoid(xi[t, :, :H] + ah[:, :H])
        z[t] = sigmoid(xi[t, :, H:2 * H] + ah[:, H:2 * H])
        hn[t] = ah[:, 2 * H:] + bhn
        n[t] = np.tanh(xi[t, :, 2 * H:] + r[t] * hn[t])
        hs[t + 1] = (1.0 - z[t]) * n[t] + z[t] * h
    logits = hs[1:] @ Wo.T + bo
    return logits, hs[-1], (x, hs, r, z, n, hn)


def backward(p, cache, dlogits):
    """Gradients of the loss given dL/dlogits (T,B,C). Truncated at the chunk start."""
    x, hs, r, z, n, hn = cache
    T, B, _ = x.shape
    H = hs.shape[2]
    Wh, Wo = p["Wh"], p["Wo"]
    g = {k: np.zeros_like(v) for k, v in p.items()}
    g["Wo"] = np.einsum("tbc,tbh->ch", dlogits, hs[1:])
    g["bo"] = dlogits.sum((0, 1))
    dh_out = dlogits @ Wo                         # (T,B,H)
    da_i = np.empty((T, B, 3 * H), hs.dtype)    # grads wrt input pre-activations
    dh = np.zeros((B, H), hs.dtype)
    for t in range(T - 1, -1, -1):
        dh = dh + dh_out[t]
        h = hs[t]
        dn = dh * (1.0 - z[t])
        dz = dh * (h - n[t])
        dan = dn * (1.0 - n[t] ** 2)
        dar = dan * hn[t] * r[t] * (1.0 - r[t])
        daz = dz * z[t] * (1.0 - z[t])
        dhn = dan * r[t]
        da_i[t, :, :H], da_i[t, :, H:2 * H], da_i[t, :, 2 * H:] = dar, daz, dan
        dah = np.concatenate([dar, daz, dhn], axis=1)   # grads wrt h @ Wh.T (+bhn)
        g["Wh"] += dah.T @ h
        g["bhn"] += dhn.sum(0)
        dh = dh * z[t] + dah @ Wh
    g["Wi"] = np.einsum("tbg,tbf->gf", da_i, x)
    g["bi"] = da_i.sum((0, 1))
    return g


def softmax(logits: np.ndarray) -> np.ndarray:
    e = np.exp(logits - logits.max(-1, keepdims=True))
    return e / e.sum(-1, keepdims=True)


def weighted_ce(logits, y, w):
    """Mean class-weighted cross-entropy and its gradient wrt logits."""
    prob = softmax(logits)
    T, B, C = logits.shape
    wy = w[y]                                    # (T,B)
    norm = wy.sum()
    picked = np.take_along_axis(prob, y[..., None], -1)[..., 0]
    loss = float(-(wy * np.log(picked + 1e-9)).sum() / norm)
    d = prob
    np.put_along_axis(d, y[..., None], np.take_along_axis(d, y[..., None], -1) - 1.0, -1)
    return loss, (d * (wy / norm)[..., None]).astype(np.float32)


class Adam:
    def __init__(self, params, lr=3e-3, b1=0.9, b2=0.999, eps=1e-8, wd=1e-5):
        self.lr, self.b1, self.b2, self.eps, self.wd = lr, b1, b2, eps, wd
        self.m = {k: np.zeros_like(v) for k, v in params.items()}
        self.v = {k: np.zeros_like(v) for k, v in params.items()}
        self.t = 0

    def step(self, params, grads, clip=1.0):
        norm = np.sqrt(sum(float((g ** 2).sum()) for g in grads.values()))
        scale = min(1.0, clip / (norm + 1e-12))
        self.t += 1
        for k in params:
            g = grads[k] * scale
            if k in WEIGHTS:
                g = g + self.wd * params[k]
            self.m[k] = self.b1 * self.m[k] + (1 - self.b1) * g
            self.v[k] = self.b2 * self.v[k] + (1 - self.b2) * g * g
            mh = self.m[k] / (1 - self.b1 ** self.t)
            vh = self.v[k] / (1 - self.b2 ** self.t)
            params[k] -= (self.lr * mh / (np.sqrt(vh) + self.eps)).astype(np.float32)
        return norm


def run_stream(p, x: np.ndarray) -> np.ndarray:
    """Streaming inference over one recording from a zero state. x (T,F) -> probs (T,C)."""
    H = p["bhn"].shape[0]
    probs = []
    h = np.zeros((1, H), np.float32)
    for s in range(0, len(x), 512):
        logits, h, _ = forward(p, x[s:s + 512, None, :], h)
        probs.append(softmax(logits[:, 0]))
    return np.concatenate(probs)


def grad_check(seed: int = 0) -> float:
    """Max relative error between analytic and numeric gradients (float64)."""
    rng = np.random.default_rng(seed)
    p = {k: v.astype(np.float64) for k, v in init_params(4, 5, 3, rng).items()}
    for k in p:
        p[k] += rng.normal(0, 0.3, p[k].shape)
    x = rng.normal(size=(6, 2, 4))
    y = rng.integers(0, 3, (6, 2))
    w = np.array([1.0, 2.0, 0.5])
    h0 = rng.normal(size=(2, 5)) * 0.5

    def f(pp):
        lo, _, c = forward(pp, x, h0)
        return weighted_ce(lo, y, w), c

    (_, d), cache = f(p)
    g = backward(p, cache, d.astype(np.float64))
    worst = 0.0
    for k in p:
        for idx in np.ndindex(p[k].shape):
            old = p[k][idx]
            p[k][idx] = old + 1e-5
            lp = f(p)[0][0]
            p[k][idx] = old - 1e-5
            lm = f(p)[0][0]
            p[k][idx] = old
            num = (lp - lm) / 2e-5
            worst = max(worst, abs(num - g[k][idx]) / max(1e-6, abs(num) + abs(g[k][idx])))
    return worst
