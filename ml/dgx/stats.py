"""Evaluation statistics: DeLong AUROC inference, cluster bootstrap, thresholds, calibration, DCA, NRI/IDI."""
import numpy as np
import pandas as pd
from scipy import stats as st
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import average_precision_score, brier_score_loss, roc_auc_score, roc_curve


# ------------------------------------------------------------------ DeLong (Sun & Xu 2014, fast O(n log n))
def _delong_components(y, preds):
    y = np.asarray(y).astype(bool)
    preds = np.atleast_2d(preds)
    pos, neg = preds[:, y], preds[:, ~y]
    m, n = pos.shape[1], neg.shape[1]
    tx = np.apply_along_axis(st.rankdata, 1, pos)
    ty = np.apply_along_axis(st.rankdata, 1, neg)
    tz = np.apply_along_axis(st.rankdata, 1, np.hstack([pos, neg]))
    aucs = tz[:, :m].sum(axis=1) / m / n - (m + 1.0) / 2.0 / n
    v01 = (tz[:, :m] - tx) / n
    v10 = 1.0 - (tz[:, m:] - ty) / m
    cov = np.atleast_2d(np.cov(v01)) / m + np.atleast_2d(np.cov(v10)) / n
    return aucs, cov


def auc_delong_ci(y, p, alpha=0.95):
    auc, cov = _delong_components(y, p)
    se = np.sqrt(cov[0, 0])
    z = st.norm.ppf(1 - (1 - alpha) / 2)
    return float(auc[0]), float(max(0, auc[0] - z * se)), float(min(1, auc[0] + z * se))


def delong_test(y, p1, p2):
    """Two-sided DeLong test for two correlated ROC curves on the same records."""
    aucs, cov = _delong_components(y, np.vstack([p1, p2]))
    var = cov[0, 0] + cov[1, 1] - 2 * cov[0, 1]
    z = (aucs[0] - aucs[1]) / np.sqrt(var) if var > 0 else 0.0
    return float(aucs[0] - aucs[1]), float(2 * st.norm.sf(abs(z)))


# ------------------------------------------------------------------ thresholds and confusion metrics
def youden_threshold(y, p):
    fpr, tpr, thr = roc_curve(y, p)
    return float(thr[np.argmax(tpr - fpr)])


def threshold_for_sensitivity(y, p, target=0.90):
    fpr, tpr, thr = roc_curve(y, p)
    return float(thr[np.argmax(tpr >= target)])


def threshold_metrics(y, p, thr):
    y = np.asarray(y).astype(bool)
    yhat = np.asarray(p) >= thr
    tp, fn = np.sum(yhat & y), np.sum(~yhat & y)
    tn, fp = np.sum(~yhat & ~y), np.sum(yhat & ~y)
    div = lambda a, b: a / b if b > 0 else np.nan
    sens, spec = div(tp, tp + fn), div(tn, tn + fp)
    ppv, npv = div(tp, tp + fp), div(tn, tn + fn)
    mcc_den = np.sqrt(float(tp + fp) * (tp + fn) * (tn + fp) * (tn + fn))
    return {
        "sensitivity": sens, "specificity": spec, "PPV": ppv, "NPV": npv,
        "PLR": div(sens, 1 - spec), "NLR": div(1 - sens, spec),
        "F1": div(2 * tp, 2 * tp + fp + fn), "G-mean": np.sqrt(sens * spec),
        "MCC": div(tp * tn - fp * fn, mcc_den), "accuracy": (tp + tn) / len(y),
        "TP": int(tp), "FP": int(fp), "TN": int(tn), "FN": int(fn),
    }


# ------------------------------------------------------------------ bootstrap (resampling participants)
def _cluster_index(groups):
    groups = np.asarray(groups)
    uniq, inv = np.unique(groups, return_inverse=True)
    members = [[] for _ in range(len(uniq))]
    for i, g in enumerate(inv):
        members[g].append(i)
    return [np.asarray(m) for m in members]


def bootstrap(fn, y, *preds, groups=None, n_boot=2000, seed=42):
    """Percentile bootstrap of fn(y, *preds) -> dict, resampling clusters (participants) with replacement."""
    rng = np.random.default_rng(seed)
    y = np.asarray(y)
    clusters = _cluster_index(groups if groups is not None else np.arange(len(y)))
    out = []
    for _ in range(n_boot):
        pick = rng.integers(0, len(clusters), len(clusters))
        idx = np.concatenate([clusters[k] for k in pick])
        if y[idx].min() == y[idx].max():
            continue
        out.append(fn(y[idx], *[np.asarray(p)[idx] for p in preds]))
    return pd.DataFrame(out)


def summarize(y, p_score, thr, p_prob=None, groups=None, n_boot=2000, seed=42):
    """Point estimates + 95 % CIs: AUROC (DeLong), everything else by cluster bootstrap."""
    p_prob = p_score if p_prob is None else p_prob

    def fn(yy, ss, pp):
        d = threshold_metrics(yy, ss, thr)
        d.update(AUPRC=average_precision_score(yy, ss), Brier=brier_score_loss(yy, pp))
        return {k: v for k, v in d.items() if k not in ("TP", "FP", "TN", "FN")}

    point = fn(np.asarray(y), np.asarray(p_score), np.asarray(p_prob))
    boot = bootstrap(fn, y, p_score, p_prob, groups=groups, n_boot=n_boot, seed=seed)
    auc, lo, hi = auc_delong_ci(y, p_score)
    rows = {"AUROC": (auc, lo, hi)}
    for k, v in point.items():
        rows[k] = (v, boot[k].quantile(0.025), boot[k].quantile(0.975))
    df = pd.DataFrame(rows, index=["estimate", "ci_low", "ci_high"]).T
    df.attrs["counts"] = {k: v for k, v in threshold_metrics(y, p_score, thr).items() if k in ("TP", "FP", "TN", "FN")}
    return df


def fmt_ci(row, pct=False, digits=3):
    f = (lambda v: f"{100 * v:.1f}") if pct else (lambda v: f"{v:.{digits}f}")
    return f"{f(row['estimate'])} ({f(row['ci_low'])}–{f(row['ci_high'])})"


# ------------------------------------------------------------------ calibration
def _logit(p, eps=1e-6):
    p = np.clip(np.asarray(p, dtype=float), eps, 1 - eps)
    return np.log(p / (1 - p))


class PlattCalibrator:
    """Logistic recalibration on the logit of the model score (2 numbers -> trivially portable to C)."""

    def fit(self, score, y):
        lr = LogisticRegression(C=1e6, max_iter=1000).fit(_logit(score)[:, None], y)
        self.a_, self.b_ = float(lr.coef_[0, 0]), float(lr.intercept_[0])
        return self

    def transform(self, score):
        return 1 / (1 + np.exp(-(self.a_ * _logit(score) + self.b_)))


def calibration_metrics(y, p, n_bins=10):
    y = np.asarray(y).astype(float)
    lp = _logit(p)
    slope_fit = LogisticRegression(C=1e6, max_iter=1000).fit(lp[:, None], y)
    # calibration-in-the-large: intercept of logit(y) = a + 1*logit(p) (offset model), solved by Newton
    a = 0.0
    for _ in range(50):
        mu = 1 / (1 + np.exp(-(a + lp)))
        a += np.sum(y - mu) / np.sum(mu * (1 - mu))
    bins = pd.qcut(p, n_bins, labels=False, duplicates="drop")
    df = pd.DataFrame({"y": y, "p": p, "bin": bins}).groupby("bin").agg(p_mean=("p", "mean"), y_mean=("y", "mean"), n=("y", "size"))
    ece = float(np.sum(df.n * np.abs(df.p_mean - df.y_mean)) / len(y))
    return {"calibration_intercept": float(a), "calibration_slope": float(slope_fit.coef_[0, 0]),
            "Brier": float(brier_score_loss(y, p)), "ECE": ece,
            "mean_predicted": float(np.mean(p)), "observed_rate": float(np.mean(y))}


def calibration_curve_ci(y, p, n_bins=10):
    bins = pd.qcut(p, n_bins, labels=False, duplicates="drop")
    df = pd.DataFrame({"y": np.asarray(y).astype(float), "p": p, "bin": bins})
    g = df.groupby("bin").agg(p_mean=("p", "mean"), k=("y", "sum"), n=("y", "size")).reset_index()
    z = 1.96
    phat = g.k / g.n
    den = 1 + z ** 2 / g.n
    centre = (phat + z ** 2 / (2 * g.n)) / den
    half = z * np.sqrt(phat * (1 - phat) / g.n + z ** 2 / (4 * g.n ** 2)) / den
    g["obs"], g["lo"], g["hi"] = phat, centre - half, centre + half
    return g


# ------------------------------------------------------------------ decision curve analysis
def decision_curve(y, p, thresholds):
    y = np.asarray(y).astype(bool)
    n, prev = len(y), y.mean()
    rows = []
    for t in thresholds:
        yhat = p >= t
        tp, fp = np.sum(yhat & y), np.sum(yhat & ~y)
        w = t / (1 - t)
        rows.append({"threshold": t, "net_benefit": tp / n - fp / n * w, "treat_all": prev - (1 - prev) * w})
    return pd.DataFrame(rows)


# ------------------------------------------------------------------ reclassification
def nri_idi(y, p_old, p_new):
    y = np.asarray(y).astype(bool)
    up, down = p_new > p_old, p_new < p_old
    nri_e = up[y].mean() - down[y].mean()
    nri_ne = down[~y].mean() - up[~y].mean()
    idi = (p_new[y].mean() - p_old[y].mean()) - (p_new[~y].mean() - p_old[~y].mean())
    return {"cfNRI": nri_e + nri_ne, "NRI_events": nri_e, "NRI_nonevents": nri_ne, "IDI": idi}


def ppv_npv_at_prevalence(sens, spec, prevalence):
    prevalence = np.asarray(prevalence)
    ppv = sens * prevalence / (sens * prevalence + (1 - spec) * (1 - prevalence))
    npv = spec * (1 - prevalence) / (spec * (1 - prevalence) + (1 - sens) * prevalence)
    return ppv, npv


def univariate_auc(X, y):
    out = {}
    for c in X.columns:
        v = X[c].fillna(X[c].median())
        out[c] = roc_auc_score(y, v)
    return pd.Series(out)
