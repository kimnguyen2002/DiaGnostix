// DiaGnostix-ECG: on-device prediabetes/diabetes risk screening from a single-lead (lead I) ECG and a
// user profile, on the ESP32-S3 N16R8.
//
// Arduino IDE settings (Tools menu) for the ESP32-S3-DevKitC-1 N16R8 (16 MB flash, 8 MB octal PSRAM)
//   Board            : ESP32S3 Dev Module   (esp32 by Espressif Systems, 3.x)
//   Flash Size       : 16MB (128Mb)
//   Partition Scheme : 16M Flash (3MB APP/9.9MB FATFS)
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
// Wi-Fi: copy secrets.example.h to secrets.h and enter your network. Pins and all other
// parameters are in config.h; the wiring is docs/wiring-diagram.svg.
//
// Risk measurement
//   1. The ECG task samples the AD8232 at 500 Hz (analogReadMilliVolts) and records three 10-s windows
//      after a 3-s warm-up (analog_frontend.h).
//   2. Each window: 32 lead-I features + signal-quality index (ecg_features.h, a line-by-line port of
//      the Python extractor the models were trained on), then a model score (risk_model.h).
//   3. Risk = median score of the windows that pass the SQI (at least two); tier high / elevated / low.
//   4. Model: primary (profile + calibrated gain), gain-invariant fallback (profile, gain not
//      calibrated) or ECG-only (no profile, calibrated gain).
//   5. High tier: LED flashes and the dashboard asks the user to see a doctor for an FPG/HbA1c test.
//
// Task layout
//   core 1, "ecg"  : AD8232 sampling at 500 Hz, live QRS detection, ECG capture, NTC (analog_frontend.h)
//   core 1, "i2c"  : MAX30102 FIFO at 200 Hz, PPG processing, BME280 (i2c_frontend.h)
//   core 1, loop() : application state, PAT pairing, sessions, risk analysis, alerts
//   core 0         : Wi-Fi, AsyncTCP/WebSocket, NimBLE host
// Only loop() modifies application state. Sensor tasks publish through FreeRTOS queues and atomics;
// network tasks post raw commands to a queue.

#include <Arduino.h>
#include <esp_timer.h>

#include "config.h"
#include "alert_led.h"
#include "analog_frontend.h"
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
#include "stats.h"
#include "web_interface.h"

SET_LOOP_TASK_STACK_SIZE(16 * 1024);  // the ECG analysis runs in loop()

static AnalogFrontEnd g_analog;
static I2cFrontEnd g_i2c;
static dgx::EcgRiskEngine g_engine;
static SettingsStore g_settings;
static AlertLed g_led;
static WebInterface g_web;
static BleInterface g_ble;
static MeasurementSession g_session;
static dsp::PatPairer g_pairer;
static dsp::BpEstimator g_bp(BP_GAMMA_PER_MMHG);
static QueueHandle_t g_commands = nullptr;

static UserProfile g_profile;
static EcgGain g_gain;
static AlertState g_alert;
static RiskResult g_result;   // last valid result (sent to new clients)
static RiskResult g_attempt;  // result being computed
static bool g_has_result = false;
static bool g_model_ok = false;
static float g_cal_sbp = NAN, g_cal_dbp = NAN;  // cuff reading of a running BP calibration
static float g_gain_ref_mvpp = GAIN_CAL_DEFAULT_REF_MVPP;

static int16_t* g_capture = nullptr;  // ECG_MAX_WINDOWS x 10 s of integer mV (PSRAM)
static int g_capture_windows = 0;     // windows armed for the running measurement
static bool g_analysis_pending = false;
static uint32_t g_analysis_since_ms = 0;

static dsp::RingBuffer<float, 10> g_recent_pat;
static dsp::RingBuffer<float, 30> g_recent_rr;
static uint32_t g_last_pat_ms = 0;
static uint32_t g_low_spo2_since = 0, g_fever_since = 0;
static bool g_spo2_latched = false, g_fever_latched = false;

// ---------------------------------------------------------------------------
// Publishing
// ---------------------------------------------------------------------------

static protocol::SessionView sessionView(uint32_t now) {
  return {g_session.state(), g_session.mode(),  g_session.progress(now), g_session.elapsedS(now),
          g_session.durationS(), g_session.beats(), g_session.pats(),       g_session.message()};
}

static String helloJson(uint32_t now) {
  protocol::HelloInfo info{g_profile.valid ? &g_profile : nullptr,
                           &g_bp.calibration(),
                           &g_gain,
                           sessionView(now),
                           &g_alert,
                           g_has_result ? &g_result : nullptr,
                           g_model_ok};
  return protocol::hello(info);
}

static void publishEvent(const String& json) {
  g_web.broadcast(json);
  g_ble.notifyEvent(json);
}

static void publishSession(uint32_t now) { publishEvent(protocol::session(sessionView(now))); }

// Errors that are not caused by a particular command go to every client.
static void publishError(const char* message) {
  Serial.printf("[app] %s\n", message);
  publishEvent(protocol::error(message));
}

// Replies to the client that sent a command.
static void reply(const RawCommand& to, const String& json) {
  if (to.source == RawCommand::Source::kWebSocket) {
    g_web.sendTo(to.client_id, json);
  } else {
    g_ble.notifyEvent(json);
  }
}

static void replyError(const RawCommand& to, const char* message) {
  Serial.printf("[app] command rejected: %s\n", message);
  reply(to, protocol::error(message));
}

// Transient LED feedback (result, error) never hides an active alert.
static void indicate(AlertLed::Pattern p, uint32_t now) {
  if (!g_alert.active()) g_led.setPattern(p, now);
}

static void setAlert(AlertKind kind, const char* message, uint32_t now) {
  g_alert.kind = kind;
  std::snprintf(g_alert.message, sizeof(g_alert.message), "%s", message);
  switch (kind) {
    case AlertKind::kRisk: g_led.setPattern(AlertLed::Pattern::kRiskAlert, now); break;
    case AlertKind::kSpo2:
    case AlertKind::kFever: g_led.setPattern(AlertLed::Pattern::kSafetyAlert, now); break;
    case AlertKind::kNone: g_led.setPattern(AlertLed::Pattern::kIdle, now); break;
  }
  Serial.printf("[alert] %s %s\n", kind == AlertKind::kNone ? "cleared" : "RAISED", message);
  publishEvent(protocol::alert(g_alert));
}

// ---------------------------------------------------------------------------
// Model inputs and self-test
// ---------------------------------------------------------------------------

static dgx::NonEcgInputs profileInputs() {
  dgx::NonEcgInputs in;
  if (!g_profile.valid) return in;
  in.age = g_profile.age_years;
  in.male_sex = g_profile.gender;
  in.bmi = g_profile.bmi();
  if (g_profile.smoking >= 0) in.smoking = g_profile.smoking;
  if (g_profile.drinking >= 1) in.drinking = g_profile.drinking;
  return in;
}

static bool modelUsesBp(dgx::ModelRole role) {
  if (role == dgx::ModelRole::kNone) return false;
  const dgx::ModelSpec& m = dgx::modelSpec(role);
  return dgx::modelUses(m, "sBP") || dgx::modelUses(m, "dBP");
}

// Boot self-test: every model input is known, and one recorded ECG window gives the features and the
// ECG-only score computed in Python (dgx_selftest_vector.h). Also measures the analysis time.
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
  Serial.printf("[model] self-test %s: score %.6f (expected %.6f), worst feature error %.1e, %lu ms per 10-s window\n",
                ok ? "passed" : "FAILED", est.score, DGX_SELFTEST_ECG_SCORE, worst, static_cast<unsigned long>(us / 1000));
  return ok;
}

// ---------------------------------------------------------------------------
// Risk measurement
// ---------------------------------------------------------------------------

static void runRiskAnalysis(uint32_t now) {
  g_analysis_pending = false;
  g_analog.stopCapture();
  const SessionSummary& s = g_session.summary();
  RiskResult& r = g_attempt;
  r = RiskResult();
  r.summary = s;
  r.gain = g_gain;
  r.inputs = profileInputs();
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
  const int n_windows = std::min<int>(g_capture_windows, static_cast<int>(g_analog.captured() / dgx::kWindowSamples));

  const int64_t t0 = esp_timer_get_time();
  g_engine.evaluate(g_capture, n_windows, g_gain.gain, ECG_ADC_REF_MV, ECG_INVERT, ECG_ADC_MAX_MV, role, r.inputs,
                    ECG_MIN_GOOD_WINDOWS, r.est);
  r.est.compute_us = static_cast<uint32_t>(esp_timer_get_time() - t0);
  r.id = g_settings.nextResultId();
  r.t_ms = now;

  for (int w = 0; w < r.est.n_windows; ++w) {
    const dgx::WindowResult& wr = r.est.window[w];
    Serial.printf("[ecg] window %d: %s, %d beats, HR %.0f, beat corr %.3f, score %.4f, clipped %d\n", w + 1,
                  dgx::windowStatusName(wr.status), wr.features.n_beats, wr.features.value[dgx::F_HR_MEAN_BPM],
                  wr.features.beat_corr, wr.score, wr.clipped_samples);
  }

  if (role == dgx::ModelRole::kNone) {
    std::snprintf(r.note, sizeof(r.note), "%s", why ? why : "No model can be used");
  } else if (r.est.tier == dgx::RiskTier::kNoResult) {
    std::snprintf(r.note, sizeof(r.note), "Only %d of %d ECG windows passed the signal-quality check (%d needed)",
                  r.est.n_good, r.est.n_windows, ECG_MIN_GOOD_WINDOWS);
  }
  const bool ok = r.est.tier != dgx::RiskTier::kNoResult;
  Serial.printf("[result] #%lu model %s, %d/%d windows, score %.4f, risk %.1f %% -> %s (analysis %lu ms)%s%s\n",
                static_cast<unsigned long>(r.id), dgx::roleName(role), r.est.n_good, r.est.n_windows, r.est.score,
                r.est.risk * 100.0, dgx::tierName(r.est.tier), static_cast<unsigned long>(r.est.compute_us / 1000),
                r.note[0] ? " | " : "", r.note);

  g_web.broadcast(protocol::result(r, true));
  g_ble.notifyEvent(protocol::result(r, false));
  if (!ok) {
    g_session.fail(r.note);
    publishSession(now);
    indicate(AlertLed::Pattern::kError, now);
    return;
  }
  g_result = r;
  g_has_result = true;
  switch (r.est.tier) {
    case dgx::RiskTier::kHigh: {
      char msg[160];
      std::snprintf(msg, sizeof(msg),
                    "Raised risk of prediabetes or diabetes (%.0f %%). Please visit a clinic or hospital for a "
                    "fasting glucose or HbA1c blood test.", r.est.risk * 100.0);
      setAlert(AlertKind::kRisk, msg, now);
      break;
    }
    case dgx::RiskTier::kElevated: indicate(AlertLed::Pattern::kResultElevated, now); break;
    default: indicate(AlertLed::Pattern::kResultNegative, now); break;
  }
}

static void finishGainCalibration(uint32_t now) {
  g_analysis_pending = false;
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
  publishEvent(protocol::gain(g_gain));
  indicate(AlertLed::Pattern::kResultNegative, now);
}

static void finishCalibration(const SessionSummary& s, uint32_t now) {
  if (!dsp::BpEstimator::plausiblePat(s.pat_ms)) {
    publishError("Calibration failed: pulse arrival time out of range; check the sensors");
    indicate(AlertLed::Pattern::kError, now);
    return;
  }
  dsp::BpCalibration cal;
  cal.sbp0_mmHg = g_cal_sbp;
  cal.dbp0_mmHg = g_cal_dbp;
  cal.pat0_ms = s.pat_ms;
  cal.calibrated = true;
  g_bp.setCalibration(cal);
  g_settings.saveCalibration(cal);
  Serial.printf("[bp] calibrated: %.0f/%.0f mmHg at PAT %.1f ms\n", cal.sbp0_mmHg, cal.dbp0_mmHg, cal.pat0_ms);
  publishEvent(protocol::calibration(cal));
  g_ble.setInfo(protocol::bleInfo(cal));
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Returns nullptr if a session may start, otherwise the reason it may not.
static const char* sessionBlocker(SessionMode mode) {
  if (g_session.state() == SessionState::kAcquiring || g_analysis_pending) return "A session is already running";
  if (mode != SessionMode::kCalibrate && !g_capture) {
    return "Not enough memory for the ECG capture: choose Tools > PSRAM > OPI PSRAM and upload again";
  }
  if (mode == SessionMode::kMeasure && !g_model_ok) {
    return "Model self-test failed; re-export the models with the notebook and upload again";
  }
  return nullptr;
}

// Model that a measurement started now would use; BP counts as available if it is calibrated.
static dgx::ModelRole plannedModel(const char** why) {
  dgx::NonEcgInputs plan = profileInputs();
  if (g_bp.calibration().calibrated) {
    plan.sbp = g_bp.calibration().sbp0_mmHg;  // placeholders: the real values are measured in the session
    plan.dbp = g_bp.calibration().dbp0_mmHg;
  }
  return dgx::chooseModel(g_gain.calibrated, plan, why);
}

static const char* startMeasurement(uint32_t duration_s, uint32_t now) {
  const char* why = nullptr;
  const dgx::ModelRole role = plannedModel(&why);
  if (role == dgx::ModelRole::kNone) return why;
  if (g_alert.kind == AlertKind::kRisk) setAlert(AlertKind::kNone, "", now);
  if (!duration_s) duration_s = SESSION_DEFAULT_DURATION_S;
  g_capture_windows = std::max(ECG_MIN_GOOD_WINDOWS,
                               std::min(ECG_MAX_WINDOWS, static_cast<int>(duration_s - SESSION_WARMUP_S) / ECG_WINDOW_S));
  const uint32_t duration = SESSION_WARMUP_S + g_capture_windows * ECG_WINDOW_S;
  g_analog.startCapture(g_capture, static_cast<size_t>(g_capture_windows) * dgx::kWindowSamples,
                        SESSION_WARMUP_S * ECG_SAMPLE_RATE_HZ);
  g_session.start(SessionMode::kMeasure, duration, now, modelUsesBp(role));
  Serial.printf("[session] measurement started: %d x %d-s ECG windows, planned model %s%s\n", g_capture_windows,
                ECG_WINDOW_S, dgx::roleName(role), modelUsesBp(role) ? " (finger sensor needed for BP)" : "");
  publishSession(now);
  return nullptr;
}

static void handleCommand(const RawCommand& raw, uint32_t now) {
  const protocol::Command cmd = protocol::parseCommand(raw.json, raw.len);
  switch (cmd.type) {
    case protocol::CommandType::kInvalid:
      replyError(raw, cmd.error);
      break;
    case protocol::CommandType::kHello:
      if (raw.source == RawCommand::Source::kWebSocket) reply(raw, helloJson(now));
      break;
    case protocol::CommandType::kStart:
      if (const char* why = sessionBlocker(SessionMode::kMeasure)) {
        replyError(raw, why);
      } else if (const char* why2 = startMeasurement(cmd.duration_s, now)) {
        replyError(raw, why2);
      }
      break;
    case protocol::CommandType::kCancel:
      g_session.cancel();
      g_analog.stopCapture();
      g_analysis_pending = false;
      break;
    case protocol::CommandType::kAck:
      if (g_alert.active()) {
        setAlert(AlertKind::kNone, "", now);
      } else {
        reply(raw, protocol::alert(g_alert));
      }
      break;
    case protocol::CommandType::kProfile:
      g_profile = cmd.profile;
      if (cmd.persist) {
        g_settings.saveProfile(g_profile);
      } else {
        g_settings.clearProfile();  // keep identifying data off the device
      }
      Serial.printf("[profile] updated (%s)\n", cmd.persist ? "saved" : "RAM only");
      publishEvent(protocol::profile(g_profile));
      break;
    case protocol::CommandType::kCalibrate:
      if (const char* why = sessionBlocker(SessionMode::kCalibrate)) {
        replyError(raw, why);
        break;
      }
      g_cal_sbp = cmd.sbp;
      g_cal_dbp = cmd.dbp;
      g_session.start(SessionMode::kCalibrate, CALIBRATION_DURATION_S, now, true);
      Serial.printf("[bp] calibration started with cuff %.0f/%.0f\n", cmd.sbp, cmd.dbp);
      publishSession(now);
      break;
    case protocol::CommandType::kClearCalibration:
      g_bp.setCalibration(dsp::BpCalibration());
      g_settings.clearCalibration();
      publishEvent(protocol::calibration(g_bp.calibration()));
      g_ble.setInfo(protocol::bleInfo(g_bp.calibration()));
      break;
    case protocol::CommandType::kCalibrateGain:
      if (const char* why = sessionBlocker(SessionMode::kGainCalibrate)) {
        replyError(raw, why);
        break;
      }
      g_gain_ref_mvpp = cmd.ref_mvpp;
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
      publishEvent(protocol::gain(g_gain));
      break;
    case protocol::CommandType::kClearGain:
      g_gain = EcgGain();
      g_settings.clearGain();
      Serial.println("[ecg] gain reset to nominal (not calibrated: the fallback model will be used)");
      publishEvent(protocol::gain(g_gain));
      break;
  }
}

// ---------------------------------------------------------------------------
// Periodic work
// ---------------------------------------------------------------------------

static Vitals collectVitals(uint32_t now) {
  Vitals v;
  v.t_ms = now;
  v.leads_on = g_analog.leadsOn();
  v.finger_on = g_i2c.fingerOn();
  v.hr_bpm = g_analog.heartRateBpm();
  v.ppg_hr_bpm = g_i2c.heartRateBpm();
  v.spo2 = g_i2c.spo2();
  v.perfusion_index = g_i2c.perfusionIndex();

  if (!v.finger_on || !v.leads_on || now - g_last_pat_ms > 5000) g_recent_pat.clear();
  if (g_recent_pat.size() >= 3) {
    v.pat_ms = dsp::median(g_recent_pat);
    const dsp::BpEstimate bp = g_bp.estimate(v.pat_ms);
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

  v.body_temp_c = g_analog.bodyTemperatureC();
  const I2cFrontEnd::Environment env = g_i2c.environment();
  v.room_temp_c = env.temp_c;
  v.humidity = env.humidity;
  v.pressure_hpa = env.pressure_hpa;
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
    setAlert(AlertKind::kSpo2, msg, now);
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
    setAlert(AlertKind::kFever, msg, now);
  }
}

static void processSensorEvents(uint32_t now) {
  AnalogFrontEnd::BeatEvent beat;
  while (g_analog.receiveBeat(&beat)) {
    g_pairer.addRPeak(beat.r_time_us);
    g_session.onBeat(beat.rr_ms, now);
    if (beat.rr_ms > 0) g_recent_rr.push(beat.rr_ms);
    g_led.blip(now);
    if (g_web.hasClients()) g_web.broadcast(protocol::beat(static_cast<uint32_t>(beat.r_time_us / 1000), beat.rr_ms));
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
    if (g_web.hasClients()) {
      const dsp::BpEstimate bp = g_bp.estimate(pat_ms);
      g_web.broadcast(protocol::pat(static_cast<uint32_t>(pats[i].r_time_us / 1000), pat_ms, bp.sbp_mmHg, bp.dbp_mmHg));
    }
  }
}

static void streamWaveforms() {
  int16_t ecg[64];
  int16_t ppg[16];
  const size_t n_ecg = g_analog.readWaveform(ecg, 64);
  const size_t n_ppg = g_i2c.readWaveform(ppg, 16);
  if (!g_web.hasClients()) return;
  static char buf[1024];
  const size_t len = protocol::wave(buf, sizeof(buf), ecg, n_ecg, ppg, n_ppg);
  if (len) g_web.broadcast(buf, len);
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
      setAlert(AlertKind::kNone, "", now);
      return;
    }
    if (g_session.state() == SessionState::kAcquiring || g_analysis_pending) return;
    const char* why = sessionBlocker(SessionMode::kMeasure);
    if (!why) why = startMeasurement(0, now);
    if (why) {
      publishError(why);
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
  if (g_settings.loadProfile(&g_profile)) Serial.println("[profile] loaded from flash");
  dsp::BpCalibration cal;
  if (g_settings.loadCalibration(&cal)) {
    g_bp.setCalibration(cal);
    Serial.printf("[bp] calibration %.0f/%.0f at PAT %.1f ms\n", cal.sbp0_mmHg, cal.dbp0_mmHg, cal.pat0_ms);
  } else {
    Serial.println("[bp] not calibrated: blood pressure uses population defaults");
  }
  if (g_settings.loadGain(&g_gain)) {
    Serial.printf("[ecg] AD8232 gain %.1f (calibrated)\n", g_gain.gain);
  } else {
    Serial.printf("[ecg] AD8232 gain not calibrated: nominal %.0f, the gain-invariant fallback model will be used\n",
                  ECG_NOMINAL_GAIN);
  }

  const size_t cap_bytes = sizeof(int16_t) * ECG_MAX_WINDOWS * dgx::kWindowSamples;
  g_capture = static_cast<int16_t*>(psramFound() ? ps_malloc(cap_bytes) : malloc(cap_bytes));
  if (g_engine.begin() && g_capture) {
    Serial.printf("[ecg] analysis buffers: %u KB extractor + %u KB capture (%s)\n",
                  static_cast<unsigned>(dgx::EcgRiskEngine::workBytes() / 1024), static_cast<unsigned>(cap_bytes / 1024),
                  psramFound() ? "PSRAM" : "internal RAM");
    g_model_ok = runSelfTest();
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
  Serial.println("[app] ready: press BOOT or use the dashboard to start a measurement");
}

void loop() {
  static uint32_t last_wave_ms = 0, last_second_ms = 0;
  const uint32_t now = millis();

  RawCommand raw;
  while (xQueueReceive(g_commands, &raw, 0) == pdTRUE) handleCommand(raw, now);

  uint32_t client;
  while (g_web.takeNewClient(&client)) g_web.sendTo(client, helloJson(now));

  processSensorEvents(now);

  if (now - last_wave_ms >= 100) {
    last_wave_ms = now;
    streamWaveforms();
  }

  if (now - last_second_ms >= 1000) {
    last_second_ms = now;
    const Vitals v = collectVitals(now);
    g_session.onVitals(v, now);
    const String json = protocol::vitals(v);
    g_web.broadcast(json);
    g_ble.notifyTelemetry(json);
    checkSafety(v, now);
    if (g_session.state() == SessionState::kAcquiring) publishSession(now);
    g_web.setStateSnapshot(helloJson(now));
  }

  if (g_session.update(now)) {
    const SessionState st = g_session.state();
    const SessionMode mode = g_session.mode();
    if (st == SessionState::kComplete && mode != SessionMode::kCalibrate) {
      g_analysis_pending = true;  // analyse once the last ECG sample has been captured
      g_analysis_since_ms = now;
    } else {
      g_analog.stopCapture();
      publishSession(now);
      if (st == SessionState::kComplete) {
        finishCalibration(g_session.summary(), now);
      } else if (st == SessionState::kFailed) {
        Serial.printf("[session] failed: %s\n", g_session.message());
        indicate(AlertLed::Pattern::kError, now);
      } else {
        Serial.println("[session] cancelled");
      }
    }
  }
  if (g_analysis_pending && (g_analog.captureDone() || now - g_analysis_since_ms > 1500)) {
    publishSession(now);
    if (g_session.mode() == SessionMode::kMeasure) {
      runRiskAnalysis(millis());
    } else {
      finishGainCalibration(millis());
    }
  }

  handleButton(now);
  g_led.update(now);
  g_web.loop();
  vTaskDelay(1);
}
