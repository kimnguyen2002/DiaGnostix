"""Model zoo, participant-grouped cross-validation and Optuna tuning."""
import json
import os

import numpy as np
import optuna
from lightgbm import LGBMClassifier
from sklearn.base import BaseEstimator, ClassifierMixin
from sklearn.ensemble import RandomForestClassifier
from sklearn.impute import SimpleImputer
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import roc_auc_score
from sklearn.model_selection import StratifiedGroupKFold
from sklearn.pipeline import Pipeline
from sklearn.preprocessing import StandardScaler
from xgboost import XGBClassifier

from .config import SEED

N_THREADS = max(1, (os.cpu_count() or 2))


def cv_splits(y, groups, n_splits=10, seed=SEED):
    """Stratified by outcome, grouped by participant (no person in both training and validation)."""
    skf = StratifiedGroupKFold(n_splits=n_splits, shuffle=True, random_state=seed)
    return list(skf.split(np.zeros(len(y)), y, groups))


def _pos_weight(y, mode):
    ratio = (len(y) - np.sum(y)) / max(1, np.sum(y))
    return {"none": 1.0, "sqrt": float(np.sqrt(ratio)), "full": float(ratio)}[mode]


# ------------------------------------------------------------------ Keras MLP (TFLite-exportable)
def _AucEarlyStopping(X_val, y_val, patience):
    """Early stopping on validation AUROC computed with scikit-learn (keeps the best weights)."""
    import tensorflow as tf

    class _Cb(tf.keras.callbacks.Callback):
        def on_train_begin(self, logs=None):
            self.best, self.best_epoch, self.best_weights, self.wait, self.history = -np.inf, 0, None, 0, []

        def on_epoch_end(self, epoch, logs=None):
            p = self.model(X_val, training=False).numpy().ravel()
            auc = roc_auc_score(y_val, p)
            self.history.append(auc)
            if auc > self.best:
                self.best, self.best_epoch, self.wait = auc, epoch + 1, 0
                self.best_weights = self.model.get_weights()
            else:
                self.wait += 1
                if self.wait >= patience:
                    self.model.stop_training = True

        def on_train_end(self, logs=None):
            if self.best_weights is not None:
                self.model.set_weights(self.best_weights)

    return _Cb()


class KerasMLP(BaseEstimator, ClassifierMixin):
    """Median-impute -> standardise -> clip to +/-5 SD -> Dense(ReLU) stack -> sigmoid. Early stopping on an
    inner participant-grouped 10 % split of the training data (the outer validation fold is never seen).
    Clipping bounds the input range so full-int8 TFLite quantisation keeps a fine resolution."""

    CLIP = 5.0

    def __init__(self, hidden=(32, 16), dropout=0.2, l2=1e-4, lr=1e-3, batch_size=256,
                 max_epochs=300, patience=25, balance="full", seed=SEED):
        self.hidden, self.dropout, self.l2, self.lr = hidden, dropout, l2, lr
        self.batch_size, self.max_epochs, self.patience = batch_size, max_epochs, patience
        self.balance, self.seed = balance, seed

    def _build(self, n_in):
        import tensorflow as tf
        reg = tf.keras.regularizers.l2(self.l2)
        inp = tf.keras.Input(shape=(n_in,), name="features")
        h = inp
        for k, units in enumerate(self.hidden):
            h = tf.keras.layers.Dense(units, activation="relu", kernel_regularizer=reg, name=f"dense_{k}")(h)
            if self.dropout > 0:
                h = tf.keras.layers.Dropout(self.dropout, name=f"dropout_{k}")(h)
        out = tf.keras.layers.Dense(1, activation="sigmoid", name="risk")(h)
        model = tf.keras.Model(inp, out, name="dgx_mlp")
        # legacy Adam: the v2.11+ optimizer is slow on Apple silicon; AUROC is tracked by _AucEarlyStopping
        model.compile(optimizer=tf.keras.optimizers.legacy.Adam(self.lr), loss="binary_crossentropy")
        return model

    def fit(self, X, y, groups=None):
        import tensorflow as tf
        X, y = np.asarray(X, dtype=float), np.asarray(y).astype(int)
        self.imputer_ = SimpleImputer(strategy="median").fit(X)
        self.scaler_ = StandardScaler().fit(self.imputer_.transform(X))
        Xs = self.transform_inputs(X)
        g = np.arange(len(y)) if groups is None else np.asarray(groups)
        tr, va = cv_splits(y, g, n_splits=10, seed=self.seed)[0]
        tf.keras.backend.clear_session()      # hundreds of models are built in one process
        tf.keras.utils.set_random_seed(self.seed)
        self.model_ = self._build(X.shape[1])
        w1 = _pos_weight(y[tr], self.balance)
        cb = _AucEarlyStopping(Xs[va], y[va], patience=self.patience)
        self.model_.fit(Xs[tr], y[tr], epochs=self.max_epochs, batch_size=self.batch_size,
                        class_weight={0: 1.0, 1: w1}, callbacks=[cb], verbose=0)
        self.history_ = {"val_auc": cb.history}
        self.best_epoch_ = cb.best_epoch
        self.classes_ = np.array([0, 1])
        return self

    def transform_inputs(self, X):
        z = self.scaler_.transform(self.imputer_.transform(np.asarray(X, dtype=float)))
        return np.clip(z, -self.CLIP, self.CLIP).astype(np.float32)

    def predict_proba(self, X):
        p = self.model_.predict(self.transform_inputs(X), batch_size=4096, verbose=0).ravel()
        return np.c_[1 - p, p]


# ------------------------------------------------------------------ estimator factory
def make_estimator(kind, params, y_train=None):
    params = dict(params)
    if kind == "lr":
        return Pipeline([("impute", SimpleImputer(strategy="median")), ("scale", StandardScaler()),
                         ("clf", LogisticRegression(C=params["C"], penalty=params["penalty"], solver="liblinear",
                                                    class_weight="balanced", max_iter=5000))])
    if kind == "rf":
        return Pipeline([("impute", SimpleImputer(strategy="median")),
                         ("clf", RandomForestClassifier(**params, class_weight="balanced_subsample",
                                                        n_jobs=N_THREADS, random_state=SEED))])
    if kind == "xgb":
        bal = params.pop("balance")
        return Pipeline([("impute", SimpleImputer(strategy="median")),
                         ("clf", XGBClassifier(**params, scale_pos_weight=_pos_weight(y_train, bal),
                                               tree_method="hist", n_jobs=N_THREADS, random_state=SEED,
                                               eval_metric="logloss"))])
    if kind == "lgbm":
        bal = params.pop("balance")
        return Pipeline([("impute", SimpleImputer(strategy="median")),
                         ("clf", LGBMClassifier(**params, subsample_freq=1, scale_pos_weight=_pos_weight(y_train, bal),
                                                n_jobs=N_THREADS, random_state=SEED, verbose=-1))])
    if kind == "mlp":
        return KerasMLP(**params)
    raise ValueError(kind)


def suggest(kind, trial, space="full"):
    t = trial
    if kind == "lr":
        return {"C": t.suggest_float("C", 1e-4, 1e2, log=True), "penalty": t.suggest_categorical("penalty", ["l1", "l2"])}
    if kind == "rf":
        return {"n_estimators": t.suggest_int("n_estimators", 100, 600, step=50),
                "max_depth": t.suggest_int("max_depth", 3, 16),
                "min_samples_leaf": t.suggest_int("min_samples_leaf", 1, 60, log=True),
                "max_features": t.suggest_float("max_features", 0.1, 0.8)}
    if kind == "xgb":
        return {"n_estimators": t.suggest_int("n_estimators", 50, 800, step=25),
                "learning_rate": t.suggest_float("learning_rate", 5e-3, 0.2, log=True),
                "max_depth": t.suggest_int("max_depth", 2, 8),
                "min_child_weight": t.suggest_float("min_child_weight", 1, 50, log=True),
                "subsample": t.suggest_float("subsample", 0.5, 1.0),
                "colsample_bytree": t.suggest_float("colsample_bytree", 0.3, 1.0),
                "reg_lambda": t.suggest_float("reg_lambda", 1e-3, 100, log=True),
                "reg_alpha": t.suggest_float("reg_alpha", 1e-3, 10, log=True),
                "gamma": t.suggest_float("gamma", 0, 5),
                "balance": t.suggest_categorical("balance", ["none", "sqrt", "full"])}
    if kind == "lgbm":
        if space == "compact":   # micro-controller budget: <= 300 trees, <= 16 leaves, depth <= 4
            size = {"n_estimators": t.suggest_int("n_estimators", 20, 300, step=10),
                    "num_leaves": t.suggest_int("num_leaves", 2, 16),
                    "max_depth": t.suggest_int("max_depth", 1, 4),
                    "learning_rate": t.suggest_float("learning_rate", 0.01, 0.3, log=True)}
        else:
            size = {"n_estimators": t.suggest_int("n_estimators", 50, 1000, step=25),
                    "num_leaves": t.suggest_int("num_leaves", 4, 128, log=True),
                    "max_depth": t.suggest_int("max_depth", 2, 10),
                    "learning_rate": t.suggest_float("learning_rate", 5e-3, 0.2, log=True)}
        return {**size,
                "min_child_samples": t.suggest_int("min_child_samples", 5, 300, log=True),
                "colsample_bytree": t.suggest_float("colsample_bytree", 0.3, 1.0),
                "subsample": t.suggest_float("subsample", 0.5, 1.0),
                "reg_lambda": t.suggest_float("reg_lambda", 1e-3, 100, log=True),
                "reg_alpha": t.suggest_float("reg_alpha", 1e-3, 10, log=True),
                "min_split_gain": t.suggest_float("min_split_gain", 0.0, 1.0),
                "balance": t.suggest_categorical("balance", ["none", "sqrt", "full"])}
    raise ValueError(kind)


MLP_GRID = [
    {"hidden": (16,), "dropout": 0.1, "l2": 1e-4},
    {"hidden": (32, 16), "dropout": 0.2, "l2": 1e-4},
    {"hidden": (64, 32), "dropout": 0.3, "l2": 1e-4},
    {"hidden": (32, 16, 8), "dropout": 0.2, "l2": 1e-3},
]


def tune(kind, X, y, groups, n_trials=50, space="full", seed=SEED, n_folds=5, cache_path=None):
    """Optuna TPE search maximising mean participant-grouped CV AUROC on the development data only."""
    if cache_path is not None and os.path.exists(cache_path):
        with open(cache_path) as fh:
            return json.load(fh)
    X, y = np.asarray(X, dtype=float), np.asarray(y).astype(int)
    splits = cv_splits(y, groups, n_splits=n_folds, seed=seed)
    trials_log = []

    def score(params, trial=None):
        aucs = []
        for k, (tr, va) in enumerate(splits):
            m = make_estimator(kind, params, y[tr])
            fit_kw = {"groups": np.asarray(groups)[tr]} if kind == "mlp" else {}
            m.fit(X[tr], y[tr], **fit_kw)
            aucs.append(roc_auc_score(y[va], m.predict_proba(X[va])[:, 1]))
            if trial is not None:
                trial.report(float(np.mean(aucs)), k)
                if trial.should_prune():
                    raise optuna.TrialPruned()
        return float(np.mean(aucs)), float(np.std(aucs))

    if kind == "mlp":   # small fixed grid (each fit is expensive)
        for params in MLP_GRID:
            mean, sd = score(params)
            trials_log.append({"params": {**params, "hidden": list(params["hidden"])}, "cv_auc": mean, "cv_sd": sd})
        best = max(trials_log, key=lambda r: r["cv_auc"])
        result = {"kind": kind, "space": space, "best_params": best["params"], "best_cv_auc": best["cv_auc"],
                  "n_trials": len(MLP_GRID), "trials": trials_log}
    else:
        optuna.logging.set_verbosity(optuna.logging.WARNING)
        study = optuna.create_study(direction="maximize", sampler=optuna.samplers.TPESampler(seed=seed),
                                    pruner=optuna.pruners.MedianPruner(n_startup_trials=10, n_warmup_steps=1))
        study.optimize(lambda tr: score(suggest(kind, tr, space), tr)[0], n_trials=n_trials)
        trials_log = [{"number": t.number, "value": t.value, "state": str(t.state)} for t in study.trials]
        result = {"kind": kind, "space": space, "best_params": study.best_params, "best_cv_auc": study.best_value,
                  "n_trials": n_trials, "trials": trials_log}
    if cache_path is not None:
        with open(cache_path, "w") as fh:
            json.dump(result, fh, indent=1)
    return result


def params_for(kind, best_params):
    p = dict(best_params)
    if kind == "mlp":
        p["hidden"] = tuple(p["hidden"])
    return p


def oof_predictions(kind, params, X, y, groups, splits):
    """Out-of-fold scores on the development set (used for thresholds, calibration, CV metrics)."""
    X, y = np.asarray(X, dtype=float), np.asarray(y).astype(int)
    oof, fold_auc = np.zeros(len(y)), []
    for tr, va in splits:
        m = make_estimator(kind, params_for(kind, params), y[tr])
        fit_kw = {"groups": np.asarray(groups)[tr]} if kind == "mlp" else {}
        m.fit(X[tr], y[tr], **fit_kw)
        oof[va] = m.predict_proba(X[va])[:, 1]
        fold_auc.append(roc_auc_score(y[va], oof[va]))
    return oof, np.asarray(fold_auc)


def fit_final(kind, params, X, y, groups):
    m = make_estimator(kind, params_for(kind, params), np.asarray(y).astype(int))
    fit_kw = {"groups": np.asarray(groups)} if kind == "mlp" else {}
    return m.fit(np.asarray(X, dtype=float), np.asarray(y).astype(int), **fit_kw)


# ------------------------------------------------------------------ model size on a micro-controller
def tree_model_size(booster_dump_trees):
    """(#trees, #internal nodes, #leaves) from a LightGBM dump_model()['tree_info'] list."""
    internal = leaves = 0

    def walk(n):
        nonlocal internal, leaves
        if "split_index" in n:
            internal += 1
            walk(n["left_child"])
            walk(n["right_child"])
        else:
            leaves += 1

    for t in booster_dump_trees:
        walk(t["tree_structure"])
    return len(booster_dump_trees), internal, leaves


def lgbm_flash_bytes(n_internal, n_leaves, n_trees):
    # SoA layout used by export.py: double threshold + int16 left/right + uint8 feature; double leaf; int32 root
    return n_internal * (8 + 2 + 2 + 1) + n_leaves * 8 + n_trees * 4


def sklearn_forest_size(estimators):
    nodes = sum(e.tree_.node_count for e in estimators)
    leaves = sum(int((e.tree_.children_left == -1).sum()) for e in estimators)
    return len(estimators), nodes - leaves, leaves
