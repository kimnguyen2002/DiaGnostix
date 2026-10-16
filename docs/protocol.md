# DiaGnostix-ECG device protocol

The device exposes the same JSON messages over two transports, plus one read-only HTTP endpoint:

| Transport | Used by | Endpoint |
|---|---|---|
| WebSocket | web dashboard (now) | `ws://<device>/ws` |
| Bluetooth LE GATT | Flutter app (later; `ENABLE_BLE 1`) | service `6e1d0001-8b3c-4a8e-9c4b-5f2d1a7c3e90` |
| HTTP (read-only) | scripts, quick checks, cloud gateway | `GET http://<device>/api/state` |

`<device>` can be any of:
- `diagnostix.local` (mDNS);
- the address printed on the serial console;
- `192.168.4.1` when the device runs its own access point.

**Conventions**
- Every message is one UTF-8 JSON object.
- Messages from the device carry a `"type"` field; commands to the device carry a `"cmd"` field.
- Values that are not available are `null`.
- `t_ms` is milliseconds since the device booted.

### Bluetooth LE

Bluetooth is enabled with `ENABLE_BLE 1` in `firmware/DiaGnostixECG/config.h` and needs the NimBLE-Arduino library.

| Characteristic | UUID | Properties | Content |
|---|---|---|---|
| Telemetry | `6e1d0002-8b3c-4a8e-9c4b-5f2d1a7c3e90` | notify | `vitals` messages (1 Hz) |
| Events | `6e1d0003-8b3c-4a8e-9c4b-5f2d1a7c3e90` | notify, read | `session`, `result`, `alert`, `profile`, `calibration`, `ecg_gain`, `error` |
| Command | `6e1d0004-8b3c-4a8e-9c4b-5f2d1a7c3e90` | write | commands (same JSON as over WebSocket) |
| Info | `6e1d0005-8b3c-4a8e-9c4b-5f2d1a7c3e90` | read | device, firmware, model version, BP calibration state |

Notifications longer than the negotiated MTU are split into frames. Each frame starts with one header byte:
- bit 7 is set on the last frame of a message;
- bits 0–6 hold the frame index; index 0 starts a message.

The client concatenates the payloads, without the header bytes, until it receives the last frame.

---

## Device → client messages

### `hello`
Sent once after a client connects and in reply to the `hello` command. `/api/state` returns the same object.

```json
{
  "type": "hello",
  "device": "DiaGnostix-ECG",
  "firmware": "2.0.0",
  "stream": { "ecg_fs": 250, "ecg_unit": "mV at the ADC", "ppg_fs": 50 },
  "model": {
    "version": "2.0.0",
    "family": "LightGBM (compact)",
    "primary_inputs": "ECG + profile",
    "self_test": true,
    "window_s": 10,
    "min_good_windows": 2,
    "threshold": 0.0912,
    "needs": ["age", "male_sex", "BMI"],
    "models": {
      "primary":  { "n_inputs": 35, "non_ecg_inputs": ["age", "male_sex", "BMI"], "uses_amplitudes": true,  "high": 0.0912, "elevated": 0.0508 },
      "fallback": { "n_inputs": 25, "non_ecg_inputs": ["age", "male_sex", "BMI"], "uses_amplitudes": false, "high": 0.0904, "elevated": 0.0493 },
      "ecg_only": { "n_inputs": 32, "non_ecg_inputs": [], "uses_amplitudes": true, "high": 0.0917, "elevated": 0.0484 }
    }
  },
  "profile": { "...": "profile object, or null" },
  "calibration": { "calibrated": false, "sbp": 120, "dbp": 80, "pat_ms": 250 },
  "ecg_gain": { "gain": 1100.0, "calibrated": false, "method": "nominal" },
  "session": { "...": "session object" },
  "alert": { "...": "alert object" },
  "last_result": { "...": "last valid result object, or null" }
}
```
- `model.threshold` and the per-model `high` and `elevated` values are **calibrated probabilities** at the tier boundaries.
- `needs` lists every non-ECG input that any of the three models uses. The dashboard shows the lifestyle questions only if they appear here.
- The numbers above are an example. The real values come from the generated headers.

### Profile object
```json
{ "age": 52, "gender": "male", "height_cm": 171.0, "weight_kg": 78.0, "bmi": 26.7, "smoking": null, "drinking": null }
```
- `gender` is `"female"` or `"male"`; it maps to the model input `male_sex`.
- `smoking` is `true`, `false` or `null` (not answered).
- `drinking` is `1` (rarely), `2` (occasionally), `3` (regularly) or `null`.
- Sent as `{"type": "profile", ...}` after every change.

### `ecg_gain`
```json
{ "type": "ecg_gain", "gain": 1043.2, "calibrated": true, "method": "sine" }
```
`method` is one of:
- `nominal`: 1100, not calibrated;
- `sine`: measured with a reference sine;
- `entered`: typed in by the user.

### `session`
```json
{ "type": "session", "state": "acquiring", "mode": "measure", "progress": 0.42, "elapsed_s": 13.9,
  "duration_s": 33, "beats": 14, "pats": 0, "message": "Keep still, breathe normally and do not talk" }
```
- `state` is `idle`, `acquiring`, `complete` or `failed`.
- `mode` is one of:
  - `measure`: risk measurement;
  - `calibrate`: blood-pressure calibration;
  - `gain`: ECG gain calibration.
- A measurement lasts 3 s of warm-up plus N × 10 s windows: N = 3 by default, 2–6 allowed.
- A completed measurement can still turn to `failed` after the analysis when fewer than two windows pass the signal-quality index. The `message` then explains why.

### `result`
Sent when the analysis of a measurement has finished.
```json
{
  "type": "result", "ok": true, "id": 17, "t_ms": 812345,
  "model": "primary", "model_version": "2.0.0",
  "tier": "high", "positive": true,
  "risk": 0.2410, "score": 0.6120,
  "threshold": 0.0912, "threshold_elevated": 0.0508,
  "advice": "Raised risk of prediabetes or diabetes. Please visit a clinic or hospital for a fasting plasma glucose or HbA1c blood test.",
  "windows_ok": 3, "windows_total": 3,
  "windows": [ { "status": "ok", "score": 0.6120, "beats": 12, "hr": 71.3, "beat_corr": 0.982, "sqi": true }, "..." ],
  "ecg": { "hr": 71.3, "rr_sd_ms": 31.0, "rmssd_ms": 28.4, "pr_ms": 152, "qrs_ms": 84, "qt_ms": 326, "qtc_ms": 355,
           "r_uv": 640, "t_r_ratio": 0.271 },
  "inputs": { "mean_rr_s": 0.842, "...": "every model input (ECG medians over the good windows + profile)" },
  "measurements": { "hr": 71.0, "sbp": null, "dbp": null, "pat_ms": null, "spo2": 97.0, "body_temp": 34.1,
                    "room_temp": 24.5, "sdnn": 31.2, "rmssd": 28.0, "beats": 34, "pats": 0, "bp_calibrated": false },
  "gain": { "gain": 1043.2, "calibrated": true, "method": "sine" },
  "compute_ms": 412,
  "warnings": []
}
```

| Field | Meaning |
|---|---|
| `risk` | Platt-calibrated probability of prediabetes or diabetes, from the median model score of the good windows |
| `tier` | `high`: score ≥ Youden threshold; `elevated`: score ≥ 90 %-sensitivity threshold; `low`: below both; `no_result`: fewer than two windows passed the signal-quality index |
| `model` | `primary`, `fallback`, `ecg_only`, or `none` when no model can run |
| `windows[].status` | `ok`, `leads_off` (a lead came off during the window), `low_quality` (failed the SQI) or `error` |
| `ok` | `false` when `tier` is `no_result`; `note` then gives the reason |

`hello.last_result` always holds the last result with a tier.

**Which model is used**

| Profile saved | Gain calibrated | Model |
|---|---|---|
| yes | yes | `primary` |
| yes | no | `fallback` (no ECG amplitudes) |
| no | yes | `ecg_only` |
| no | no | none: the `start` command is rejected with the reason |

A model that uses blood pressure is used only with a cuff-calibrated BP estimate. The session then requires the finger sensor.

### `alert`
```json
{ "type": "alert", "active": true, "kind": "risk",
  "message": "Raised risk of prediabetes or diabetes (24 %). Please visit a clinic or hospital for a fasting glucose or HbA1c blood test." }
```
`kind` is `risk` (high tier), `spo2`, `fever` or `none`. An alert stays until it is acknowledged (`ack` or the BOOT button).

### `vitals` (once per second)
```json
{ "type": "vitals", "t_ms": 812000, "leads_on": true, "finger_on": true, "hr": 71.2, "ppg_hr": 70.8, "spo2": 97.0,
  "pi": 2.10, "pat_ms": 245.3, "sbp": 121, "dbp": 78, "bp_calibrated": false, "sdnn": 32.1, "rmssd": 29.4,
  "body_temp": 34.2, "room_temp": 24.6, "humidity": 55.0, "pressure": 1012.3 }
```

### `beat`, `pat`
```json
{ "type": "beat", "t_ms": 812345, "rr": 842.0 }
{ "type": "pat", "t_ms": 812345, "pat_ms": 245.3, "sbp": 121.0, "dbp": 78.0 }
```
One message per detected heartbeat and per pulse arrival time, for research logging.

### `wave` (10 per second)
```json
{ "type": "wave", "ecg": [1650, 1652, 1661], "ppg": [60123, 60145] }
```
- `ecg`: millivolts at the ADC pin, decimated to 250 Hz. It is for display only; the model analyses the full 500 Hz capture on the device.
- `ppg`: raw MAX30102 infrared counts at 50 Hz.

### `calibration`, `error`
```json
{ "type": "calibration", "calibrated": true, "sbp": 118, "dbp": 76, "pat_ms": 241.0 }
{ "type": "error", "message": "A session is already running" }
```

---

## Client → device commands

| Command | Fields | Effect |
|---|---|---|
| `hello` | — | device replies with `hello` |
| `start` | `duration_s` (optional, 23–63; default 33) | starts a risk measurement: 3 s warm-up + ⌊(duration − 3) / 10⌋ windows |
| `cancel` | — | stops the running session |
| `ack` | — | acknowledges the active alert |
| `profile` | `age` (18–100), `gender`, `height_cm` (120–220), `weight_kg` (25–250), optional `smoking`, `drinking`, `persist` | stores the profile; `"persist": false` keeps it in RAM only |
| `calibrate` | `sbp`, `dbp` (cuff reading) | 20-s blood-pressure calibration |
| `clear_calibration` | — | back to the population BP default |
| `calibrate_gain` | `ref_mvpp` (0.2–2.5, default 1.0) | 6-s AD8232 gain measurement from a reference sine between RA and LA |
| `set_gain` | `gain` (500–2000) | stores a gain measured another way (e.g. with an oscilloscope) |
| `clear_gain` | — | back to the nominal gain 1100 (not calibrated: the fallback model is used) |

Errors come back as `{"type": "error", "message": "..."}`.
