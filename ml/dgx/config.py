"""Project-wide paths, constants and plotting palette for the DiaGnostix-ECG risk models.

Constants marked HARDWARE describe the device wiring (docs/wiring-diagram.svg): AD8232 OUTPUT -> ESP32-S3 ADC1
(GPIO4), sampled at 500 Hz.
"""
import os
from pathlib import Path

# ---------------------------------------------------------------- paths
ML_DIR = Path(__file__).resolve().parents[1]                # .../DiaGnostix/ml
REPO_DIR = ML_DIR.parent                                    # .../DiaGnostix
# Koga et al. 2025 dataset (Zenodo 16416031, CC BY 4.0); not part of the repository, see README "Data"
DATA_DIR = Path(os.environ.get("DGX_DATA_DIR", REPO_DIR / "data" / "koga2025"))

PARTICIPANTS_CSV = DATA_DIR / "participant_characteristics.csv"
EXTERNAL_CSV = DATA_DIR / "external_cohort_characteristics.csv"
FEAT_1LEAD_CSV = DATA_DIR / "ecg_feature_data" / "ecg_features_1-lead.csv"
FEAT_12LEAD_CSV = DATA_DIR / "ecg_feature_data" / "ecg_features_12-leads.csv"
FEAT_EXT_CSV = DATA_DIR / "ecg_feature_data" / "ecg_features_12-leads_external_cohort.csv"
FEATURE_LIST_1LEAD = DATA_DIR / "feature_list" / "feature_list_28_1-lead.csv"
FEATURE_LIST_12LEAD = DATA_DIR / "feature_list" / "feature_list_269_12-lead.csv"
RAW_INTERNAL_DIR = DATA_DIR / "raw_ecg_data"
RAW_EXTERNAL_DIR = DATA_DIR / "external_ecg_data"
PUBLISHED_1LEAD_MODEL_DIR = DATA_DIR / "model_1-lead"
PUBLISHED_12LEAD_MODEL_DIR = DATA_DIR / "model_12-lead"

CACHE_DIR = ML_DIR / "cache"
OUT_DIR = ML_DIR / "outputs"
FIG_DIR = OUT_DIR / "figures"
TAB_DIR = OUT_DIR / "tables"
MODEL_DIR = OUT_DIR / "models"
DEPLOY_DIR = OUT_DIR / "deploy"
SKETCH_DIR = REPO_DIR / "firmware" / "DiaGnostixECG"
FIRMWARE_TEST_DIR = REPO_DIR / "firmware" / "test"

# ---------------------------------------------------------------- study constants
SEED = 42
LABEL = "prediabetes_diabetes"          # FPG >= 110 mg/dL or HbA1c >= 6.0 % or diabetes treatment
FS_CLINICAL = 500                       # Hz, both cohorts (Fukuda Denshi VaSera / Nihon Kohden Cardiofax)
RECORD_SECONDS = 10.0                   # every record is truncated to the first 10 s (device protocol)
# Amplitude calibration of the raw CSV integers. Not stated in the dataset; 5 uV/LSB reproduces the lead-I
# R-wave amplitudes plotted in mV in Koga et al. 2025, Fig. 2c (median ~0.41 mV normoglycaemia, ~0.62 mV
# prediabetes/diabetes). Verified again in Section 2 of the notebook.
UV_PER_LSB = 5.0
# The external raw CSVs (Nihon Kohden) are stored at a 4x finer resolution (see Section 2).
UV_PER_LSB_EXTERNAL = UV_PER_LSB / 4

# ---------------------------------------------------------------- HARDWARE: ESP32-S3 N16R8 + AD8232 (this device)
FS_DEVICE = 500                 # Hz, ECG task period 2 ms (firmware/DiaGnostixECG/config.h ECG_SAMPLE_RATE_HZ)
AD8232_GAIN = 1100.0            # SparkFun-type AD8232 board: in-amp 100 x op-amp 11 (calibrated per board)
ADC_REF_MV = 1650.0             # AD8232 REF / output mid-point at 3.3 V supply
ADC_FULL_SCALE_MV = 3100.0      # ESP32-S3 ADC1 usable range with 11 dB attenuation
ADC_LSB_MV = 1.0                # analogReadMilliVolts() returns integer millivolts
ADC_NOISE_MV = 3.0              # conservative ADC + supply noise (1 SD) at the ADC pin

# ---------------------------------------------------------------- non-ECG inputs available on the device
CLINICAL = ["age", "male_sex", "BMI"]            # profile on the dashboard / app
BLOOD_PRESSURE = ["sBP", "dBP"]                  # cuffless estimate from pulse arrival time (AD8232 + MAX30102)
LIFESTYLE = ["smoking", "drinking"]              # self-report (would need two extra profile questions)

# ---------------------------------------------------------------- plotting (validated reference palette)
INK = {"primary": "#0b0b0b", "secondary": "#52514e", "muted": "#898781",
       "grid": "#e1e0d9", "axis": "#c3c2b7", "surface": "#fcfcfb"}
SERIES = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#4a3aa7", "#e34948"]
MODEL_COLORS = {
    "LightGBM": SERIES[0],
    "MLP": SERIES[1],
    "Logistic regression": SERIES[2],
    "XGBoost": SERIES[3],
    "Random forest": SERIES[4],
    "Device model": SERIES[5],
    "DiaCardia (published)": SERIES[6],
    "Clinical-only": SERIES[7],
}
CLASS_COLORS = {"Normoglycaemia": SERIES[0], "Prediabetes/diabetes": SERIES[1]}
