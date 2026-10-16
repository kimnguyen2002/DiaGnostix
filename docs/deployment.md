# Deployment on the ESP32-S3 N16R8: decision and workflow

## 1. Train on the PC, infer on the device

The dataset is far too large to train on a micro-controller: 16,766 ECGs, about 168 MB of raw lead-I samples cached as `int16`. Training is not even the question, though. What matters is whether the trained model, and the computation needed to feed it, fit on the ESP32-S3 N16R8. They fit easily:

| Part | Size on the device | Where | Time |
|---|---|---|---|
| Three risk models (plain C node tables / coefficients) | a few KB to a few tens of KB of flash (Section 14 of the notebook) | flash (3 MB app partition) | microseconds per prediction |
| ECG feature extractor work buffers | 304 KB (`EcgFeatureExtractor::kWorkBytes`) | octal PSRAM | printed at boot ("ms per 10-s window") |
| ECG capture (up to six 10-s windows of int16 mV) | 59 KB | octal PSRAM | — |
| Dashboard (gzip) | ≈ 23 KB | flash | — |

**Decision: features and model run entirely on the ESP32-S3, as dependency-free C.**
- **No TensorFlow Lite Micro is needed.** The models are gradient-boosted trees, logistic regression or a small MLP on 25–40 numbers. As plain C they compile without any library, need under 1 KB of RAM, and give results identical to Python. The parity tests in Section 15 compare them with Python: maximum difference ≈ 10⁻¹⁶ for trees and logistic regression, ≈ 10⁻⁷ for the float32 MLP.
- **TFLite Micro would add about 100 KB of runtime code and a tensor arena, and gain nothing** at this model size. It becomes worthwhile only for a future raw-waveform CNN.
- **The cloud is not needed for a result.** A result is available offline right after the recording (the boot log prints the measured analysis time per window), and no personal data leaves the device. The cloud tier is reserved for optional longitudinal analysis and the clinician view (Section 4).

## 2. Why `.h5`, and how C/C++ in the Arduino IDE reads the model

The notebook saves the Keras MLP of the model-family comparison as **`ml/outputs/models/dgx_mlp.h5`** (and `.keras`). An Arduino sketch cannot open an HDF5 file; there is no HDF5 parser for the ESP32 in the Arduino IDE. The notebook therefore converts the trained models into C source:

```
Keras MLP  ──► dgx_mlp.h5 / .keras ──► TFLite float32 / int8 ──► dgx_mlp_int8_tflite.h (TFLite Micro byte array)
     │
     └────────────────────────────────────────────────────────► dgx_model_mlp.h (plain C weights, no library)

LightGBM / logistic regression ──► dgx_model_primary.h, dgx_model_fallback.h, dgx_model_ecg_only.h (plain C)
```

The deployed models are whichever family the pre-specified rule selected. They are included through `dgx_models.h`:
- `DGX_PRI` (primary);
- `DGX_FB` (fallback);
- `DGX_ECG` (ECG-only).

Each header contains:
- the input names, in the order the model expects;
- one median per input, used to fill missing values;
- the model itself;
- the Platt calibration;
- the two tier thresholds.

The firmware (`risk_model.h`) looks inputs up **by name**. A re-trained model with a different input set therefore needs no code change.

## 3. Updating the models on the device

1. Re-run `ml/notebooks/01_ecg_prediabetes_training_and_export.ipynb`. Section 15 writes:
   - the C headers to `ml/outputs/deploy/`, and copies them into `firmware/DiaGnostixECG/`;
   - the golden vectors to `firmware/test/golden/`.
2. Run the host tests: `make -C firmware/test`. They check:
   - that the C++ feature extractor reproduces the Python features on the golden recordings;
   - that the three model headers reproduce the Python scores;
   - the session logic: median of windows, lead-off, low signal quality, inverted leads, model choice and gain calibration.
3. Open `firmware/DiaGnostixECG/DiaGnostixECG.ino` in the Arduino IDE and upload with the N16R8 settings ([hardware.md](hardware.md)).
4. Watch the Serial Monitor. The **boot self-test** runs one recorded ECG window (`dgx_selftest_vector.h`) through the extractor and the ECG-only model, and compares the result with Python. If it fails, measurements are disabled.

## 4. Cloud and Flutter app (next phase)

The device already exposes everything an app or a cloud service needs:
- **Live data and results.** These are available over the WebSocket now, and over BLE later (`ENABLE_BLE 1`); see [protocol.md](protocol.md).
- **Reproducibility.** `ml/outputs/models/feature_spec.json` holds, for each model:
  - its inputs, medians, thresholds, tiers and calibration;
  - the advice text shown with each tier.

  A phone or server can therefore recompute or audit any result from its `inputs`.
- **Phone or server inference** if wanted: `dgx_mlp_fp32.tflite` with `dgx_mlp_preprocessing.json` (`tflite_flutter`), or the joblib/booster files.
- **Hospital referral.** A `high` result tells the user to visit a clinic or hospital for a fasting glucose or HbA1c test. The Flutter app can add:
  - a reminder to book that test;
  - a shareable PDF report of the result and its `windows` and `ecg` details;
  - opt-in upload to a cloud service for longitudinal trends.

  Identifying data should stay on the phone. The device accepts the profile with `"persist": false` (RAM only).
