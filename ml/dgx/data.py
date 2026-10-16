"""Loading of every table in the Zenodo record and a cached loader for raw lead-I waveforms."""
import os
import numpy as np
import pandas as pd
from joblib import Parallel, delayed

from . import config as C


def load_participants():
    df = pd.read_csv(C.PARTICIPANTS_CSV, dtype={"participant_id": str, "ecg_id": str})
    df["male_sex"] = df["male_sex"].astype(int)
    df["smoking"] = df["smoking"].astype(int)
    for col in ["dm_under_treatment", "prediabetes_diabetes", "development_data"]:
        df[col] = df[col].astype(bool)
    return df.set_index("ecg_id")


def load_external():
    df = pd.read_csv(C.EXTERNAL_CSV, dtype={"ecg_id": str})
    df["prediabetes_diabetes"] = df["prediabetes_diabetes"].astype(bool)
    df["dm_under_treatment"] = df["dm_under_treatment"].astype(bool)
    return df.set_index("ecg_id")


def load_feature_table(path):
    return pd.read_csv(path, dtype={"ecg_id": str}).set_index("ecg_id")


def feature_list_1lead():
    return pd.read_csv(C.FEATURE_LIST_1LEAD)["feature"].tolist()


def add_glycaemic_strata(df):
    """Split the composite endpoint into its clinical components (used for secondary analyses)."""
    fpg, a1c, tx = df["FPG"], df["HbA1c"], df["dm_under_treatment"].astype(bool)
    diabetes = (fpg >= 126) | (a1c >= 6.5) | tx
    df = df.copy()
    df["diabetes_range"] = diabetes
    df["prediabetes_only"] = df[C.LABEL] & ~diabetes
    # ADA 2025 definition (FPG 100-125 mg/dL or HbA1c 5.7-6.4 %), used as a sensitivity analysis
    df["label_ada"] = (fpg >= 100) | (a1c >= 5.7) | tx
    return df


# ---------------------------------------------------------------- raw waveforms
def _read_lead_I(path, n_samples):
    # files have a 12-column header (cp932 lead names) and a trailing comma on internal-cohort rows;
    # column 0 is lead I (verified with Einthoven's law I + III = II on every file checked)
    x = pd.read_csv(path, skiprows=1, header=None, usecols=[0], dtype=np.int32,
                    encoding="latin1").values[:, 0]
    if len(x) < n_samples:
        raise ValueError(f"{path} shorter than {n_samples} samples")
    return x[:n_samples].astype(np.int16)


def load_raw_lead_I(ecg_ids, raw_dir, cache_name, n_jobs=-1):
    """Return int16 array (n_records, 5000) of the first 10 s of lead I, cached as .npy."""
    n = int(C.RECORD_SECONDS * C.FS_CLINICAL)
    cache = C.CACHE_DIR / f"{cache_name}.npy"
    ids_cache = C.CACHE_DIR / f"{cache_name}_ids.txt"
    ecg_ids = list(ecg_ids)
    if cache.exists() and ids_cache.exists():
        cached_ids = ids_cache.read_text().split()
        if cached_ids == ecg_ids:
            return np.load(cache)
    paths = [os.path.join(raw_dir, f"{i}.csv") for i in ecg_ids]
    arrs = Parallel(n_jobs=n_jobs, batch_size=64)(delayed(_read_lead_I)(p, n) for p in paths)
    X = np.stack(arrs)
    C.CACHE_DIR.mkdir(parents=True, exist_ok=True)
    np.save(cache, X)
    ids_cache.write_text("\n".join(ecg_ids))
    return X
