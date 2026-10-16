"""Publication figure style (thin marks, recessive grid, fixed colour per entity) and shared panels."""
import matplotlib as mpl
import matplotlib.pyplot as plt
import numpy as np
import pandas as pd
from sklearn.metrics import average_precision_score, precision_recall_curve, roc_curve

from . import config as C
from .stats import auc_delong_ci


def set_style():
    mpl.rcParams.update({
        "figure.dpi": 110, "savefig.dpi": 300, "figure.facecolor": "white", "axes.facecolor": "white",
        "font.family": "sans-serif", "font.sans-serif": ["Helvetica", "Arial", "DejaVu Sans"],
        "font.size": 9, "axes.titlesize": 10, "axes.titleweight": "bold", "axes.labelsize": 9,
        "xtick.labelsize": 8, "ytick.labelsize": 8, "legend.fontsize": 8, "legend.frameon": False,
        "text.color": C.INK["primary"], "axes.labelcolor": C.INK["primary"],
        "xtick.color": C.INK["secondary"], "ytick.color": C.INK["secondary"],
        "axes.edgecolor": C.INK["axis"], "axes.linewidth": 0.8,
        "axes.spines.top": False, "axes.spines.right": False,
        "axes.grid": True, "grid.color": C.INK["grid"], "grid.linewidth": 0.6, "grid.linestyle": "-",
        "axes.axisbelow": True, "lines.linewidth": 1.6, "lines.markersize": 5,
        "axes.prop_cycle": mpl.cycler(color=C.SERIES), "pdf.fonttype": 42, "ps.fonttype": 42,
    })


def savefig(fig, name, data=None):
    """PNG (300 dpi) + vector PDF; optional CSV 'table view' of the plotted data."""
    C.FIG_DIR.mkdir(parents=True, exist_ok=True)
    fig.savefig(C.FIG_DIR / f"{name}.png", bbox_inches="tight")
    fig.savefig(C.FIG_DIR / f"{name}.pdf", bbox_inches="tight")
    if data is not None:
        (data if isinstance(data, pd.DataFrame) else pd.DataFrame(data)).to_csv(C.FIG_DIR / f"{name}_data.csv", index=False)


def chance_line(ax):
    ax.plot([0, 1], [0, 1], color=C.INK["muted"], lw=0.8, ls=(0, (2, 2)), zorder=0)


def roc_panel(ax, curves, title=None):
    """curves: list of (label, y, score, colour). Legend shows AUROC with DeLong 95 % CI."""
    rows = []
    for c in curves:
        label, y, p, color = c[:4]
        ls = c[4] if len(c) > 4 else "-"
        fpr, tpr, _ = roc_curve(y, p)
        auc, lo, hi = auc_delong_ci(y, p)
        ax.plot(fpr, tpr, color=color, ls=ls, label=f"{label}  {auc:.3f} ({lo:.3f}–{hi:.3f})")
        rows.append(pd.DataFrame({"model": label, "fpr": fpr, "tpr": tpr}))
    chance_line(ax)
    ax.set(xlim=(0, 1), ylim=(0, 1.01), xlabel="1 − specificity", ylabel="Sensitivity", aspect="equal")
    ax.legend(loc="lower right", title="AUROC (95 % CI)", title_fontsize=8)
    if title:
        ax.set_title(title, loc="left")
    return pd.concat(rows)


def pr_panel(ax, curves, title=None):
    rows = []
    for c in curves:
        label, y, p, color = c[:4]
        ls = c[4] if len(c) > 4 else "-"
        prec, rec, _ = precision_recall_curve(y, p)
        ap = average_precision_score(y, p)
        ax.plot(rec, prec, color=color, ls=ls, label=f"{label}  AP {ap:.3f}")
        rows.append(pd.DataFrame({"model": label, "recall": rec, "precision": prec}))
    prev = np.mean(curves[0][1])
    ax.axhline(prev, color=C.INK["muted"], lw=0.8, ls=(0, (2, 2)), zorder=0)
    ax.text(0.02, prev + 0.01, f"prevalence {prev:.3f}", ha="left", va="bottom", color=C.INK["secondary"], fontsize=7)
    ax.set(xlim=(0, 1), ylim=(0, 1.01), xlabel="Recall (sensitivity)", ylabel="Precision (PPV)")
    ax.legend(loc="upper right")
    if title:
        ax.set_title(title, loc="left")
    return pd.concat(rows)


def confusion_panel(ax, counts, title):
    m = np.array([[counts["TN"], counts["FP"]], [counts["FN"], counts["TP"]]])
    cmap = mpl.colors.LinearSegmentedColormap.from_list("seq", ["#f4f8fd", "#86b6ef", "#1c5cab"])
    ax.imshow(m / m.sum(axis=1, keepdims=True), cmap=cmap, vmin=0, vmax=1)
    for i in range(2):
        for j in range(2):
            frac = m[i, j] / m[i].sum()
            ax.text(j, i, f"{m[i, j]}\n({100 * frac:.1f} %)", ha="center", va="center", fontsize=9,
                    color="white" if frac > 0.6 else C.INK["primary"])
    ax.set_xticks([0, 1], ["Predicted\nnegative", "Predicted\npositive"])
    ax.set_yticks([0, 1], ["Normoglycaemia", "Prediabetes/\ndiabetes"])
    ax.grid(False)
    ax.set_title(title, loc="left")
