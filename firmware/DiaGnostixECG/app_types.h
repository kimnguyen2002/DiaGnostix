#pragma once

#include <cmath>
#include <cstdint>

#include "config.h"
#include "ecg_risk.h"

// Data exchanged between the firmware modules.

struct UserProfile {
  bool valid = false;
  float age_years = NAN;
  uint8_t gender = 0;     // 0 = female, 1 = male (male_sex in the training data)
  float height_m = NAN;
  float weight_kg = NAN;
  int8_t smoking = -1;    // -1 not answered, 0 no, 1 yes
  int8_t drinking = -1;   // -1 not answered, 1 rarely, 2 occasionally, 3 regularly

  float bmi() const { return height_m > 0 ? weight_kg / (height_m * height_m) : NAN; }
};

// AD8232 -> ADC gain used to convert the ECG to microvolts at the electrodes.
struct EcgGain {
  enum Method : uint8_t { kNominal = 0, kSineReference = 1, kEntered = 2 };
  float gain = ECG_NOMINAL_GAIN;
  bool calibrated = false;
  uint8_t method = kNominal;
};

// Live snapshot, refreshed once per second.
struct Vitals {
  uint32_t t_ms = 0;
  bool leads_on = false;
  bool finger_on = false;
  float hr_bpm = NAN;
  float ppg_hr_bpm = NAN;
  float spo2 = NAN;
  float perfusion_index = NAN;
  float pat_ms = NAN;
  float sbp = NAN;
  float dbp = NAN;
  bool bp_calibrated = false;
  float sdnn_ms = NAN;
  float rmssd_ms = NAN;
  float body_temp_c = NAN;
  float room_temp_c = NAN;
  float humidity = NAN;
  float pressure_hpa = NAN;
};

enum class SessionState : uint8_t { kIdle, kAcquiring, kComplete, kFailed };
// kMeasure: risk measurement; kCalibrate: blood-pressure calibration; kGainCalibrate: AD8232 gain calibration.
enum class SessionMode : uint8_t { kMeasure, kCalibrate, kGainCalibrate };

// Summary of a completed acquisition.
struct SessionSummary {
  SessionMode mode = SessionMode::kMeasure;
  float hr_bpm = NAN;
  float pat_ms = NAN;
  float spo2 = NAN;
  float body_temp_c = NAN;
  float room_temp_c = NAN;
  float sdnn_ms = NAN;
  float rmssd_ms = NAN;
  int beats = 0;
  int pats = 0;
};

// One risk measurement: the model estimate plus the context it was made in.
struct RiskResult {
  uint32_t id = 0;
  uint32_t t_ms = 0;
  int64_t t_epoch = 0;       // wall-clock seconds (set from the signed-in browser), 0 if unknown
  int user = -1;             // owner of the measurement (user_store.h id)
  dgx::RiskEstimate est;     // ~2 KB: held in a static instance, never on the stack
  dgx::NonEcgInputs inputs;  // the non-ECG values the model received
  SessionSummary summary;
  float sbp = NAN;
  float dbp = NAN;
  bool bp_calibrated = false;
  EcgGain gain;
  char note[160] = "";       // why there is no result, if so
};

enum class AlertKind : uint8_t { kNone, kRisk, kSpo2, kFever };

struct AlertState {
  AlertKind kind = AlertKind::kNone;
  char message[160] = "";
  bool active() const { return kind != AlertKind::kNone; }
};

// One connected dashboard (WebSocket client id) or the BLE link, and who is signed in on it.
struct ClientSlot {
  bool used = false;
  uint32_t id = 0;
  int user = -1;               // user_store.h id, -1 = not signed in
  char token[33] = "";
  uint8_t failures = 0;        // failed sign-ins (simple brute-force brake)
  uint32_t blocked_until_ms = 0;
};
static constexpr uint32_t kBleClientId = 0xFFFFFFFFu;
