# DiaGnostix

**NTU SmartMHI** · Prediabetes and diabetes risk screening from a single-lead ECG and a short profile, computed on an ESP32-S3 microcontroller.

The device:
1. records three 10-second lead-I ECG windows with an AD8232 front end;
2. computes 32 ECG features and a signal-quality index on the ESP32-S3;
3. combines them with the user's age, sex and BMI;
4. runs a small risk model in plain C.

When the risk is high, the LED flashes and the dashboard asks the user to visit a clinic or hospital for a fasting glucose or HbA1c test.

> **Research prototype, not a medical device.** DiaGnostix does not diagnose diabetes. The models were validated on clinical ECGs passed through a simulation of the device's signal chain; validation with recordings from the device itself is still required.

| | |
|---|---|
| Training data | 16,766 health check-up ECGs (1,447 with prediabetes or diabetes), Koga *et al.* 2025; external cohort of 2,456 ECGs from another hospital and ECG vendor |
| Primary model (ECG + age, sex, BMI) | logistic regression, 36 coefficients, 1.1 KB |
| Internal test AUROC (n = 1,676) | **0.878** (95 % CI 0.853–0.903) |
| High-risk tier, internal test | sensitivity 82.3 %, specificity 76.3 %, NPV 97.8 % |
| External AUROC, ECG-only model (n = 2,456) | **0.757** (0.732–0.782) |
| Firmware, ESP32-S3 N16R8 | 1.15 MB flash (36 % of the app partition), 69 KB static RAM, ≈ 360 KB PSRAM |

## Contents

- [How it works](#how-it-works)
- [Hardware](#hardware)
- [Repository structure](#repository-structure)
- [Getting started](#getting-started)
- [Using the device](#using-the-device)
- [Results](#results)
- [Limitations](#limitations)
- [Documentation](#documentation)
- [Data and license](#data-and-license)

## How it works

```
 lead-I ECG (RA right wrist, LA left wrist, RL right leg)
        │  AD8232 (0.5–40 Hz, gain ≈ 1100)
        ▼
 ESP32-S3 ADC1 GPIO4, 500 Hz, analogReadMilliVolts()  ──►  3 s warm-up + 3 × 10-s windows (PSRAM)
        │
        ▼  per window (firmware/DiaGnostixECG/ecg_features.h)
 µV = (mV − 1650) × 1000 / gain → median removal → 0.5–40 Hz zero-phase band-pass
 → R peaks (gradient/boxcar + T-wave rejection) → P/Q/S/T → 32 features + signal-quality index
        │
        ▼  risk_model.h: model chosen by what is available
 primary (ECG + profile, gain calibrated) │ fallback (no ECG amplitudes) │ ECG-only (no profile)
        │
        ▼  median score of the windows that pass the quality check (≥ 2) → calibrated risk → tier
 high     → LED flashes, alert: "visit a clinic or hospital for a fasting glucose or HbA1c test"
 elevated → LED blinks: "ask for a test at your next check-up"
 low      → LED on for 2 s
```

**The other sensors are not model inputs**, because the training data contain no such measurements. They provide monitoring and safety checks:
- the MAX30102 gives SpO₂ and a cuffless blood-pressure estimate;
- the NTC gives skin temperature, with a fever warning;
- the BME280 records room conditions.

**Training and deployment.** The models are trained in Python on device-matched features: the AD8232 filters and the ESP32-S3 ADC are simulated before feature extraction. They are then exported as dependency-free C headers, and the firmware needs no machine-learning library. The C/C++ implementation reproduces the Python reference to ≈ 10⁻¹¹ for the features and ≈ 10⁻¹⁶ for the model scores. A boot self-test checks this on the device.

## Hardware

| Part | Connection (ESP32-S3-DevKitC-1 N16R8) |
|---|---|
| AD8232 ECG module | OUTPUT → GPIO4 (ADC1), LO+ → GPIO5, LO− → GPIO6, 3V3, GND |
| MAX30102 pulse oximeter | I²C SDA GPIO8, SCL GPIO9 (0x57) |
| BME280 | I²C SDA GPIO8, SCL GPIO9 (0x76/0x77) |
| 10 kΩ NTC thermistor | divider node → GPIO1 |
| Alert LED + 220 Ω | GPIO15 |

![Wiring diagram](docs/wiring-diagram.svg)

The bill of materials, electrode placement, AD8232 gain calibration and electrical-safety notes are in [docs/hardware.md](docs/hardware.md).

## Repository structure

```
DiaGnostix/
├── firmware/
│   ├── DiaGnostixECG/           Arduino sketch for the ESP32-S3 N16R8
│   │   ├── DiaGnostixECG.ino    application: sessions, model choice, alerts, commands
│   │   ├── ecg_features.h       on-device ECG feature extractor (PSRAM buffers)
│   │   ├── risk_model.h         the three risk models behind one interface
│   │   ├── ecg_risk.h           windows → features → scores → median → risk tier
│   │   ├── dgx_model_*.h        generated risk models (primary, fallback, ECG-only)
│   │   ├── dgx_dsp_spec.h, dgx_selftest_vector.h   generated filter constants and boot self-test
│   │   └── …                    sensor front ends, session, JSON protocol, Wi-Fi/BLE, LED
│   ├── test/                    host tests (signal processing; extractor and models vs Python)
│   └── web/index.html           dashboard source
├── ml/
│   ├── dgx/                     Python package: device simulation, features, models, statistics, export
│   ├── notebooks/               training, validation and export notebook (executed)
│   ├── outputs/                 figures, tables, trained models, exported C headers, results summary
│   └── environment.yml
├── tools/
│   ├── embed_dashboard.py       compresses the dashboard into the sketch
│   └── mock_device.py           device simulator for dashboard and app development
├── docs/                        hardware, wiring, device protocol, deployment
└── Dataset/                     DiaBD diabetes dataset (CSV)
```

## Getting started

### 1. Upload the firmware (Arduino IDE)

1. Install the **esp32** board package 3.x (Espressif Systems), then these libraries:
   - ESP Async WebServer and Async TCP (ESP32Async);
   - ArduinoJson 7;
   - SparkFun MAX3010x Pulse and Proximity Sensor Library;
   - Adafruit BME280 Library.
2. Copy `firmware/DiaGnostixECG/secrets.example.h` to `secrets.h` and enter your Wi-Fi network. Without it, the device opens the access point `DiaGnostix` (password `diagnostix`).
3. Open `firmware/DiaGnostixECG/DiaGnostixECG.ino` and choose these settings:

   | Setting | Value |
   |---|---|
   | Board | ESP32S3 Dev Module |
   | Flash Size | 16MB (128Mb) |
   | Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
   | PSRAM | OPI PSRAM |

   Then upload.
4. In the Serial Monitor (115200 baud), check for these lines:
   - `flash 16 MB, PSRAM 8 MB`;
   - `[model] self-test passed`.

### 2. Run the host tests

```bash
make -C firmware/test
```

- **Signal-processing tests:** the QRS detector, PPG, pulse arrival time, blood pressure and thermistor.
- **Pipeline tests:**
  - the ECG feature extractor and the three model headers against the Python golden vectors;
  - the session logic: median of windows, lead-off, low signal quality, inverted leads, model choice and gain calibration.

### 3. Develop without hardware

```bash
conda env create -f ml/environment.yml && conda activate diagnostix
python tools/mock_device.py            # http://localhost:8080 ; options: --risk high|low, --board-gain 1043, --no-finger
```

The simulator serves the dashboard and speaks the same WebSocket protocol as the device ([docs/protocol.md](docs/protocol.md)). It replays a real lead-I ECG as ADC millivolts and scores each measurement with the same models and model-choice rule as the firmware.

After editing `firmware/web/index.html`, run `python tools/embed_dashboard.py`.

### 4. Retrain the models

1. Download the Koga *et al.* dataset from [Zenodo 16416031](https://zenodo.org/records/16416031) and unpack it into `data/koga2025/`, or point `DGX_DATA_DIR` to the unpacked folder.
2. Open `ml/notebooks/01_ecg_prediabetes_training_and_export.ipynb` with the `diagnostix` kernel and run all cells. A full run takes about 1 h on an Apple M1 Pro.

The notebook covers:
- data checks and the device simulation;
- pre-specified input and model selection;
- internal and external validation;
- hardware robustness.

At the end it writes the C headers into `firmware/DiaGnostixECG/` and the golden vectors into `firmware/test/golden/`.

## Using the device

1. Open `http://diagnostix.local` (or the IP shown on the serial console). Save the profile: age, sex, height, weight.
2. **Calibrate the AD8232 gain once**, under *ECG gain calibration* on the dashboard, with a 1 mV sine reference ([docs/hardware.md](docs/hardware.md#ad8232-gain-calibration)). Until then the device uses a gain-invariant fallback model.
3. Attach the electrodes: red on the right wrist, yellow on the left wrist, green on the right leg. Sit still and press **Start measurement** (33 s) or the BOOT button.
4. Read the result:
   - the risk tier and calibrated risk;
   - the advice;
   - the ECG analysis: windows used, heart rate, HRV, PR/QRS/QTc, R amplitude.

   A **high** tier keeps the LED flashing until acknowledged and advises a fasting glucose or HbA1c test.

## Results

The full tables and figures are in [`ml/outputs/`](ml/outputs/); the key numbers are in [`ml/outputs/RESULTS_SUMMARY.md`](ml/outputs/RESULTS_SUMMARY.md).

**Input selection** (development data, out-of-fold AUROC; pre-specified rule):

| Inputs | AUROC | Decision |
|---|---|---|
| ECG (32 features) | 0.784 | start |
| + age, sex, BMI | 0.843 | added |
| + blood pressure (10 mmHg device error) | 0.843 | not added |
| + smoking, drinking | 0.843 | not added |

**Model families.** Logistic regression (0.845), compact LightGBM (0.843) and an MLP (0.842) tie, so the smallest model is deployed. Larger models do not help: XGBoost 0.842, random forest 0.835.

**Internal test** (primary model):

| | High tier | Elevated or high |
|---|---|---|
| Sensitivity | 82.3 % (76.2–88.5) | 91.8 % (87.2–96.0) |
| Specificity | 76.3 % (74.2–78.5) | 62.5 % (60.0–65.0) |
| PPV / NPV | 25.1 % / 97.8 % | 19.0 % / 98.8 % |

**Added value of the ECG.** Adding the ECG to age, sex and BMI improves:
- AUROC: +0.026 (P < 0.001);
- reclassification: IDI +0.053, category-free NRI +0.278.

**Prediabetes.** 72.5 % of people in the prediabetes range are flagged high. The AUROC for prediabetes range vs normoglycaemia is 0.837.

**External validation** (ECG-only model): AUROC 0.757. The model under-estimates risk at the higher external prevalence, so a new site needs local recalibration.

**Hardware robustness** (simulated):

| Factor | Effect |
|---|---|
| ADC noise up to 20 mV | none (AUROC ≥ 0.877) |
| EMG/motion noise down to 5 dB SNR | AUROC ≥ 0.875; the quality check refuses noisier recordings |
| 2× AD8232 gain error | primary specificity 76.4 % → 72.5 %; the gain-invariant fallback is unaffected (AUROC 0.876) |

## Limitations

- **Simulated device signals.** The AD8232 front end, ADC, noise and gain errors were simulated on clinical ECGs. A paired study with the physical device and same-day FPG/HbA1c is needed before any clinical use.
- **Partial external validation.** The external cohort has no age, sex or BMI, so only the ECG-only model was validated externally.
- **Population and endpoint.**
  - The training data are Japanese check-up cohorts, with a composite endpoint: FPG ≥ 110 mg/dL, HbA1c ≥ 6.0 % or treatment.
  - Calibration must be updated locally for other populations or ADA criteria.
- **Current status only.** The model detects current dysglycaemia; it does not predict future diabetes.

## Documentation

| Document | Content |
|---|---|
| [docs/hardware.md](docs/hardware.md) | bill of materials, wiring, lead-I electrode placement, AD8232 gain calibration, electrical safety |
| [docs/protocol.md](docs/protocol.md) | WebSocket and Bluetooth LE messages and commands |
| [docs/deployment.md](docs/deployment.md) | on-device inference, model export path (Keras `.h5` → TFLite / C), updating the models |
| [ml/outputs/RESULTS_SUMMARY.md](ml/outputs/RESULTS_SUMMARY.md) | key validation results, regenerated by the notebook |

## Data and license

- **Code, firmware and documentation:** Apache License 2.0 (see [LICENSE](LICENSE)).
- **ECG data:** Koga *et al.*, *Artificial Intelligence Identifies Individuals with Prediabetes from Single-Lead Electrocardiograms*, Zenodo record 16416031, CC BY 4.0. The data are not stored in this repository, except for 22 short lead-I excerpts in `firmware/test/golden/` that are used by the tests.
- **DiaBD dataset** (`Dataset/`): Prama *et al.*, *Data in Brief* 2025, CC BY 4.0.

If you use this work, please cite the ECG dataset and study:

```bibtex
@article{koga2025diacardia,
  title   = {Artificial intelligence identifies individuals with prediabetes using single-lead electrocardiograms},
  author  = {Koga, Daisuke and Kaneda, Ryo and Komiya, Chikara and Ohno, Satoshi and Takeuchi, Akira and
             Hara, Kazunari and Horino, Masato and Aoki, Jun and Okazaki, Rei and Ishii, Ryoko and
             Murakami, Masanori and Tsujimoto, Kazutaka and Ikeda, Kenji and Katagiri, Hideki and
             Shimizu, Hideyuki and Yamada, Tetsuya},
  journal = {Cardiovascular Diabetology},
  volume  = {24},
  pages   = {415},
  year    = {2025},
  doi     = {10.1186/s12933-025-02982-4}
}
```
