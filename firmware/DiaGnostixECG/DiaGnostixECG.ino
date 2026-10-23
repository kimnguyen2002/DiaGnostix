// DiaGnostix-ECG: on-device prediabetes/diabetes risk screening from a single-lead (lead I) ECG and a
// user profile, on the ESP32-S3 N16R8.
//
// Arduino IDE settings (Tools menu) for the ESP32-S3-DevKitC-1 N16R8 (16 MB flash, 8 MB octal PSRAM)
//   Board            : ESP32S3 Dev Module   (esp32 by Espressif Systems, 3.x)
//   Flash Size       : 16MB (128Mb)
//   Partition Scheme : 16M Flash (3MB APP/9.9MB FATFS)   (the FAT partition holds the user accounts)
//   PSRAM            : OPI PSRAM            (required: the ECG analysis buffers live in PSRAM)
//   USB CDC On Boot  : Disabled for the "UART" USB port, Enabled for the "USB" port
//   everything else  : default (Flash Mode QIO 80MHz)
//
// Libraries (Sketch > Include Library > Manage Libraries)
//   ESP Async WebServer (ESP32Async), Async TCP (ESP32Async), ArduinoJson 7.x,
//   SparkFun MAX3010x Pulse and Proximity Sensor Library,
//   Adafruit BME280 Library (installs Adafruit Unified Sensor and BusIO)
//   NimBLE-Arduino (h2zero) 2.x only if ENABLE_BLE is set to 1 in config.h
// No TensorFlow Lite library is needed: the risk models are plain C (dgx_model_*.h).
//
// Using the device: join the Wi-Fi network "DiaGnostix" (password "diagnostix"); the sign-in page opens
// by itself (captive portal). Each user has an account with their own profile, blood-pressure
// calibration and results. Pins and all other parameters are in config.h; the wiring is
// docs/wiring-diagram.svg.
//
// Risk measurement
//   1. The ECG task samples the AD8232 at 500 Hz (analogReadMilliVolts) and records three 10-s windows
//      after a 3-s warm-up (analog_frontend.h).
//   2. Each window is analysed as soon as it has been captured, on core 0, while the next window is
//      recorded (analysis_worker.h): 32 lead-I features + signal-quality index (ecg_features.h).
//   3. When the recording ends only the last window is left; then the model scores, the median of the
//      SQI-passing windows (at least two), the calibrated risk and the tier (risk_model.h, ecg_risk.h).
//   4. Model: primary (profile + calibrated gain), gain-invariant fallback (profile, gain not
//      calibrated) or ECG-only (no profile, calibrated gain).
//   5. High tier: LED flashes and the dashboard asks the user to see a doctor for an FPG/HbA1c test.
//
// Task layout
//   core 1, "ecg"      : AD8232 sampling at 500 Hz, live QRS detection, ECG capture, NTC
//   core 1, "i2c"      : MAX30102 FIFO at 200 Hz, PPG processing, BME280
//   core 1, loop()     : accounts, sessions, PAT pairing, smoothing, results, alerts
//   core 0, "analysis" : ECG window analysis (low priority, yields to Wi-Fi)
//   core 0             : Wi-Fi, DNS (captive portal), AsyncTCP/WebSocket, NimBLE host
// Only loop() modifies application state. Sensor tasks publish through FreeRTOS queues and atomics;
// network tasks post raw commands to a queue.

#include <Arduino.h>
#include <esp_timer.h>
#include <sys/time.h>

#include "config.h"
#include "alert_led.h"
#include "analog_frontend.h"
#include "analysis_worker.h"
#include "app_types.h"
#include "ble_interface.h"
#include "bp_estimator.h"
#include "command_queue.h"
#include "dgx_selftest_vector.h"
#include "ecg_risk.h"
#include "i2c_frontend.h"
#include "measurement_session.h"
#include "pat_pairer.h"
#include "protocol.h"
#include "risk_model.h"
#include "settings_store.h"
#include "smoothing.h"
#include "stats.h"
#include "user_store.h"
#include "web_interface.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);

static AnalogFrontEnd g_analog;
static I2cFrontEnd g_i2c;
static dgx::EcgRiskEngine g_engine;
static AnalysisWorker g_worker;
static SettingsStore g_settings;
static UserStore g_users;
static LoginSessions g_logins;
static AlertLed g_led;
static WebInterface g_web;
static BleInterface g_ble;
static MeasurementSession g_session;
static dsp::PatPairer g_pairer;
static dsp::BpEstimator g_bp(BP_GAMMA_PER_MMHG);
static QueueHandle_t g_commands = nullptr;

// ---------------------------------------------------------------- users and connected clients
static UserProfile g_profiles[MAX_USERS];         // RAM copies (persisted ones are also on flash)
static dsp::BpCalibration g_bp_cals[MAX_USERS];   // person-specific PAT -> BP calibration
static int g_device_user = -1;                    // whose BP calibration the live display uses
static int g_session_user = -1;                   // owner of the running / last session

static ClientSlot g_clients[AP_MAX_CLIENTS + 4];

// ---------------------------------------------------------------- device state
static EcgGain g_gain;
static AlertState g_alert;
static RiskResult g_result;   // last valid result of g_result_user (sent to them on reconnect)
static RiskResult g_attempt;  // result being computed (the worker writes est.window[])
static int g_result_user = -1;
static bool g_model_ok = false;
static bool g_time_known = false;
static float g_cal_sbp = NAN, g_cal_dbp = NAN;
static float g_gain_ref_mvpp = GAIN_CAL_DEFAULT_REF_MVPP;

static int16_t* g_capture = nullptr;  // ECG_MAX_WINDOWS x 10 s of integer mV (PSRAM)
static int g_capture_windows = 0;     // windows armed for the running measurement
static int g_windows_posted = 0;      // windows handed to the analysis worker
static int g_windows_reported = 0;    // window progress messages sent
static int64_t g_capture_done_us = 0; // last sample captured (result latency starts here)
static bool g_finalize_pending = false;
static uint32_t g_finalize_since_ms = 0;

static dsp::RingBuffer<float, 10> g_recent_pat;
static dsp::RingBuffer<float, 30> g_recent_rr;
static uint32_t g_last_pat_ms = 0;
static uint32_t g_low_spo2_since = 0, g_fever_since = 0;
static bool g_spo2_latched = false, g_fever_latched = false;

// Displayed vitals: Hampel outlier rejection + EMA + deadband (smoothing.h, host-tested).
struct VitalFilters {
  dsp::VitalSmoother hr{dsp::vitalConfig(4.0f, 3.0f, 0.5f)};
  dsp::VitalSmoother ppg_hr{dsp::vitalConfig(4.0f, 3.0f, 0.5f)};
  dsp::VitalSmoother spo2{dsp::vitalConfig(5.0f, 1.5f, 0.25f)};
  dsp::VitalSmoother pat{dsp::vitalConfig(5.0f, 8.0f, 0.5f)};
  dsp::VitalSmoother body{dsp::vitalConfig(5.0f, 0.3f, 0.05f)};
  dsp::VitalSmoother room{dsp::vitalConfig(10.0f, 0.3f, 0.05f)};
  dsp::VitalSmoother humidity{dsp::vitalConfig(10.0f, 2.0f, 0.25f)};
  dsp::VitalSmoother pressure{dsp::vitalConfig(10.0f, 0.5f, 0.05f)};
};
static VitalFilters g_vf;

static uint32_t uptimeS() { return millis() / 1000; }

// ---------------------------------------------------------------------------
// Sending: personal data only to the owner's clients, live signals to every signed-in client
// ---------------------------------------------------------------------------

static void sendToClient(const ClientSlot& c, const String& json) {
  if (c.id == kBleClientId) {
    g_ble.notifyEvent(json);
  } else {
    g_web.sendTo(c.id, json);
  }
}

static void sendToUser(int user, const String& json) {
  if (user < 0) return;
  for (const ClientSlot& c : g_clients)
    if (c.used && c.user == user) sendToClient(c, json);
}

static void sendToSignedIn(const String& json) {
  for (const ClientSlot& c : g_clients)
    if (c.used && c.user >= 0) sendToClient(c, json);
}

// Live signals (waveforms, vitals, beats) belong to whoever is attached to the sensors: during a
// measurement only its owner receives them, otherwise every signed-in client does.
static bool liveTo(const ClientSlot& c) {
  if (!c.used || c.user < 0) return false;
  return g_session.state() != SessionState::kAcquiring || g_session_user < 0 || c.user == g_session_user;
}

static bool anyLiveListener() {
  for (const ClientSlot& c : g_clients)
    if (liveTo(c)) return true;
  return false;
}

static void sendLive(const String& json) {
  for (const ClientSlot& c : g_clients)
    if (liveTo(c)) sendToClient(c, json);
}

static ClientSlot* clientById(uint32_t id, bool create) {
  for (ClientSlot& c : g_clients)
    if (c.used && c.id == id) return &c;
  if (!create) return nullptr;
  for (ClientSlot& c : g_clients) {
    if (!c.used) {
      c = ClientSlot();
      c.used = true;
      c.id = id;
      return &c;
    }
  }
  return nullptr;
}

static protocol::SessionView sessionView(uint32_t now) {
  return {g_session.state(), g_session.mode(),  g_session.progress(now), g_session.elapsedS(now),
          g_session.durationS(), g_session.beats(), g_session.pats(),       g_session.message()};
}

static void publishSession(uint32_t now) {
  const protocol::SessionView v = sessionView(now);
  for (const ClientSlot& c : g_clients)
    if (c.used && c.user >= 0) sendToClient(c, protocol::session(v, c.user == g_session_user));
}

static String helloFor(const ClientSlot& c, uint32_t now) {
  const bool in = c.user >= 0 && g_users.valid(c.user);
  protocol::HelloInfo info{in ? g_users.username(c.user) : nullptr,
                           g_users.count(),
                           in && c.user == g_session_user,
                           in && g_profiles[c.user].valid ? &g_profiles[c.user] : nullptr,
                           in ? &g_bp_cals[c.user] : &g_bp.calibration(),
                           &g_gain,
                           sessionView(now),
                           &g_alert,
                           in && c.user == g_result_user ? &g_result : nullptr,
                           g_model_ok};
  return protocol::hello(info);
}

static String deviceSnapshot() {
  JsonDocument doc;  // device status only: GET /api/state needs no sign-in
  doc["device"] = DIAGNOSTIX_DEVICE_NAME;
  doc["firmware"] = DIAGNOSTIX_FIRMWARE_VERSION;
  doc["model_version"] = DGX_MODEL_VERSION;
  doc["users"] = g_users.count();
  doc["session"] = g_session.state() == SessionState::kAcquiring ? "busy" : "idle";
  doc["self_test"] = g_model_ok;
  String out;
  serializeJson(doc, out);
  return out;
}

static void publishError(int user, const char* message) {
  Serial.printf("[app] %s\n", message);
  if (user >= 0) {
    sendToUser(user, protocol::error(message));
  } else {
    sendToSignedIn(protocol::error(message));
  }
}

static void replyError(const ClientSlot& c, const char* message) {
  Serial.printf("[app] command rejected: %s\n", message);
  sendToClient(c, protocol::error(message));
}

// Transient LED feedback (result, error) never hides an active alert.
static void indicate(AlertLed::Pattern p, uint32_t now) {
  if (!g_alert.active()) g_led.setPattern(p, now);
}

static void setAlert(AlertKind kind, const char* message, uint32_t now, int user) {
  g_alert.kind = kind;
  std::snprintf(g_alert.message, sizeof(g_alert.message), "%s", message);
  switch (kind) {
    case AlertKind::kRisk: g_led.setPattern(AlertLed::Pattern::kRiskAlert, now); break;
    case AlertKind::kSpo2:
    case AlertKind::kFever: g_led.setPattern(AlertLed::Pattern::kSafetyAlert, now); break;
    case AlertKind::kNone: g_led.setPattern(AlertLed::Pattern::kIdle, now); break;
  }
  Serial.printf("[alert] %s %s\n", kind == AlertKind::kNone ? "cleared" : "RAISED", message);
  const String json = protocol::alert(g_alert);
  if (kind == AlertKind::kRisk && user >= 0) {
    sendToUser(user, json);  // a risk result is personal
  } else {
    sendToSignedIn(json);    // safety alerts and clearing concern whoever is at the device
  }
}

// ---------------------------------------------------------------------------
// Accounts
// ---------------------------------------------------------------------------

static void setClock(double now_ms) {
  if (!(now_ms > 1.6e12)) return;  // plausible epoch milliseconds only
  const int64_t now_s = static_cast<int64_t>(now_ms / 1000.0);
  if (g_time_known && llabs(now_s - static_cast<int64_t>(time(nullptr))) < 60) return;
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(now_s);
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  g_time_known = true;
}

static void useBpCalibrationOf(int user) {
  g_device_user = user;
  g_bp.setCalibration(user >= 0 ? g_bp_cals[user] : dsp::BpCalibration());
}

static void signIn(ClientSlot& c, int user, const char* token, uint32_t now) {
  c.user = user;
  c.failures = 0;
  std::snprintf(c.token, sizeof(c.token), "%s", token);
  if (g_session.state() != SessionState::kAcquiring) useBpCalibrationOf(user);
  Serial.printf("[auth] \"%s\" signed in\n", g_users.username(user));
  sendToClient(c, protocol::auth(true, g_users.username(user), token, nullptr));
  sendToClient(c, helloFor(c, now));
}

static void signOut(ClientSlot& c, const char* message) {
  if (c.token[0]) g_logins.revoke(c.token);
  c.user = -1;
  c.token[0] = '\0';
  sendToClient(c, protocol::auth(false, nullptr, nullptr, message));
}

static void loadUsers() {
  for (int id = 0; id < MAX_USERS; ++id) {
    g_profiles[id] = UserProfile();
    g_bp_cals[id] = dsp::BpCalibration();
    if (!g_users.valid(id)) continue;
    g_users.loadProfile(id, &g_profiles[id]);
    g_users.loadBpCalibration(id, &g_bp_cals[id]);
  }
}

// ---------------------------------------------------------------------------
// Model inputs and self-test
// ---------------------------------------------------------------------------

static dgx::NonEcgInputs profileInputs(int user) {
  dgx::NonEcgInputs in;
  if (user < 0 || !g_profiles[user].valid) return in;
  const UserProfile& p = g_profiles[user];
  in.age = p.age_years;
  in.male_sex = p.gender;
  in.bmi = p.bmi();
  if (p.smoking >= 0) in.smoking = p.smoking;
  if (p.drinking >= 1) in.drinking = p.drinking;
  return in;
}

static bool modelUsesBp(dgx::ModelRole role) {
  if (role == dgx::ModelRole::kNone) return false;
  const dgx::ModelSpec& m = dgx::modelSpec(role);
  return dgx::modelUses(m, "sBP") || dgx::modelUses(m, "dBP");
}

// Boot self-test: every model input is known, and one recorded ECG window gives the features and the
// ECG-only score computed in Python (dgx_selftest_vector.h). Also measures the per-window analysis time.
static bool runSelfTest() {
  const char* unknown = "";
  if (!dgx::modelInputsKnown(&unknown)) {
    Serial.printf("[model] ERROR: the firmware does not know the model input \"%s\"\n", unknown);
    return false;
  }
  if (!g_engine.ready()) return false;
  static dgx::RiskEstimate est;
  const int64_t t0 = esp_timer_get_time();
  g_engine.evaluate(DGX_SELFTEST_MV, 1, DGX_SELFTEST_GAIN, DGX_SELFTEST_REF_MV, false, ECG_ADC_MAX_MV,
                    dgx::ModelRole::kEcgOnly, dgx::NonEcgInputs(), 1, est);
  const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - t0);
  bool ok = est.n_good == 1 && est.window[0].features.n_beats == DGX_SELFTEST_N_BEATS &&
            std::fabs(est.score - DGX_SELFTEST_ECG_SCORE) < 1e-6;
  double worst = 0.0;
  for (int k = 0; k < dgx::kNumFeatures; ++k) {
    const double e = DGX_SELFTEST_FEATURES[k], a = est.window[0].features.value[k];
    if (std::isnan(e) != std::isnan(a)) ok = false;
    if (!std::isnan(e)) worst = std::fmax(worst, std::fabs(a - e) / std::fmax(1.0, std::fabs(e)));
  }
  if (worst > 1e-6) ok = false;
  Serial.printf("[model] self-test %s: score %.6f (expected %.6f), worst feature error %.1e, "
                "analysis %lu ms per 10-s window\n",
                ok ? "passed" : "FAILED", est.score, DGX_SELFTEST_ECG_SCORE, worst, static_cast<unsigned long>(us / 1000));
  return ok;
}

// ---------------------------------------------------------------------------
// Risk measurement: windows analysed during the recording, result finalised right after it
// ---------------------------------------------------------------------------

// Hands every newly completed 10-s window to the analysis worker and reports finished windows.
static void feedAnalysis() {
  if (g_session.mode() != SessionMode::kMeasure) return;
  if (g_session.state() != SessionState::kAcquiring && !g_finalize_pending) return;
  const size_t captured = g_analog.captured();
  while (g_windows_posted < g_capture_windows &&
         captured >= static_cast<size_t>(g_windows_posted + 1) * dgx::kWindowSamples) {
    g_worker.post(g_windows_posted, g_capture + static_cast<size_t>(g_windows_posted) * dgx::kWindowSamples);
    ++g_windows_posted;
  }
  if (!g_capture_done_us && captured >= static_cast<size_t>(g_capture_windows) * dgx::kWindowSamples) {
    g_capture_done_us = esp_timer_get_time();
  }
  while (g_windows_reported < g_worker.done()) {
    const dgx::WindowResult& w = g_attempt.est.window[g_windows_reported];
    sendToUser(g_session_user, protocol::windowProgress(g_windows_reported, g_capture_windows, w));
    Serial.printf("[ecg] window %d/%d: %s, %d beats, HR %.0f, beat corr %.3f\n", g_windows_reported + 1,
                  g_capture_windows, dgx::windowStatusName(w.status), w.features.n_beats,
                  w.features.value[dgx::F_HR_MEAN_BPM], w.features.beat_corr);
    ++g_windows_reported;
  }
}

static void finalizeRisk(uint32_t now) {
  g_finalize_pending = false;
  g_analog.stopCapture();
  const int user = g_session_user;
  const SessionSummary& s = g_session.summary();
  RiskResult& r = g_attempt;
  r.user = user;
  r.summary = s;
  r.gain = g_gain;
  r.inputs = profileInputs(user);
  if (g_session.requiresFinger() || std::isfinite(s.pat_ms)) {
    const dsp::BpEstimate bp = g_bp.estimate(s.pat_ms);
    r.sbp = bp.sbp_mmHg;
    r.dbp = bp.dbp_mmHg;
    r.bp_calibrated = g_bp.calibration().calibrated;
    if (r.bp_calibrated && std::isfinite(r.sbp) && std::isfinite(r.dbp)) {  // uncalibrated BP is never a model input
      r.inputs.sbp = r.sbp;
      r.inputs.dbp = r.dbp;
    }
  }
  const char* why = nullptr;
  const dgx::ModelRole role = dgx::chooseModel(g_gain.calibrated, r.inputs, &why);
  r.est.n_windows = g_windows_posted;  // windows never captured (session cut short) are not used
  const int64_t t0 = esp_timer_get_time();
  dgx::EcgRiskEngine::finalize(role, r.inputs, ECG_MIN_GOOD_WINDOWS, r.est);
  const int64_t t1 = esp_timer_get_time();
  r.est.compute_us = g_worker.lastWindowUs() + static_cast<uint32_t>(t1 - t0);
  r.est.latency_ms = g_capture_done_us ? static_cast<uint32_t>((t1 - g_capture_done_us) / 1000) : 0;
  r.id = g_settings.nextResultId();
  r.t_ms = now;
  r.t_epoch = g_time_known ? static_cast<int64_t>(time(nullptr)) : 0;

  if (role == dgx::ModelRole::kNone) {
    std::snprintf(r.note, sizeof(r.note), "%s", why ? why : "No model can be used");
  } else if (r.est.tier == dgx::RiskTier::kNoResult) {
    std::snprintf(r.note, sizeof(r.note), "Only %d of %d ECG windows passed the signal-quality check (%d needed)",
                  r.est.n_good, r.est.n_windows, ECG_MIN_GOOD_WINDOWS);
  }
  const bool ok = r.est.tier != dgx::RiskTier::kNoResult;
  Serial.printf("[result] #%lu %s: model %s, %d/%d windows, score %.4f, risk %.1f %% -> %s%s | latency %lu ms "
                "after the last sample (last window %lu ms)%s%s\n",
                static_cast<unsigned long>(r.id), g_users.username(user), dgx::roleName(role), r.est.n_good,
                r.est.n_windows, r.est.score, r.est.risk * 100.0, dgx::tierName(r.est.tier),
                r.est.borderline ? " (borderline)" : "", static_cast<unsigned long>(r.est.latency_ms),
                static_cast<unsigned long>(g_worker.lastWindowUs() / 1000), r.note[0] ? " | " : "", r.note);

  sendToUser(user, protocol::result(r, true));
  if (!ok) {
    g_session.fail(r.note);
    publishSession(now);
    indicate(AlertLed::Pattern::kError, now);
    return;
  }
  g_users.appendResult(user, protocol::resultLine(r));
  g_result = r;
  g_result_user = user;
  switch (r.est.tier) {
    case dgx::RiskTier::kHigh: {
      char msg[160];
      std::snprintf(msg, sizeof(msg),
                    "Raised risk of prediabetes or diabetes (%.0f %%). Please visit a clinic or hospital for a "
                    "fasting glucose or HbA1c blood test.", r.est.risk * 100.0);
      setAlert(AlertKind::kRisk, msg, now, user);
      break;
    }
    case dgx::RiskTier::kElevated: indicate(AlertLed::Pattern::kResultElevated, now); break;
    default: indicate(AlertLed::Pattern::kResultNegative, now); break;
  }
}

static void finishGainCalibration(uint32_t now) {
  g_finalize_pending = false;
  g_analog.stopCapture();
  const int n = static_cast<int>(g_analog.captured());
  for (int i = 0; i < n; ++i) {
    if (g_capture[i] >= ECG_ADC_MAX_MV || g_capture[i] <= 0) {
      g_session.fail("The reference signal reached the ADC limit; use a smaller amplitude (e.g. 1 mV peak-to-peak)");
      publishSession(now);
      indicate(AlertLed::Pattern::kError, now);
      return;
    }
  }
  const double gain = dgx::EcgRiskEngine::gainFromSine(g_capture, n, g_gain_ref_mvpp);
  if (!(gain >= ECG_GAIN_MIN && gain <= ECG_GAIN_MAX)) {
    char msg[128];
    std::snprintf(msg, sizeof(msg), "Measured gain %.0f is implausible (expected %.0f-%.0f); check the reference signal",
                  gain, ECG_GAIN_MIN, ECG_GAIN_MAX);
    g_session.fail(msg);
    publishSession(now);
    indicate(AlertLed::Pattern::kError, now);
    return;
  }
  g_gain.gain = static_cast<float>(gain);
  g_gain.calibrated = true;
  g_gain.method = EcgGain::kSineReference;
  g_settings.saveGain(g_gain);
  Serial.printf("[ecg] gain calibrated: %.1f (nominal %.0f, %+.1f %%) from a %.2f mV p-p reference\n", gain,
                ECG_NOMINAL_GAIN, 100.0 * (gain / ECG_NOMINAL_GAIN - 1.0), g_gain_ref_mvpp);
  sendToSignedIn(protocol::gain(g_gain));
  indicate(AlertLed::Pattern::kResultNegative, now);
}

static void finishBpCalibration(const SessionSummary& s, uint32_t now) {
  const int user = g_session_user;
  if (!dsp::BpEstimator::plausiblePat(s.pat_ms)) {
    publishError(user, "Calibration failed: pulse arrival time out of range; check the sensors");
    indicate(AlertLed::Pattern::kError, now);
    return;
  }
  dsp::BpCalibration cal;
  cal.sbp0_mmHg = g_cal_sbp;
  cal.dbp0_mmHg = g_cal_dbp;
  cal.pat0_ms = s.pat_ms;
  cal.calibrated = true;
  g_bp_cals[user] = cal;
  g_users.saveBpCalibration(user, cal);
  useBpCalibrationOf(user);
  Serial.printf("[bp] %s calibrated: %.0f/%.0f mmHg at PAT %.1f ms\n", g_users.username(user), cal.sbp0_mmHg,
                cal.dbp0_mmHg, cal.pat0_ms);
  sendToUser(user, protocol::calibration(cal));
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Returns nullptr if a session may start, otherwise the reason it may not.
static const char* sessionBlocker(SessionMode mode) {
  if (g_session.state() == SessionState::kAcquiring || g_finalize_pending) return "The device is busy with another measurement";
  if (mode != SessionMode::kCalibrate && !g_capture) {
    return "Not enough memory for the ECG capture: choose Tools > PSRAM > OPI PSRAM and upload again";
  }
  if (mode == SessionMode::kMeasure && !g_model_ok) {
    return "Model self-test failed; re-export the models with the notebook and upload again";
  }
  return nullptr;
}

// Model that a measurement started now would use; BP counts as available if it is calibrated.
static dgx::ModelRole plannedModel(int user, const char** why) {
  dgx::NonEcgInputs plan = profileInputs(user);
  if (g_bp_cals[user].calibrated) {
    plan.sbp = g_bp_cals[user].sbp0_mmHg;  // placeholders: the real values are measured in the session
    plan.dbp = g_bp_cals[user].dbp0_mmHg;
  }
  return dgx::chooseModel(g_gain.calibrated, plan, why);
}

static const char* startMeasurement(int user, uint32_t duration_s, uint32_t now) {
  const char* why = nullptr;
  const dgx::ModelRole role = plannedModel(user, &why);
  if (role == dgx::ModelRole::kNone) return why;
  if (g_alert.kind == AlertKind::kRisk) setAlert(AlertKind::kNone, "", now, -1);
  if (!duration_s) duration_s = SESSION_DEFAULT_DURATION_S;
  g_capture_windows = std::max(ECG_MIN_GOOD_WINDOWS,
                               std::min(ECG_MAX_WINDOWS, static_cast<int>(duration_s - SESSION_WARMUP_S) / ECG_WINDOW_S));
  const uint32_t duration = SESSION_WARMUP_S + g_capture_windows * ECG_WINDOW_S;
  g_session_user = user;
  useBpCalibrationOf(user);
  g_attempt = RiskResult();
  g_attempt.est.n_windows = g_capture_windows;
  g_windows_posted = g_windows_reported = 0;
  g_capture_done_us = 0;
  g_finalize_pending = false;
  g_worker.startSession(&g_attempt.est, g_gain.gain, ECG_INVERT);
  g_analog.startCapture(g_capture, static_cast<size_t>(g_capture_windows) * dgx::kWindowSamples,
                        SESSION_WARMUP_S * ECG_SAMPLE_RATE_HZ);
  g_session.start(SessionMode::kMeasure, duration, now, modelUsesBp(role));
  Serial.printf("[session] %s: measurement started, %d x %d-s ECG windows, planned model %s%s\n",
                g_users.username(user), g_capture_windows, ECG_WINDOW_S, dgx::roleName(role),
                modelUsesBp(role) ? " (finger sensor needed for BP)" : "");
  publishSession(now);
  return nullptr;
}

static bool signInAllowed(ClientSlot& c, uint32_t now) {
  if (c.blocked_until_ms && static_cast<int32_t>(c.blocked_until_ms - now) > 0) {
    sendToClient(c, protocol::auth(false, nullptr, nullptr, "Too many attempts; wait 30 s"));
    return false;
  }
  return true;
}

static void noteFailedSignIn(ClientSlot& c, uint32_t now, const char* message) {
  if (++c.failures >= 5) {
    c.failures = 0;
    c.blocked_until_ms = now + 30000;
  }
  sendToClient(c, protocol::auth(false, nullptr, nullptr, message));
}

static void handleCommand(const RawCommand& raw, uint32_t now) {
  const uint32_t id = raw.source == RawCommand::Source::kBle ? kBleClientId : raw.client_id;
  ClientSlot* cp = clientById(id, true);
  if (!cp) return;
  ClientSlot& c = *cp;
  const protocol::Command cmd = protocol::parseCommand(raw.json, raw.len);
  const int user = c.user;

  switch (cmd.type) {
    case protocol::CommandType::kInvalid: replyError(c, cmd.error); return;
    case protocol::CommandType::kHello: sendToClient(c, helloFor(c, now)); return;
    case protocol::CommandType::kRegister: {
      if (!signInAllowed(c, now)) return;
      int new_id = -1;
      if (const char* err = g_users.create(cmd.username, cmd.password, &new_id)) {
        sendToClient(c, protocol::auth(false, nullptr, nullptr, err));
        return;
      }
      setClock(cmd.now_ms);
      g_profiles[new_id] = UserProfile();
      g_bp_cals[new_id] = dsp::BpCalibration();
      Serial.printf("[auth] account \"%s\" created (%d users)\n", cmd.username, g_users.count());
      signIn(c, new_id, g_logins.create(new_id, uptimeS()), now);
      sendToSignedIn(String("{\"type\":\"users\",\"count\":") + g_users.count() + "}");
      return;
    }
    case protocol::CommandType::kLogin: {
      if (!signInAllowed(c, now)) return;
      const int uid = g_users.find(cmd.username);
      if (uid < 0 || !g_users.verify(uid, cmd.password)) {
        noteFailedSignIn(c, now, "Wrong username or password");
        return;
      }
      setClock(cmd.now_ms);
      signIn(c, uid, g_logins.create(uid, uptimeS()), now);
      return;
    }
    case protocol::CommandType::kAuth: {
      const int uid = g_logins.resolve(cmd.token, uptimeS());
      if (uid < 0 || !g_users.valid(uid)) {
        sendToClient(c, protocol::auth(false, nullptr, nullptr, "Please sign in again"));
        return;
      }
      setClock(cmd.now_ms);
      signIn(c, uid, cmd.token, now);
      return;
    }
    default: break;
  }

  if (user < 0 || !g_users.valid(user)) {  // everything below needs a signed-in user
    sendToClient(c, protocol::auth(false, nullptr, nullptr, "Please sign in"));
    return;
  }

  switch (cmd.type) {
    case protocol::CommandType::kLogout:
      Serial.printf("[auth] \"%s\" signed out\n", g_users.username(user));
      signOut(c, "Signed out");
      break;
    case protocol::CommandType::kDeleteAccount:
      if (!g_users.verify(user, cmd.password)) {
        replyError(c, "Wrong password: the account was not deleted");
        break;
      }
      if (g_session_user == user && g_session.state() == SessionState::kAcquiring) {
        g_session.cancel();
        g_analog.stopCapture();
      }
      Serial.printf("[auth] account \"%s\" deleted\n", g_users.username(user));
      g_users.remove(user);
      g_logins.revokeUser(user);
      g_profiles[user] = UserProfile();
      g_bp_cals[user] = dsp::BpCalibration();
      if (g_result_user == user) g_result_user = -1;
      if (g_session_user == user) g_session_user = -1;
      if (g_device_user == user) useBpCalibrationOf(-1);
      for (ClientSlot& other : g_clients) {
        if (other.used && other.user == user) {
          other.user = -1;
          other.token[0] = '\0';
          sendToClient(other, protocol::auth(false, nullptr, nullptr, "Account deleted"));
        }
      }
      break;
    case protocol::CommandType::kHistory:
      sendToClient(c, protocol::history(g_users.history(user, std::max(1, std::min(cmd.limit, HISTORY_MAX_RESULTS)))));
      break;
    case protocol::CommandType::kStart:
      if (const char* why = sessionBlocker(SessionMode::kMeasure)) {
        replyError(c, why);
      } else if (const char* why2 = startMeasurement(user, cmd.duration_s, now)) {
        replyError(c, why2);
      }
      break;
    case protocol::CommandType::kCancel:
      if (g_session_user != user) {
        replyError(c, "Only the person who started the measurement can cancel it");
        break;
      }
      g_session.cancel();
      g_analog.stopCapture();
      g_finalize_pending = false;
      break;
    case protocol::CommandType::kAck:
      if (g_alert.active()) {
        setAlert(AlertKind::kNone, "", now, -1);
      } else {
        sendToClient(c, protocol::alert(g_alert));
      }
      break;
    case protocol::CommandType::kProfile:
      g_profiles[user] = cmd.profile;
      if (cmd.persist) {
        g_users.saveProfile(user, g_profiles[user]);
      } else {
        g_users.clearProfile(user);  // RAM only: nothing identifying stays on the flash
      }
      Serial.printf("[profile] %s updated (%s)\n", g_users.username(user), cmd.persist ? "saved" : "RAM only");
      sendToUser(user, protocol::profile(g_profiles[user]));
      break;
    case protocol::CommandType::kCalibrate:
      if (const char* why = sessionBlocker(SessionMode::kCalibrate)) {
        replyError(c, why);
        break;
      }
      g_cal_sbp = cmd.sbp;
      g_cal_dbp = cmd.dbp;
      g_session_user = user;
      useBpCalibrationOf(user);
      g_session.start(SessionMode::kCalibrate, CALIBRATION_DURATION_S, now, true);
      Serial.printf("[bp] %s: calibration started with cuff %.0f/%.0f\n", g_users.username(user), cmd.sbp, cmd.dbp);
      publishSession(now);
      break;
    case protocol::CommandType::kClearCalibration:
      g_bp_cals[user] = dsp::BpCalibration();
      g_users.clearBpCalibration(user);
      if (g_device_user == user) useBpCalibrationOf(user);
      sendToUser(user, protocol::calibration(g_bp_cals[user]));
      break;
    case protocol::CommandType::kCalibrateGain:
      if (const char* why = sessionBlocker(SessionMode::kGainCalibrate)) {
        replyError(c, why);
        break;
      }
      g_gain_ref_mvpp = cmd.ref_mvpp;
      g_session_user = user;
      g_analog.startCapture(g_capture, (GAIN_CAL_DURATION_S - GAIN_CAL_SETTLE_S) * ECG_SAMPLE_RATE_HZ,
                            GAIN_CAL_SETTLE_S * ECG_SAMPLE_RATE_HZ);
      g_session.start(SessionMode::kGainCalibrate, GAIN_CAL_DURATION_S, now, false);
      Serial.printf("[ecg] gain calibration started with a %.2f mV p-p reference\n", cmd.ref_mvpp);
      publishSession(now);
      break;
    case protocol::CommandType::kSetGain:
      g_gain.gain = cmd.gain;
      g_gain.calibrated = true;
      g_gain.method = EcgGain::kEntered;
      g_settings.saveGain(g_gain);
      Serial.printf("[ecg] gain entered: %.1f\n", cmd.gain);
      sendToSignedIn(protocol::gain(g_gain));
      break;
    case protocol::CommandType::kClearGain:
      g_gain = EcgGain();
      g_settings.clearGain();
      Serial.println("[ecg] gain reset to nominal (not calibrated: the fallback model will be used)");
      sendToSignedIn(protocol::gain(g_gain));
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Periodic work
// ---------------------------------------------------------------------------

static Vitals collectVitals(uint32_t now) {
  static uint32_t last_ms = 0;
  const float dt = last_ms ? (now - last_ms) / 1000.0f : 1.0f;
  last_ms = now;
  Vitals v;
  v.t_ms = now;
  v.leads_on = g_analog.leadsOn();
  v.finger_on = g_i2c.fingerOn();
  v.hr_bpm = g_vf.hr.update(v.leads_on ? g_analog.heartRateBpm() : NAN, dt);
  v.ppg_hr_bpm = g_vf.ppg_hr.update(v.finger_on ? g_i2c.heartRateBpm() : NAN, dt);
  v.spo2 = g_vf.spo2.update(v.finger_on ? g_i2c.spo2() : NAN, dt);
  v.perfusion_index = g_i2c.perfusionIndex();

  if (!v.finger_on || !v.leads_on || now - g_last_pat_ms > 5000) g_recent_pat.clear();
  const float pat_raw = g_recent_pat.size() >= 3 ? dsp::median(g_recent_pat) : NAN;
  v.pat_ms = g_vf.pat.update(pat_raw, dt);
  if (std::isfinite(v.pat_ms)) {
    const dsp::BpEstimate bp = g_bp.estimate(v.pat_ms);  // BP from the smoothed PAT: consistent display
    v.sbp = bp.sbp_mmHg;
    v.dbp = bp.dbp_mmHg;
  }
  v.bp_calibrated = g_bp.calibration().calibrated;

  if (!v.leads_on) g_recent_rr.clear();
  if (g_recent_rr.size() >= 10) {
    float rr[30], clean[30];
    for (size_t i = 0; i < g_recent_rr.size(); ++i) rr[i] = g_recent_rr[i];
    const float med = dsp::median(g_recent_rr);
    size_t n = 0;
    for (size_t i = 0; i < g_recent_rr.size(); ++i) {
      if (std::fabs(rr[i] - med) <= 0.2f * med) clean[n++] = rr[i];
    }
    dsp::hrvTimeDomain(clean, n, &v.sdnn_ms, &v.rmssd_ms);
  }

  v.body_temp_c = g_vf.body.update(g_analog.bodyTemperatureC(), dt);
  const I2cFrontEnd::Environment env = g_i2c.environment();
  v.room_temp_c = g_vf.room.update(env.temp_c, dt);
  v.humidity = g_vf.humidity.update(env.humidity, dt);
  v.pressure_hpa = g_vf.pressure.update(env.pressure_hpa, dt);
  return v;
}

// Sustained low SpO2 or fever raises a local alert, independent of the risk model.
static void checkSafety(const Vitals& v, uint32_t now) {
  const bool low_spo2 = v.finger_on && std::isfinite(v.spo2) && v.spo2 < SAFETY_SPO2_THRESHOLD;
  if (!low_spo2) {
    g_low_spo2_since = 0;
    g_spo2_latched = false;
  } else if (!g_low_spo2_since) {
    g_low_spo2_since = now;
  } else if (!g_spo2_latched && now - g_low_spo2_since >= SAFETY_HOLD_S * 1000u) {
    g_spo2_latched = true;
    char msg[96];
    std::snprintf(msg, sizeof(msg), "Low blood oxygen: SpO2 %.0f %% for %d s", v.spo2, SAFETY_HOLD_S);
    setAlert(AlertKind::kSpo2, msg, now, -1);
  }

  const bool fever = std::isfinite(v.body_temp_c) && v.body_temp_c >= SAFETY_FEVER_THRESHOLD_C;
  if (!fever) {
    g_fever_since = 0;
    g_fever_latched = false;
  } else if (!g_fever_since) {
    g_fever_since = now;
  } else if (!g_fever_latched && now - g_fever_since >= SAFETY_HOLD_S * 1000u) {
    g_fever_latched = true;
    char msg[96];
    std::snprintf(msg, sizeof(msg), "High body temperature: %.1f C", v.body_temp_c);
    setAlert(AlertKind::kFever, msg, now, -1);
  }
}

static void processSensorEvents(uint32_t now) {
  const bool listeners = anyLiveListener();
  AnalogFrontEnd::BeatEvent beat;
  while (g_analog.receiveBeat(&beat)) {
    g_pairer.addRPeak(beat.r_time_us);
    g_session.onBeat(beat.rr_ms, now);
    if (beat.rr_ms > 0) g_recent_rr.push(beat.rr_ms);
    g_led.blip(now);
    if (listeners) sendLive(protocol::beat(static_cast<uint32_t>(beat.r_time_us / 1000), beat.rr_ms));
  }

  I2cFrontEnd::ArrivalEvent arrival;
  while (g_i2c.receiveArrival(&arrival)) g_pairer.addArrival(arrival.arrival_us);

  dsp::PatPairer::Pat pats[8];
  const int n = g_pairer.poll(esp_timer_get_time(), pats, 8);
  for (int i = 0; i < n; ++i) {
    const float pat_ms = pats[i].pat_ms;
    if (!dsp::BpEstimator::plausiblePat(pat_ms)) continue;
    g_recent_pat.push(pat_ms);
    g_last_pat_ms = now;
    g_session.onPat(pat_ms, now);
    if (listeners) {
      const dsp::BpEstimate bp = g_bp.estimate(pat_ms);
      sendLive(protocol::pat(static_cast<uint32_t>(pats[i].r_time_us / 1000), pat_ms, bp.sbp_mmHg, bp.dbp_mmHg));
    }
  }
}

static void streamWaveforms() {
  int16_t ecg[64];
  int16_t ppg[16];
  const size_t n_ecg = g_analog.readWaveform(ecg, 64);
  const size_t n_ppg = g_i2c.readWaveform(ppg, 16);
  if (!anyLiveListener()) return;
  static char buf[1024];
  const size_t len = protocol::wave(buf, sizeof(buf), ecg, n_ecg, ppg, n_ppg);
  if (!len) return;
  for (const ClientSlot& c : g_clients)
    if (liveTo(c) && c.id != kBleClientId) g_web.sendTo(c.id, buf, len);
}

static void handleButton(uint32_t now) {
  static bool last = true;
  static uint32_t changed_ms = 0;
  const bool level = digitalRead(PIN_BUTTON);  // active low
  if (level != last && now - changed_ms > 50) {
    changed_ms = now;
    last = level;
    if (level) return;  // act on press only
    if (g_alert.active()) {
      setAlert(AlertKind::kNone, "", now, -1);
      return;
    }
    if (g_session.state() == SessionState::kAcquiring || g_finalize_pending) return;
    // The button measures the user who used the device last (signed in on the dashboard).
    const char* why = g_device_user < 0 || !g_users.valid(g_device_user) ? "Sign in on the dashboard first" : nullptr;
    if (!why) why = sessionBlocker(SessionMode::kMeasure);
    if (!why) why = startMeasurement(g_device_user, 0, now);
    if (why) {
      publishError(-1, why);
      indicate(AlertLed::Pattern::kError, now);
    }
  }
}

// Reports the module that is really running, so a wrong Tools-menu setting is noticed at the first
// start. Expected on an N16R8: flash 16 MB, PSRAM 8 MB.
static void printBoardInfo() {
  Serial.printf("[board] %s rev %d, %d cores, flash %lu MB, PSRAM %lu MB, free heap %lu KB\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), static_cast<unsigned long>(ESP.getFlashChipSize() >> 20),
                static_cast<unsigned long>(ESP.getPsramSize() >> 20), static_cast<unsigned long>(ESP.getFreeHeap() >> 10));
  if (!psramFound()) {
    Serial.println("[board] WARNING: no PSRAM found. On the N16R8 choose Tools > PSRAM > OPI PSRAM; the ECG analysis "
                   "buffers are allocated in PSRAM.");
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n%s firmware %s, models %s (%s; primary inputs: %s)\n", DIAGNOSTIX_DEVICE_NAME,
                DIAGNOSTIX_FIRMWARE_VERSION, DGX_MODEL_VERSION, DGX_MODEL_FAMILY, DGX_PRIMARY_INPUT_SET);
  printBoardInfo();

  g_led.begin(PIN_ALERT_LED, PIN_BUZZER);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  g_settings.begin();
  if (g_settings.loadGain(&g_gain)) {
    Serial.printf("[ecg] AD8232 gain %.1f (calibrated)\n", g_gain.gain);
  } else {
    Serial.printf("[ecg] AD8232 gain not calibrated: nominal %.0f, the gain-invariant fallback model will be used\n",
                  ECG_NOMINAL_GAIN);
  }
  if (g_users.begin()) {
    loadUsers();
    Serial.printf("[auth] %d user account(s) on the FAT partition\n", g_users.count());
  } else {
    Serial.println("[auth] ERROR: FAT partition not available; choose Partition Scheme 16M Flash (3MB APP/9.9MB FATFS)");
  }

  const size_t cap_bytes = sizeof(int16_t) * ECG_MAX_WINDOWS * dgx::kWindowSamples;
  g_capture = static_cast<int16_t*>(psramFound() ? ps_malloc(cap_bytes) : malloc(cap_bytes));
  if (g_engine.begin() && g_capture) {
    Serial.printf("[ecg] analysis buffers: %u KB extractor + %u KB capture (%s)\n",
                  static_cast<unsigned>(dgx::EcgRiskEngine::workBytes() / 1024), static_cast<unsigned>(cap_bytes / 1024),
                  psramFound() ? "PSRAM" : "internal RAM");
    g_model_ok = runSelfTest() && g_worker.begin(&g_engine);
  } else {
    Serial.println("[ecg] ERROR: not enough memory for the ECG analysis buffers");
    g_capture = nullptr;
  }
  if (!g_model_ok) {
    Serial.println("[model] ERROR: risk measurements are disabled");
    g_led.setPattern(AlertLed::Pattern::kError, millis());
  }

  if (!g_analog.begin()) Serial.println("[ecg] ERROR: could not start the ECG task");
  if (!g_i2c.begin()) Serial.println("[i2c] ERROR: could not start the I2C task");
  if (!g_i2c.ppgPresent()) Serial.println("[ppg] WARNING: MAX30102 not found on I2C (SpO2 and BP unavailable)");
  if (!g_i2c.envPresent()) Serial.println("[env] WARNING: BME280 not found on I2C");

  g_commands = xQueueCreate(8, sizeof(RawCommand));
  g_web.begin(g_commands);
#if ENABLE_BLE
  g_ble.begin(g_commands, protocol::bleInfo(g_bp.calibration()));
#endif
  Serial.println("[app] ready: join the Wi-Fi \"" AP_SSID "\" and sign in on the page that opens");
}

void loop() {
  static uint32_t last_wave_ms = 0, last_second_ms = 0;
  const uint32_t now = millis();

  uint32_t client;
  while (g_web.takeNewClient(&client)) {
    if (ClientSlot* c = clientById(client, true)) sendToClient(*c, helloFor(*c, now));
  }
  while (g_web.takeGoneClient(&client)) {
    if (ClientSlot* c = clientById(client, false)) *c = ClientSlot();
  }

  RawCommand raw;
  while (xQueueReceive(g_commands, &raw, 0) == pdTRUE) handleCommand(raw, now);

  processSensorEvents(now);
  feedAnalysis();

  if (now - last_wave_ms >= 100) {
    last_wave_ms = now;
    streamWaveforms();
  }

  if (now - last_second_ms >= 1000) {
    last_second_ms = now;
    const Vitals v = collectVitals(now);
    g_session.onVitals(v, now);
    if (anyLiveListener()) sendLive(protocol::vitals(v));
    checkSafety(v, now);
    if (g_session.state() == SessionState::kAcquiring) publishSession(now);
    g_web.setStateSnapshot(deviceSnapshot());
  }

  if (g_session.update(now)) {
    const SessionState st = g_session.state();
    const SessionMode mode = g_session.mode();
    publishSession(now);
    if (st == SessionState::kComplete && mode != SessionMode::kCalibrate) {
      g_finalize_pending = true;  // finalise as soon as the last window has been captured and analysed
      g_finalize_since_ms = now;
    } else {
      g_analog.stopCapture();
      if (st == SessionState::kComplete) {
        finishBpCalibration(g_session.summary(), now);
      } else if (st == SessionState::kFailed) {
        Serial.printf("[session] failed: %s\n", g_session.message());
        indicate(AlertLed::Pattern::kError, now);
      } else {
        Serial.println("[session] cancelled");
      }
    }
  }
  if (g_finalize_pending) {
    if (g_session.mode() == SessionMode::kGainCalibrate) {
      if (g_analog.captureDone() || now - g_finalize_since_ms > 1500) finishGainCalibration(now);
    } else {
      feedAnalysis();
      const bool all_posted = g_windows_posted == g_capture_windows;
      const bool capture_stalled = now - g_finalize_since_ms > 1500;  // sample(s) missing at the end
      if ((all_posted || capture_stalled) && g_worker.done() == g_windows_posted) {
        feedAnalysis();  // report the last window first
        finalizeRisk(millis());
      } else if (now - g_finalize_since_ms > 15000) {
        g_finalize_pending = false;  // should not happen: the analysis task stalled
        g_session.fail("The ECG analysis did not finish; please measure again");
        publishSession(now);
      }
    }
  }

  handleButton(now);
  g_led.update(now);
  g_web.loop();
  vTaskDelay(1);
}
