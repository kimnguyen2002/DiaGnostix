#!/usr/bin/env python3
"""Figure and table of the vital-sign smoother (firmware/DiaGnostixECG/smoothing.h).

Runs the host benchmark firmware/test/test_smoothing (600-s, 1-Hz streams with Gaussian noise, sparse outlier
spikes and a step change at t = 300 s; metrics averaged over 50 seeds) and writes
ml/outputs/tables/table24_smoothing.csv and ml/outputs/figures/fig17_smoothing.

    make -C firmware/test test_smoothing && python tools/smoothing_figure.py
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "ml"))
from dgx import config as C  # noqa: E402
from dgx import plotting as P  # noqa: E402


def main():
    exe = ROOT / "firmware" / "test" / "test_smoothing"
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "smoothing.csv"
        subprocess.run([str(exe), "--csv", str(out)], check=True, capture_output=True)
        streams = pd.read_csv(out)
        m = pd.read_csv(f"{out}.metrics.csv")
    m["jitter reduction %"] = 100 * (1 - m.jitter_out / m.jitter_raw)
    m["error reduction %"] = 100 * (1 - m.err_out / m.err_raw)
    m.to_csv(C.TAB_DIR / "table24_smoothing.csv", index=False, float_format="%.4f")
    print(m[["vital", "jitter_raw", "jitter_out", "jitter reduction %", "err_raw", "err_out", "worst_raw", "worst_out",
             "step90_s"]].round(2).to_string(index=False))

    P.set_style()
    fig, axes = plt.subplots(2, 1, figsize=(3.5, 2.75), sharex=True)
    for ax, (vital, tag) in zip(axes, (("Heart rate (ECG)", "a"), ("SpO2", "b"))):
        s = streams[streams.vital == vital]
        unit = s.unit.iloc[0]
        ax.plot(s.t_s, s.raw, ".", ms=1.6, color="0.62", label="sensor reading")
        ax.plot(s.t_s, s["true"], "--", lw=0.8, color="0.2", label="true value")
        ax.plot(s.t_s, s.shown, lw=1.3, color="#2a78d6", label="displayed")
        lo, hi = s["true"].min(), s["true"].max()
        pad = 0.9 * (hi - lo) + 4 * s.raw.sub(s["true"]).abs().median()
        ax.set_ylim(lo - pad, hi + pad)
        ax.set_ylabel(f"{vital.replace('SpO2', 'SpO$_2$')} ({unit})")
        ax.set_title(f"{tag}  {vital.replace('SpO2', 'SpO$_2$')}", loc="left")
    axes[0].legend(frameon=False, fontsize=6.5, loc="upper left", markerscale=4, ncol=3)
    axes[1].set_xlabel("Time (s)")
    fig.tight_layout()
    P.savefig(fig, "fig17_smoothing", data=m)


if __name__ == "__main__":
    main()
