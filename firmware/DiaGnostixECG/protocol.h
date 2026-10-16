#pragma once

#include <Arduino.h>

#include <ArduinoJson.h>
#include <cstdio>
#include <cstring>

#include "app_types.h"
#include "bp_estimator.h"
#include "config.h"
#include "risk_model.h"

// JSON messages of docs/protocol.md, shared by the WebSocket and BLE transports.
namespace protocol {

enum class CommandType : uint8_t {
  kInvalid,
  kHello,
  kStart,
  kCancel,
  kAck,
  kProfile,
  kCalibrate,
  kClearCalibration,
  kCalibrateGain,
  kSetGain,
  kClearGain,
};

struct Command {
  CommandType type = CommandType::kInvalid;
  uint32_t duration_s = 0;  // kStart; 0 = default
  UserProfile profile;      // kProfile
  bool persist = true;      // kProfile
  float sbp = NAN;          // kCalibrate
  float dbp = NAN;
  float ref_mvpp = NAN;     // kCalibrateGain: peak-to-peak amplitude of the reference sine at the electrodes
  float gain = NAN;         // kSetGain
  char error[96] = "";      // set when type == kInvalid
};

// Parses and validates one command. On failure type is kInvalid and error holds a message for the client.
Command parseCommand(const char* data, size_t len);

// Live view of the session for serialisation.
struct SessionView {
  SessionState state;
  SessionMode mode;
  float progress;
  float elapsed_s;
  uint32_t duration_s;
  int beats;
  int pats;
  const char* message;
};

struct HelloInfo {
  const UserProfile* profile;  // nullptr if none
  const dsp::BpCalibration* calibration;
  const EcgGain* gain;
  SessionView session;
  const AlertState* alert;
  const RiskResult* last_result;  // nullptr if none
  bool model_self_test;
};

String hello(const HelloInfo& info);
String bleInfo(const dsp::BpCalibration& cal);
String vitals(const Vitals& v);
String session(const SessionView& s);
String result(const RiskResult& r, bool include_inputs);
String calibration(const dsp::BpCalibration& c);
String gain(const EcgGain& g);
String alert(const AlertState& a);
String profile(const UserProfile& p);
String error(const char* message);
String beat(uint32_t t_ms, float rr_ms);
String pat(uint32_t t_ms, float pat_ms, float sbp, float dbp);

// Waveform batch; written into buf (returns length, 0 if it does not fit).
size_t wave(char* buf, size_t cap, const int16_t* ecg, size_t n_ecg, const int16_t* ppg, size_t n_ppg);

// ---- implementation

namespace detail {

// Numbers are written with a fixed number of decimals; NaN becomes null.
inline void setNum(JsonObject o, const char* key, double v, int decimals = 1) {
  if (!std::isfinite(v)) {
    o[key] = nullptr;
    return;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
  o[key] = serialized(String(buf));
}

inline const char* stateName(SessionState s) {
  switch (s) {
    case SessionState::kAcquiring: return "acquiring";
    case SessionState::kComplete: return "complete";
    case SessionState::kFailed: return "failed";
    default: return "idle";
  }
}

inline const char* modeName(SessionMode m) {
  switch (m) {
    case SessionMode::kCalibrate: return "calibrate";
    case SessionMode::kGainCalibrate: return "gain";
    default: return "measure";
  }
}

inline const char* alertName(AlertKind k) {
  switch (k) {
    case AlertKind::kRisk: return "risk";
    case AlertKind::kSpo2: return "spo2";
    case AlertKind::kFever: return "fever";
    default: return "none";
  }
}

inline const char* gainMethodName(uint8_t m) {
  switch (m) {
    case EcgGain::kSineReference: return "sine";
    case EcgGain::kEntered: return "entered";
    default: return "nominal";
  }
}

inline void writeProfile(JsonObject o, const UserProfile& p) {
  setNum(o, "age", p.age_years, 0);
  o["gender"] = p.gender ? "male" : "female";
  setNum(o, "height_cm", p.height_m * 100.0f, 1);
  setNum(o, "weight_kg", p.weight_kg, 1);
  setNum(o, "bmi", p.bmi(), 1);
  if (p.smoking >= 0) o["smoking"] = p.smoking == 1; else o["smoking"] = nullptr;
  if (p.drinking >= 1) o["drinking"] = p.drinking; else o["drinking"] = nullptr;
}

inline void writeCalibration(JsonObject o, const dsp::BpCalibration& c) {
  o["calibrated"] = c.calibrated;
  setNum(o, "sbp", c.sbp0_mmHg, 0);
  setNum(o, "dbp", c.dbp0_mmHg, 0);
  setNum(o, "pat_ms", c.pat0_ms, 1);
}

inline void writeGain(JsonObject o, const EcgGain& g) {
  setNum(o, "gain", g.gain, 1);
  o["calibrated"] = g.calibrated;
  o["method"] = gainMethodName(g.method);
}

inline void writeSession(JsonObject o, const SessionView& s) {
  o["state"] = stateName(s.state);
  o["mode"] = modeName(s.mode);
  setNum(o, "progress", s.progress, 3);
  setNum(o, "elapsed_s", s.elapsed_s, 1);
  o["duration_s"] = s.duration_s;
  o["beats"] = s.beats;
  o["pats"] = s.pats;
  o["message"] = s.message;
}

inline void writeAlert(JsonObject o, const AlertState& a) {
  o["active"] = a.active();
  o["kind"] = alertName(a.kind);
  o["message"] = a.message;
}

// Non-ECG inputs a model uses (names as in the model headers).
inline void writeNonEcgNames(JsonArray a, const dgx::ModelSpec& m) {
  for (int i = 0; i < m.n_inputs; ++i)
    if (!dgx::isEcgInput(m.input_names[i])) a.add(m.input_names[i]);
}

inline bool usesAmplitudes(const dgx::ModelSpec& m) {
  for (int i = 0; i < m.n_inputs; ++i) {
    const char* n = m.input_names[i];
    const size_t len = std::strlen(n);
    if (len > 3 && !std::strcmp(n + len - 3, "_uv")) return true;
  }
  return false;
}

inline void writeModels(JsonObject model, bool self_test) {
  model["version"] = DGX_MODEL_VERSION;
  model["family"] = DGX_MODEL_FAMILY;
  model["primary_inputs"] = DGX_PRIMARY_INPUT_SET;
  model["self_test"] = self_test;
  model["window_s"] = ECG_WINDOW_S;
  model["min_good_windows"] = ECG_MIN_GOOD_WINDOWS;
  const dgx::ModelSpec& pri = dgx::modelSpec(dgx::ModelRole::kPrimary);
  setNum(model, "threshold", pri.calibrate(pri.highThreshold()), 4);  // high tier of the primary model
  JsonObject models = model["models"].to<JsonObject>();
  JsonArray needs = model["needs"].to<JsonArray>();
  for (dgx::ModelRole r : {dgx::ModelRole::kPrimary, dgx::ModelRole::kFallback, dgx::ModelRole::kEcgOnly}) {
    const dgx::ModelSpec& m = dgx::modelSpec(r);
    JsonObject o = models[m.name].to<JsonObject>();
    o["n_inputs"] = m.n_inputs;
    writeNonEcgNames(o["non_ecg_inputs"].to<JsonArray>(), m);
    o["uses_amplitudes"] = usesAmplitudes(m);
    setNum(o, "high", m.calibrate(m.highThreshold()), 4);
    setNum(o, "elevated", m.calibrate(m.elevatedThreshold()), 4);
    for (int i = 0; i < m.n_inputs; ++i) {
      const char* n = m.input_names[i];
      if (dgx::isEcgInput(n)) continue;
      bool seen = false;
      for (JsonVariantConst v : needs) seen = seen || !std::strcmp(v.as<const char*>(), n);
      if (!seen) needs.add(n);
    }
  }
}

inline void writeResult(JsonObject o, const RiskResult& r, bool include_inputs) {
  const dgx::RiskEstimate& e = r.est;
  const bool has_model = e.role != dgx::ModelRole::kNone;
  o["id"] = r.id;
  o["t_ms"] = r.t_ms;
  o["model"] = dgx::roleName(e.role);
  o["model_version"] = DGX_MODEL_VERSION;
  o["tier"] = dgx::tierName(e.tier);
  o["positive"] = e.tier == dgx::RiskTier::kHigh;
  setNum(o, "risk", e.risk, 4);
  setNum(o, "score", e.score, 4);
  if (has_model) {
    const dgx::ModelSpec& m = dgx::modelSpec(e.role);
    setNum(o, "threshold", m.calibrate(m.highThreshold()), 4);
    setNum(o, "threshold_elevated", m.calibrate(m.elevatedThreshold()), 4);
  }
  o["advice"] = dgx::tierAdvice(e.tier);
  if (r.note[0]) o["note"] = r.note;
  o["windows_ok"] = e.n_good;
  o["windows_total"] = e.n_windows;
  JsonArray wins = o["windows"].to<JsonArray>();
  int clipped = 0;
  for (int w = 0; w < e.n_windows; ++w) {
    const dgx::WindowResult& wr = e.window[w];
    JsonObject wo = wins.add<JsonObject>();
    wo["status"] = dgx::windowStatusName(wr.status);
    setNum(wo, "score", wr.score, 4);
    wo["beats"] = wr.features.n_beats;
    setNum(wo, "hr", wr.features.value[dgx::F_HR_MEAN_BPM], 1);
    setNum(wo, "beat_corr", wr.features.beat_corr, 3);
    wo["sqi"] = wr.features.sqi_ok;
    clipped += wr.clipped_samples;
  }
  JsonObject ecg = o["ecg"].to<JsonObject>();
  setNum(ecg, "hr", e.summary[dgx::F_HR_MEAN_BPM], 1);
  setNum(ecg, "rr_sd_ms", e.summary[dgx::F_SD_RR_S] * 1000.0, 1);
  setNum(ecg, "rmssd_ms", e.summary[dgx::F_RMSSD_MS], 1);
  setNum(ecg, "pr_ms", e.summary[dgx::F_PR_PEAK_MS], 0);
  setNum(ecg, "qrs_ms", e.summary[dgx::F_QRS_MEAN_MS], 0);
  setNum(ecg, "qt_ms", e.summary[dgx::F_QT_PEAK_MS], 0);
  setNum(ecg, "qtc_ms", e.summary[dgx::F_QTC_PEAK_MS], 0);
  setNum(ecg, "r_uv", e.summary[dgx::F_MEAN_R_UV], 0);
  setNum(ecg, "t_r_ratio", e.summary[dgx::F_T_R_RATIO], 3);
  if (include_inputs && has_model) {
    const dgx::ModelSpec& m = dgx::modelSpec(e.role);
    JsonObject in = o["inputs"].to<JsonObject>();
    dgx::EcgFeatures f;
    for (int k = 0; k < dgx::kNumFeatures; ++k) f.value[k] = e.summary[k];
    for (int i = 0; i < m.n_inputs; ++i) {
      float v;
      if (dgx::inputValue(m.input_names[i], f, r.inputs, &v)) setNum(in, m.input_names[i], v, 3);
    }
  }
  JsonObject meas = o["measurements"].to<JsonObject>();
  setNum(meas, "hr", r.summary.hr_bpm, 1);
  setNum(meas, "sbp", r.sbp, 0);
  setNum(meas, "dbp", r.dbp, 0);
  setNum(meas, "pat_ms", r.summary.pat_ms, 1);
  setNum(meas, "spo2", r.summary.spo2, 1);
  setNum(meas, "body_temp", r.summary.body_temp_c, 1);
  setNum(meas, "room_temp", r.summary.room_temp_c, 1);
  setNum(meas, "sdnn", r.summary.sdnn_ms, 1);
  setNum(meas, "rmssd", r.summary.rmssd_ms, 1);
  meas["beats"] = r.summary.beats;
  meas["pats"] = r.summary.pats;
  meas["bp_calibrated"] = r.bp_calibrated;
  writeGain(o["gain"].to<JsonObject>(), r.gain);
  o["compute_ms"] = e.compute_us / 1000;

  JsonArray w = o["warnings"].to<JsonArray>();
  if (e.role == dgx::ModelRole::kFallback)
    w.add("The ECG gain is not calibrated, so the gain-invariant fallback model was used (ECG amplitudes ignored)");
  if (e.role == dgx::ModelRole::kEcgOnly)
    w.add("No profile entered: the ECG-only model was used, which is less accurate than the model with age, sex and BMI");
  if (has_model && e.n_good < e.n_windows) {
    char msg[96];
    std::snprintf(msg, sizeof(msg), "%d of %d ECG windows were rejected (leads off or low signal quality)",
                  e.n_windows - e.n_good, e.n_windows);
    w.add(msg);
  }
  if (clipped > e.n_windows * dgx::kWindowSamples / 100)
    w.add("The ECG reached the ADC limit; check the electrode placement (right wrist, left wrist, right leg)");
  if (std::isfinite(r.summary.body_temp_c) && r.summary.body_temp_c >= 37.5f)
    w.add("Raised body temperature: fever raises the heart rate and can raise the risk; measure again when well");
  if (std::isfinite(r.summary.spo2) && r.summary.spo2 < SAFETY_SPO2_THRESHOLD) w.add("Low SpO2 measured");
  if (std::isfinite(r.sbp) && !r.bp_calibrated)
    w.add("Blood pressure is an uncalibrated estimate; calibrate with a cuff for accurate values");
}

inline String finish(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  return out;
}

inline bool readBool(JsonVariantConst v, bool fallback) {
  if (v.is<bool>()) return v.as<bool>();
  if (v.is<int>()) return v.as<int>() != 0;
  return fallback;
}

inline float readNum(JsonVariantConst v) { return v.is<float>() ? v.as<float>() : NAN; }

inline Command invalid(const char* msg) {
  Command c;
  c.type = CommandType::kInvalid;
  std::snprintf(c.error, sizeof(c.error), "%s", msg);
  return c;
}

}  // namespace detail

using namespace detail;

inline Command parseCommand(const char* data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok || !doc.is<JsonObject>()) {
    return invalid("Command is not a JSON object");
  }
  const char* cmd = doc["cmd"] | "";
  Command c;

  if (!std::strcmp(cmd, "hello")) {
    c.type = CommandType::kHello;
  } else if (!std::strcmp(cmd, "start")) {
    c.type = CommandType::kStart;
    c.duration_s = doc["duration_s"] | 0u;
    if (c.duration_s != 0 && (c.duration_s < SESSION_MIN_DURATION_S || c.duration_s > SESSION_MAX_DURATION_S)) {
      static char msg[64];
      std::snprintf(msg, sizeof(msg), "duration_s must be between %d and %d", SESSION_MIN_DURATION_S,
                    SESSION_MAX_DURATION_S);
      return invalid(msg);
    }
  } else if (!std::strcmp(cmd, "cancel")) {
    c.type = CommandType::kCancel;
  } else if (!std::strcmp(cmd, "ack")) {
    c.type = CommandType::kAck;
  } else if (!std::strcmp(cmd, "clear_calibration")) {
    c.type = CommandType::kClearCalibration;
  } else if (!std::strcmp(cmd, "calibrate")) {
    c.type = CommandType::kCalibrate;
    c.sbp = readNum(doc["sbp"]);
    c.dbp = readNum(doc["dbp"]);
    if (!(c.sbp >= 70 && c.sbp <= 250 && c.dbp >= 40 && c.dbp <= 150 && c.sbp > c.dbp)) {
      return invalid("Cuff reading out of range (SBP 70-250, DBP 40-150 mmHg, SBP > DBP)");
    }
  } else if (!std::strcmp(cmd, "calibrate_gain")) {
    c.type = CommandType::kCalibrateGain;
    c.ref_mvpp = doc["ref_mvpp"].isNull() ? GAIN_CAL_DEFAULT_REF_MVPP : readNum(doc["ref_mvpp"]);
    if (!(c.ref_mvpp >= 0.2f && c.ref_mvpp <= 2.5f)) return invalid("ref_mvpp must be 0.2-2.5 mV peak-to-peak");
  } else if (!std::strcmp(cmd, "set_gain")) {
    c.type = CommandType::kSetGain;
    c.gain = readNum(doc["gain"]);
    if (!(c.gain >= ECG_GAIN_MIN && c.gain <= ECG_GAIN_MAX)) return invalid("gain must be 500-2000");
  } else if (!std::strcmp(cmd, "clear_gain")) {
    c.type = CommandType::kClearGain;
  } else if (!std::strcmp(cmd, "profile")) {
    c.type = CommandType::kProfile;
    UserProfile& p = c.profile;
    p.age_years = readNum(doc["age"]);
    const char* gender = doc["gender"] | "";
    if (!std::strcmp(gender, "male")) {
      p.gender = 1;
    } else if (!std::strcmp(gender, "female")) {
      p.gender = 0;
    } else {
      return invalid("gender must be \"female\" or \"male\"");
    }
    p.height_m = readNum(doc["height_cm"]) / 100.0f;
    p.weight_kg = readNum(doc["weight_kg"]);
    if (!(p.age_years >= 18 && p.age_years <= 100)) return invalid("Age must be 18-100 years");
    if (!(p.height_m >= 1.20f && p.height_m <= 2.20f)) return invalid("Height must be 120-220 cm");
    if (!(p.weight_kg >= 25 && p.weight_kg <= 250)) return invalid("Weight must be 25-250 kg");
    p.smoking = doc["smoking"].isNull() ? -1 : (readBool(doc["smoking"], false) ? 1 : 0);
    const int drinking = doc["drinking"] | -1;
    if (drinking != -1 && (drinking < 1 || drinking > 3)) return invalid("drinking must be 1 (rarely), 2 or 3 (regularly)");
    p.drinking = static_cast<int8_t>(drinking);
    p.valid = true;
    c.persist = readBool(doc["persist"], true);
  } else {
    return invalid("Unknown command");
  }
  return c;
}

inline String hello(const HelloInfo& info) {
  JsonDocument doc;
  doc["type"] = "hello";
  doc["device"] = DIAGNOSTIX_DEVICE_NAME;
  doc["firmware"] = DIAGNOSTIX_FIRMWARE_VERSION;
  JsonObject stream = doc["stream"].to<JsonObject>();
  stream["ecg_fs"] = ECG_SAMPLE_RATE_HZ / ECG_STREAM_DECIMATION;
  stream["ecg_unit"] = "mV at the ADC";
  stream["ppg_fs"] = PPG_SAMPLE_RATE_HZ / PPG_STREAM_DECIMATION;
  writeModels(doc["model"].to<JsonObject>(), info.model_self_test);

  if (info.profile && info.profile->valid) {
    writeProfile(doc["profile"].to<JsonObject>(), *info.profile);
  } else {
    doc["profile"] = nullptr;
  }
  writeCalibration(doc["calibration"].to<JsonObject>(), *info.calibration);
  writeGain(doc["ecg_gain"].to<JsonObject>(), *info.gain);
  writeSession(doc["session"].to<JsonObject>(), info.session);
  writeAlert(doc["alert"].to<JsonObject>(), *info.alert);
  if (info.last_result) {
    writeResult(doc["last_result"].to<JsonObject>(), *info.last_result, true);
  } else {
    doc["last_result"] = nullptr;
  }
  return finish(doc);
}

inline String bleInfo(const dsp::BpCalibration& cal) {
  JsonDocument doc;
  doc["device"] = DIAGNOSTIX_DEVICE_NAME;
  doc["firmware"] = DIAGNOSTIX_FIRMWARE_VERSION;
  doc["model_version"] = DGX_MODEL_VERSION;
  doc["calibrated"] = cal.calibrated;
  return finish(doc);
}

inline String vitals(const Vitals& v) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["type"] = "vitals";
  o["t_ms"] = v.t_ms;
  o["leads_on"] = v.leads_on;
  o["finger_on"] = v.finger_on;
  setNum(o, "hr", v.hr_bpm);
  setNum(o, "ppg_hr", v.ppg_hr_bpm);
  setNum(o, "spo2", v.spo2);
  setNum(o, "pi", v.perfusion_index, 2);
  setNum(o, "pat_ms", v.pat_ms);
  setNum(o, "sbp", v.sbp, 0);
  setNum(o, "dbp", v.dbp, 0);
  o["bp_calibrated"] = v.bp_calibrated;
  setNum(o, "sdnn", v.sdnn_ms);
  setNum(o, "rmssd", v.rmssd_ms);
  setNum(o, "body_temp", v.body_temp_c);
  setNum(o, "room_temp", v.room_temp_c);
  setNum(o, "humidity", v.humidity);
  setNum(o, "pressure", v.pressure_hpa);
  return finish(doc);
}

inline String session(const SessionView& s) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["type"] = "session";
  writeSession(o, s);
  return finish(doc);
}

inline String result(const RiskResult& r, bool include_inputs) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["type"] = "result";
  o["ok"] = r.est.tier != dgx::RiskTier::kNoResult;
  writeResult(o, r, include_inputs);
  return finish(doc);
}

inline String calibration(const dsp::BpCalibration& c) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["type"] = "calibration";
  writeCalibration(o, c);
  return finish(doc);
}

inline String gain(const EcgGain& g) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["type"] = "ecg_gain";
  writeGain(o, g);
  return finish(doc);
}

inline String alert(const AlertState& a) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["type"] = "alert";
  writeAlert(o, a);
  return finish(doc);
}

inline String profile(const UserProfile& p) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["type"] = "profile";
  writeProfile(o, p);
  return finish(doc);
}

inline String error(const char* message) {
  JsonDocument doc;
  doc["type"] = "error";
  doc["message"] = message;
  return finish(doc);
}

inline String beat(uint32_t t_ms, float rr_ms) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["type"] = "beat";
  o["t_ms"] = t_ms;
  setNum(o, "rr", rr_ms > 0 ? rr_ms : NAN, 1);
  return finish(doc);
}

inline String pat(uint32_t t_ms, float pat_ms, float sbp, float dbp) {
  JsonDocument doc;
  JsonObject o = doc.to<JsonObject>();
  o["type"] = "pat";
  o["t_ms"] = t_ms;
  setNum(o, "pat_ms", pat_ms, 1);
  setNum(o, "sbp", sbp, 1);
  setNum(o, "dbp", dbp, 1);
  return finish(doc);
}

inline size_t wave(char* buf, size_t cap, const int16_t* ecg, size_t n_ecg, const int16_t* ppg, size_t n_ppg) {
  size_t len = 0;
  auto append = [&](const char* fmt, int v) {
    if (len >= cap) return;
    const int n = std::snprintf(buf + len, cap - len, fmt, v);
    len = (n < 0 || len + n >= cap) ? cap : len + n;
  };
  auto appendText = [&](const char* s) {
    if (len >= cap) return;
    const int n = std::snprintf(buf + len, cap - len, "%s", s);
    len = (n < 0 || len + n >= cap) ? cap : len + n;
  };
  appendText("{\"type\":\"wave\",\"ecg\":[");
  for (size_t i = 0; i < n_ecg; ++i) append(i ? ",%d" : "%d", ecg[i]);
  appendText("],\"ppg\":[");
  for (size_t i = 0; i < n_ppg; ++i) append(i ? ",%d" : "%d", ppg[i]);
  appendText("]}");
  return len >= cap ? 0 : len;
}

}  // namespace protocol
