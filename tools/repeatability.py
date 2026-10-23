#!/usr/bin/env python3
"""Test-retest stability of the device's risk output under measurement noise (internal test set).

Every internal-test record is "measured" twice. A measurement is three 10-s windows, each passed through the
device chain (ml/dgx/edge_features.process_record: wearable noise at a given in-band SNR -> AD8232 filters
-> ESP32-S3 ADC in integer mV) with its own noise realisation, and scored with the deployed primary model.
Two read-outs are compared:

  single window : the tier of the first window (a device that reports one 10-s recording)
  device        : the firmware rule (ecg_risk.h): median score of the windows that pass the signal-quality
                  index, at least two needed, plus the borderline flag (good windows on both sides of a tier
                  boundary)

Reported per noise level: tier agreement between the two measurements, linearly weighted Cohen's kappa over
the ordered tiers low < elevated < high, within-person SD of the calibrated risk, no-result rate, AUROC, and
how many of the tier changes the borderline flag announces.

Limitation: the dataset holds one 10-s recording per person, so the windows of a simulated measurement share
the same beats and differ only in noise; beat-to-beat physiology between windows is not simulated.

    conda activate diagnostix
    python tools/repeatability.py            # writes ml/outputs/tables/table23_repeatability.csv and fig16
"""
from __future__ import annotations

import json
import sys
import warnings
from pathlib import Path

import joblib
import numpy as np
import pandas as pd
from joblib import Parallel, delayed
from sklearn.metrics import cohen_kappa_score, roc_auc_score

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ml"))
from dgx import config as C  # noqa: E402
from dgx import data  # noqa: E402
from dgx import edge_features as ef  # noqa: E402
from dgx import plotting as P  # noqa: E402

SNRS = [None, 20, 10, 5]           # None = electrodes clean, only the ADC noise (3 mV at the pin)
N_MEAS, N_WIN, MIN_GOOD = 2, 3, 2
TIER_CODE = {"low": 0, "elevated": 1, "high": 2}
N_BOOT = 1000


def window_features(x, idx, snr_i, snr):
    out = []
    for m in range(N_MEAS):
        for w in range(N_WIN):
            seed = int(np.random.SeedSequence([idx, snr_i, m, w]).generate_state(1)[0])
            f = ef.process_record(x, snr_db=snr, seed=seed, uv_per_lsb=C.UV_PER_LSB)
            out.append({k: f[k] for k in ef.EDGE_ALL} | {"sqi_ok": bool(f["sqi_ok"]), "rec": idx, "meas": m, "win": w})
    return out


def main():
    warnings.filterwarnings("ignore", "All-NaN slice", RuntimeWarning)  # records with no SQI-passing window
    spec = json.loads((C.MODEL_DIR / "feature_spec.json").read_text())["models"]["primary"]
    model = joblib.load(C.MODEL_DIR / "model_primary.joblib")
    th, platt = spec["tiers_on_score"], spec["platt"]

    part = data.load_participants()
    test = part[~part.development_data]
    ids = (C.CACHE_DIR / "raw_leadI_internal_ids.txt").read_text().split()
    X = np.load(C.CACHE_DIR / "raw_leadI_internal.npy", mmap_mode="r")
    pos = {e: i for i, e in enumerate(ids)}
    test = test[test.index.isin(pos)]
    rows = [pos[e] for e in test.index]
    y = test[C.LABEL].to_numpy(bool)
    print(f"internal test: {len(test)} records, {y.sum()} events")

    def tier(s):
        return np.where(s >= th["high"], 2, np.where(s >= th["elevated"], 1, 0))

    def risk(s):
        z = np.log(np.clip(s, 1e-6, 1 - 1e-6) / (1 - np.clip(s, 1e-6, 1 - 1e-6)))
        return 1 / (1 + np.exp(-(platt["a"] * z + platt["b"])))

    summary, per_record = [], {}
    for snr_i, snr in enumerate(SNRS):
        label = "ADC noise only" if snr is None else f"{snr} dB"
        feats = Parallel(n_jobs=-1, batch_size=8)(
            delayed(window_features)(np.asarray(X[r], dtype=float), k, snr_i, snr) for k, r in enumerate(rows))
        F = pd.DataFrame([f for rec in feats for f in rec])
        prof = test[C.CLINICAL].reset_index(drop=True)
        Z = F.join(prof, on="rec")
        F["score"] = model.predict_proba(Z[spec["inputs"]].to_numpy(float))[:, 1]
        F.loc[~F.sqi_ok, "score"] = np.nan

        n = len(test)
        S = F.pivot_table(index="rec", columns=["meas", "win"], values="score", dropna=False).reindex(range(n))
        S = S.to_numpy().reshape(n, N_MEAS, N_WIN)
        single = S[:, :, 0]                                          # first window of each measurement
        good = np.isfinite(S).sum(axis=2)
        dev = np.where(good >= MIN_GOOD, np.nanmedian(np.where(np.isfinite(S), S, np.nan), axis=2), np.nan)
        lo, hi = np.nanmin(S, axis=2), np.nanmax(S, axis=2)
        border = np.zeros_like(dev, dtype=bool)
        for t in (th["high"], th["elevated"]):
            border |= (lo < t) & (t <= hi)
        border &= np.isfinite(dev)

        for name, sc in (("single window", single), ("device (median of 3)", dev)):
            ok = np.isfinite(sc).all(axis=1)
            t1, t2 = tier(sc[ok, 0]), tier(sc[ok, 1])
            r1, r2 = risk(sc[ok, 0]), risk(sc[ok, 1])
            agree = (t1 == t2)
            rng = np.random.default_rng(0)
            boots = [agree[rng.integers(0, ok.sum(), ok.sum())].mean() for _ in range(N_BOOT)]
            first = np.isfinite(sc[:, 0])
            row = {
                "noise": label, "read-out": name, "n pairs": int(ok.sum()),
                "no-result rate %": 100 * (1 - np.isfinite(sc).mean()),
                "tier agreement %": 100 * agree.mean(),
                "agreement CI low": 100 * np.percentile(boots, 2.5), "agreement CI high": 100 * np.percentile(boots, 97.5),
                "weighted kappa": cohen_kappa_score(t1, t2, weights="linear"),
                "within-person SD of risk (pp)": 100 * np.sqrt(np.mean((r1 - r2) ** 2) / 2),
                "high-tier changes %": 100 * np.mean((t1 == 2) != (t2 == 2)),
                "AUROC": roc_auc_score(y[first], sc[first, 0]),
            }
            if name.startswith("device"):
                b = border[ok]
                changed = ~agree
                row["borderline rate %"] = 100 * b.mean()
                row["tier changes flagged %"] = 100 * (b[:, 0] | b[:, 1])[changed].mean() if changed.any() else np.nan
                row["agreement when not flagged %"] = 100 * agree[~(b[:, 0] | b[:, 1])].mean()
                per_record[label] = (r1, r2, risk(single[ok, 0]), risk(single[ok, 1]))
            summary.append(row)
            print(f"{label:15s} {name:22s} agreement {row['tier agreement %']:.1f} % "
                  f"(kappa {row['weighted kappa']:.3f}), SD {row['within-person SD of risk (pp)']:.2f} pp, "
                  f"no result {row['no-result rate %']:.1f} %, AUROC {row['AUROC']:.3f}"
                  + (f", borderline {row['borderline rate %']:.1f} %, changes flagged {row['tier changes flagged %']:.0f} %"
                     if 'borderline rate %' in row else ""))

    tab = pd.DataFrame(summary)
    tab.to_csv(C.TAB_DIR / "table23_repeatability.csv", index=False, float_format="%.4f")

    # ---------------------------------------------------------------- figure
    import matplotlib.pyplot as plt
    P.set_style()
    fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.6), gridspec_kw={"width_ratios": [1.25, 1]})
    ax = axes[0]
    labels = [s for s in tab["noise"].unique()]
    xs = np.arange(len(labels))
    for k, (name, col) in enumerate((("single window", "#eb6834"), ("device (median of 3)", "#2a78d6"))):
        d = tab[tab["read-out"] == name].set_index("noise").loc[labels]
        err = np.vstack([d["tier agreement %"] - d["agreement CI low"], d["agreement CI high"] - d["tier agreement %"]])
        ax.bar(xs + (k - 0.5) * 0.38, d["tier agreement %"], 0.36, yerr=err, color=col, label=name, capsize=2,
               error_kw={"lw": 0.8})
    ax.set_xticks(xs, labels)
    ax.set_ylim(min(80, tab["tier agreement %"].min() - 3), 104)
    ax.set_yticks(np.arange(80, 101, 5))
    ax.set_ylabel("Same tier in both measurements (%)")
    ax.set_xlabel("Body noise (in-band SNR)")
    ax.legend(frameon=False, fontsize=7, loc="upper right")
    ax.set_title("a  Test–retest tier agreement", loc="left")

    ax = axes[1]
    key = "10 dB"
    r1, r2, s1, s2 = per_record[key]
    ax.scatter(100 * s1, 100 * s2, s=3, color="#eb6834", alpha=0.35, lw=0, label="single window")
    ax.scatter(100 * r1, 100 * r2, s=3, color="#2a78d6", alpha=0.5, lw=0, label="device (median of 3)")
    lim = max(np.nanmax(100 * s1), np.nanmax(100 * r1)) * 1.05
    ax.plot([0, lim], [0, lim], color="0.5", lw=0.7)
    ax.set_xlim(0, lim)
    ax.set_ylim(0, lim)
    ax.set_xlabel("Risk, measurement 1 (%)")
    ax.set_ylabel("Risk, measurement 2 (%)")
    ax.legend(frameon=False, fontsize=7, loc="upper left", markerscale=3)
    ax.set_title(f"b  Repeated risk at {key}", loc="left")
    fig.tight_layout()
    P.savefig(fig, "fig16_repeatability", data=tab)
    print("wrote", C.TAB_DIR / "table23_repeatability.csv")


if __name__ == "__main__":
    main()
