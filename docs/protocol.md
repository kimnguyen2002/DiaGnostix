# DiaGnostix-ECG device protocol (firmware 3.0)

The device runs its own Wi-Fi network and serves the dashboard itself; no internet connection and no app are needed.

| Item | Value |
|---|---|
| Wi-Fi network (always on) | SSID `DiaGnostix`, password `diagnostix` (WPA2), device at `192.168.4.1` |
| Sign-in page | opens by itself after joining (captive portal); otherwise `http://192.168.4.1/` |
| WebSocket | `ws://192.168.4.1/ws`: all messages below |
| HTTP (read-only, no sign-in) | `GET /api/state`: device status only, no personal data |
| Optional home network | `secrets.h` (`WIFI_SSID`, `WIFI_PASSWORD`): the device also joins it and is reachable at `http://diagnostix.local` |
| Bluetooth LE (`ENABLE_BLE 1`) | service `6e1d0001-8b3c-4a8e-9c4b-5f2d1a7c3e90`, same JSON messages |

**Conventions**
- Every message is one UTF-8 JSON object.
- Messages from the device carry a `"type"` field; commands to the device carry a `"cmd"` field.
- Values that are not available are `null`.
- `t_ms` is milliseconds since the device booted.
- `t` is wall-clock seconds (Unix time). The device has no clock battery: it takes the time from the browser at sign-in (`now`), so `t` is present only after someone has signed in since the last boot.

## Captive portal

The device answers every DNS query with its own address. The operating-system connectivity checks (`/generate_204`, `/gen_204`, `/hotspot-detect.html`, `/library/test/success.html`, `/connecttest.txt`, `/ncsi.txt`, `/redirect`, `/canonical.html`, `/success.txt`, `/fwlink`) and any request for another host are redirected to `http://192.168.4.1/`. Phones therefore show the sign-in page as soon as they join the network.

The network has no internet access. Keep using the dashboard in the sign-in window, or choose "Use without internet" / "Stay connected" when the phone asks, then open `http://192.168.4.1/` in the browser.

## Accounts and data

- Up to 20 accounts (`MAX_USERS`). A username is 3–24 characters from `a–z 0–9 . _ -` (case-insensitive); a password has at least 6 characters.
- Passwords are stored as PBKDF2-HMAC-SHA256 hashes (10 000 iterations, 16-byte random salt per account). They are never stored or sent back in clear text.
- Per-user data on the device's FAT partition (`/u/<id>/`):
  - account record;
  - profile (only if saved with `persist: true`);
  - blood-pressure calibration;
  - results history: the last 300, one JSON line each.
- Shared by all users: the AD8232 gain calibration (a property of the board) and the result counter.
- Sign-in sessions are random 128-bit tokens kept in RAM. A token expires 12 h after its last use and on reboot.
- Five wrong passwords on one connection block sign-in on it for 30 s.

**Who receives what**

| Data | Recipients |
|---|---|
| `hello` before sign-in | device, firmware, models, gain, whether a session is running; no personal data |
| profile, calibration, result, window progress, history, risk alert | only the clients signed in as that user |
| live `wave`, `vitals`, `beat`, `pat` | during a measurement only its owner; otherwise every signed-in client |
| `session` | every signed-in client; `mine` tells whether it belongs to the receiver |
| SpO₂ and fever alerts, `ecg_gain` | every signed-in client |

## Changes from firmware 2.0

| | 2.0 | 3.0 |
|---|---|---|
| Network | home Wi-Fi or access point | own access point always on, captive portal, optional home Wi-Fi in addition |
| Users | one profile per device | accounts with sign-in; profile, BP calibration and history per user |
| History | in the browser (`localStorage`) | on the device, per user (`history` command) |
| Analysis | all windows after the recording | each window while the next is recorded; `window` progress messages; result right after the recording |
| Result | — | `borderline`, `score_spread`, `latency_ms`, `t` |
| Live values | raw readings | outlier rejection + smoothing; ECG display stream band-pass 0.5–40 Hz + 60 Hz notch |

---

## Device → client messages

### `hello`
Sent after a client connects, after sign-in, and in reply to the `hello` command.

```json
{
  "type": "hello",
  "device": "DiaGnostix-ECG",
  "firmware": "3.0.0",
  "auth": { "signed_in": true, "username": "alice", "users": 3 },
  "stream": { "ecg_fs": 250, "ecg_unit": "mV, filtered for display", "ppg_fs": 50 },
  "model": { "version": "2.0.0", "self_test": true, "window_s": 10, "min_good_windows": 2, "threshold": 0.0912,
             "needs": ["age", "male_sex", "BMI"], "models": { "primary": { "...": "..." }, "fallback": {}, "ecg_only": {} } },
  "session": { "...": "session object" },
  "ecg_gain": { "gain": 1100.0, "calibrated": false, "method": "nominal" },
  "profile": { "...": "profile object, or null" },
  "calibration": { "calibrated": false, "sbp": 120, "dbp": 80, "pat_ms": 250 },
  "alert": { "...": "alert object" },
  "last_result": { "...": "this user's last valid result since boot, or null" }
}
```
- `auth.users` is the number of accounts on the device. The dashboard uses it to suggest "Create account" on a new device.
- `profile`, `calibration`, `alert` and `last_result` are present only when `auth.signed_in` is `true`.
- `model.threshold` and the per-model `high` and `elevated` values are **calibrated probabilities** at the tier boundaries.
- `needs` lists every non-ECG input that any of the three models uses.

### `auth`
Reply to `register`, `login`, `auth`, `logout` and `delete_account`, and sent when a command needs a sign-in.
```json
{ "type": "auth", "ok": true, "username": "alice", "token": "9f1c0e2a7b..." }
{ "type": "auth", "ok": false, "message": "Wrong username or password" }
```
After `ok: true` the device also sends a full `hello`.

### Profile object
```json
{ "age": 52, "gender": "male", "height_cm": 171.0, "weight_kg": 78.0, "bmi": 26.7, "smoking": null, "drinking": null }
```
- `gender` is `"female"` or `"male"`; it maps to the model input `male_sex`.
- `smoking` is `true`, `false` or `null`; `drinking` is `1` (rarely), `2` (occasionally), `3` (regularly) or `null`.
- Sent as `{"type": "profile", ...}` to the user's clients after every change.

### `ecg_gain`
```json
{ "type": "ecg_gain", "gain": 1043.2, "calibrated": true, "method": "sine" }
```
`method` is `nominal` (1100, not calibrated), `sine` (measured with a reference sine) or `entered` (typed in).

### `session`
```json
{ "type": "session", "state": "acquiring", "mode": "measure", "progress": 0.42, "elapsed_s": 13.9,
  "duration_s": 33, "beats": 14, "pats": 0, "message": "Keep still, breathe normally and do not talk", "mine": true }
```
- `state` is `idle`, `acquiring`, `complete` or `failed`; `mode` is `measure`, `calibrate` (BP) or `gain`.
- `mine` is `false` when another user owns the session. The dashboard then shows "In use by another user" and hides the details.
- A measurement lasts 3 s of warm-up plus N × 10 s windows: N = 3 by default, 2–6 allowed.

### `window`
Sent to the owner as soon as each 10-s window has been analysed, while the next window is being recorded.
```json
{ "type": "window", "index": 0, "of": 3, "status": "ok", "beats": 12, "hr": 71.3, "sqi": true }
```
`status` is `ok`, `leads_off`, `low_quality` or `error`.

### `result`
Sent to the owner right after the recording. Only the last window and the final step remain at that point, so `latency_ms` is the time from the last captured sample to the result.
```json
{
  "type": "result", "ok": true, "id": 17, "t_ms": 812345, "t": 1793000000,
  "model": "primary", "model_version": "2.0.0",
  "tier": "high", "positive": true, "risk": 0.2410, "score": 0.6120,
  "threshold": 0.0912, "threshold_elevated": 0.0508,
  "borderline": false, "score_spread": 0.0310, "latency_ms": 96,
  "advice": "Raised risk of prediabetes or diabetes. Please visit a clinic or hospital for a fasting plasma glucose or HbA1c blood test.",
  "windows_ok": 3, "windows_total": 3,
  "windows": [ { "status": "ok", "score": 0.6120, "beats": 12, "hr": 71.3, "beat_corr": 0.982, "sqi": true }, "..." ],
  "ecg": { "hr": 71.3, "rr_sd_ms": 31.0, "rmssd_ms": 28.4, "pr_ms": 152, "qrs_ms": 84, "qt_ms": 326, "qtc_ms": 355,
           "r_uv": 640, "t_r_ratio": 0.271 },
  "inputs": { "mean_rr_s": 0.842, "...": "every model input (ECG medians over the good windows + profile)" },
  "measurements": { "hr": 71.0, "sbp": null, "dbp": null, "pat_ms": null, "spo2": 97.0, "body_temp": 34.1,
                    "room_temp": 24.5, "sdnn": 31.2, "rmssd": 28.0, "beats": 34, "pats": 0, "bp_calibrated": false },
  "gain": { "gain": 1043.2, "calibrated": true, "method": "sine" },
  "compute_ms": 41,
  "warnings": []
}
```

| Field | Meaning |
|---|---|
| `risk` | Platt-calibrated probability of prediabetes or diabetes, from the median model score of the good windows |
| `tier` | `high`: score ≥ Youden threshold; `elevated`: score ≥ 90 %-sensitivity threshold; `low`: below both; `no_result`: fewer than two windows passed the signal-quality index |
| `borderline` | the good windows fall on both sides of a tier boundary; the median is reported and a warning asks for a repeat measurement |
| `score_spread` | highest minus lowest window score |
| `latency_ms` | last captured sample → result (on the device) |
| `compute_ms` | analysis time of the last window plus the final step |
| `ok` | `false` when `tier` is `no_result`; `note` then gives the reason |

**Which model is used**

| Profile saved | Gain calibrated | Model |
|---|---|---|
| yes | yes | `primary` |
| yes | no | `fallback` (no ECG amplitudes) |
| no | yes | `ecg_only` |
| no | no | none: the `start` command is rejected with the reason |

### `history`
Reply to the `history` command: the user's stored results, oldest first.
```json
{ "type": "history", "results": [
  { "id": 17, "t": 1793000000, "tier": "high", "model": "primary", "risk": 0.241, "score": 0.612, "borderline": false,
    "windows_ok": 3, "windows_total": 3, "hr": 71.3, "qtc_ms": 355, "rmssd_ms": 28.4, "sbp": null, "dbp": null,
    "spo2": 97.0, "latency_ms": 96 } ] }
```

### `alert`
```json
{ "type": "alert", "active": true, "kind": "risk",
  "message": "Raised risk of prediabetes or diabetes (24 %). Please visit a clinic or hospital for a fasting glucose or HbA1c blood test." }
```
`kind` is `risk` (high tier, sent to the owner only), `spo2`, `fever` or `none`. An alert stays until it is acknowledged (`ack` or the BOOT button).

### `vitals`, `beat`, `pat`, `wave`, `calibration`, `error`
- `vitals` (1 Hz): every value has passed outlier rejection and smoothing (`smoothing.h`: Hampel filter, then change detection, then exponential smoothing with a display dead-band). Blood pressure is computed from the smoothed pulse arrival time.
- `wave` (10 Hz): `ecg` in mV at the ADC, band-pass 0.5–40 Hz with a 60 Hz notch, decimated to 250 Hz. This is for display only: the model analyses the unfiltered 500 Hz capture. `ppg` is in arbitrary units at 50 Hz.
- `beat`, `pat`, `calibration` and `error` are unchanged from firmware 2.0.

---

## Client → device commands

| Command | Fields | Sign-in | Effect |
|---|---|---|---|
| `hello` | — | no | device replies with `hello` |
| `register` | `username`, `password`, `now` (browser `Date.now()`) | no | creates an account and signs in |
| `login` | `username`, `password`, `now` | no | signs in; reply `auth` with a `token` |
| `auth` | `token`, `now` | no | resumes a sign-in (page reload, reconnect) |
| `logout` | — | yes | signs this connection out and revokes its token |
| `delete_account` | `password` | yes | deletes the account and all its data; every client signed in as it is signed out |
| `history` | `limit` (1–300, default 60) | yes | replies with `history` |
| `start` | `duration_s` (optional, 23–63; default 33) | yes | starts a risk measurement for this user |
| `cancel` | — | yes | stops the running session (owner only) |
| `ack` | — | yes | acknowledges the active alert |
| `profile` | `age` (18–100), `gender`, `height_cm` (120–220), `weight_kg` (25–250), optional `smoking`, `drinking`, `persist` | yes | stores the user's profile; `"persist": false` keeps it in RAM only |
| `calibrate` | `sbp`, `dbp` (cuff reading) | yes | 20-s blood-pressure calibration for this user |
| `clear_calibration` | — | yes | back to the population BP default |
| `calibrate_gain` | `ref_mvpp` (0.2–2.5, default 1.0) | yes | 6-s AD8232 gain measurement from a reference sine between RA and LA |
| `set_gain` | `gain` (500–2000) | yes | stores a gain measured another way |
| `clear_gain` | — | yes | back to the nominal gain 1100 (not calibrated: the fallback model is used) |

Errors come back as `{"type": "error", "message": "..."}`. A command that needs a sign-in, sent without one, is answered with `{"type": "auth", "ok": false, "message": "Please sign in"}`.

The BOOT button starts a measurement for the user who signed in on the device most recently.
