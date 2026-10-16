# DiaGnostix-ECG: key results (auto-generated 2026-10-17)

## Data and design
- Internal cohort: 16,766 ECGs from 16,339 adults; 1,447 (8.6 %) prediabetes/diabetes.
- Participant-disjoint split: development 15,090 (1,300 events) / internal test 1,676 (147 events); 10-fold grouped CV inside development.
- External cohort (other hospital and ECG vendor): 2,456 ECGs, 366 events (14.9 %); no age, sex, BMI or BP.
- Device simulation: AD8232 filters → ESP32-S3 ADC (integer mV, gain 1100, 3 mV noise) at 500 Hz; 1.06 % of internal records reach the ADC limit.

## Benchmark
- Published DiaCardia lead-I ensemble, internal test AUROC 0.844 (95 % CI 0.811–0.877); external 0.746 (95 % CI 0.721–0.772).

## Inputs and model (pre-specified rules, development data only)
- Input selection: ECG → ECG + profile: +0.059 → add; ECG + profile → ECG + profile + BP: -0.000 → do not add; ECG + profile → ECG + profile + BP + lifestyle: -0.000 → do not add.
- Primary input set: **ECG + profile** (35 inputs). Model family: **Logistic regression** (36 coefficients, 1.1 KB).
- Same rule per deployed model: fallback **Logistic regression** (26 coefficients); ECG-only **LightGBM (compact)** (230 trees, 920 splits).
- Development OOF AUROC: primary 0.845; fallback 0.844; ECG-only 0.784; profile only (no ECG) 0.828.
- Riley minimum sample size (p = 35): 2,389 records needed vs 15,090 available.

## Internal test (primary model)
- AUROC 0.878 (95 % CI 0.853–0.903); vs published DiaCardia lead I ΔAUROC +0.034 (DeLong P = 0.0042).
- High tier (Youden): sensitivity 82.3 (76.2–88.5) %, specificity 76.3 (74.2–78.5) %, PPV 25.1 %, NPV 97.8 %.
- Elevated or high (90 %-sensitivity): sensitivity 91.8 (87.2–96.0) %, specificity 62.5 (60.0–65.0) %, NPV 98.8 %.
- Calibration after Platt: intercept -0.04, slope 1.16, Brier 0.0622.
- Reclassification: Primary vs Profile: IDI +0.053 (+0.035 to +0.072), cfNRI +0.278 (+0.120 to +0.442); Primary vs ECG-only: IDI +0.067 (+0.041 to +0.096), cfNRI +0.533 (+0.377 to +0.706); ECG-only vs Profile: IDI -0.014 (-0.043 to +0.012), cfNRI -0.352 (-0.535 to -0.184).
- Flagged high (internal): normoglycaemia 23.7 (21.6–25.9) %; prediabetes range 72.5 (61.0–81.6) %; untreated diabetes range 87.5 (71.9–95.0) %.
- Prediabetes range vs normoglycaemia AUROC: 0.837 (0.795–0.879).

## External validation (ECG-only model)
- AUROC 0.757 (95 % CI 0.732–0.782); vs published DiaCardia lead I ΔAUROC +0.010 (P = 0.0591).
- High tier: sensitivity 73.8 %, specificity 65.2 %.
- Calibration-in-the-large intercept +0.55 (mean predicted 0.098 vs observed 0.149).
- Prediabetes range vs normoglycaemia AUROC: 0.727 (0.693–0.761); flagged high: prediabetes range 69.0 (62.3–75.0) %.

## Hardware robustness
- Acquisition chain (primary, retrained test AUROC): clinical_500 0.877; ad8232_500 0.878; esp32s3_500 0.878.
- ADC noise 20 mV at the pin: test AUROC 0.877.
- Body noise at 10 dB in-band SNR: test AUROC 0.875, SQI pass 98.7 %.
- Gain error 1.2×: primary sensitivity/specificity 82.3/75.2 %; fallback 83.0/74.7 % (calibrated gain: 82.3/76.4 %).

## Deployment on the ESP32-S3 N16R8
- Three plain-C models, 23.7 KB of flash in total; C vs Python max |Δ score| 3.3e-16; risk tiers identical in 100.00 % of test records.
- Keras MLP exported as .h5 → TFLite: float32 max |Δ| 1.2e-07; int8 max |Δ| 0.019.
- Published DiaCardia lead-I ensemble as a C node table: ≈ 4.4 MB, and its features cannot be computed on a micro-controller.
