#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "app_types.h"
#include "config.h"
#include "stats.h"

// Timed acquisition that turns a stream of beats, pulse arrival times and
// vitals into one robust summary (medians over the session). Used for a risk
// measurement, a blood-pressure calibration and an ECG gain calibration. The
// risk itself is computed from the captured ECG windows (ecg_risk.h), whose
// own signal-quality index decides whether the ECG is usable; this class only
// checks that the electrodes (and, when blood pressure is needed, the finger)
// stayed in place.
class MeasurementSession {
 public:
  // require_finger: the MAX30102 must see a finger (blood pressure is needed).
  void start(SessionMode mode, uint32_t duration_s, uint32_t now_ms, bool require_finger);
  void cancel();  // returns to kIdle

  // Event inputs from the main loop. Ignored unless acquiring.
  void onBeat(float rr_ms, uint32_t now_ms);
  void onPat(float pat_ms, uint32_t now_ms);
  void onVitals(const Vitals& v, uint32_t now_ms);  // once per second

  // Advances the state machine. Returns true exactly once when the session
  // leaves kAcquiring (state is then kComplete, kFailed or kIdle).
  bool update(uint32_t now_ms);

  // Marks a completed session as failed afterwards (e.g. no usable ECG window).
  void fail(const char* message);

  SessionState state() const { return state_; }
  SessionMode mode() const { return mode_; }
  float progress(uint32_t now_ms) const;
  float elapsedS(uint32_t now_ms) const;
  uint32_t durationS() const { return duration_s_; }
  int beats() const { return n_rr_; }
  int pats() const { return n_pat_; }
  bool requiresFinger() const { return require_finger_; }
  const char* message() const { return message_; }
  const SessionSummary& summary() const { return summary_; }

 private:
  static constexpr int kMaxBeats = 256;
  static constexpr int kMaxSeconds = SESSION_MAX_DURATION_S + 10;

  bool inWarmup(uint32_t now_ms) const;
  void finish();
  void setMessage(const char* m);
  const char* holdMessage() const;

  SessionState state_ = SessionState::kIdle;
  SessionMode mode_ = SessionMode::kMeasure;
  bool require_finger_ = false;
  uint32_t start_ms_ = 0;
  uint32_t duration_s_ = 0;

  float rr_[kMaxBeats];
  int n_rr_ = 0;
  float pat_[kMaxBeats];
  int n_pat_ = 0;
  float spo2_[kMaxSeconds];
  int n_spo2_ = 0;
  float body_[kMaxSeconds];
  int n_body_ = 0;
  float room_[kMaxSeconds];
  int n_room_ = 0;
  int seconds_total_ = 0;
  int seconds_leads_ = 0;
  int seconds_finger_ = 0;

  bool finished_pending_ = false;
  SessionSummary summary_;
  char message_[128] = "";
};

namespace session_detail {

inline float medianOf(const float* v, int n) {
  if (n <= 0) return NAN;
  static float scratch[256];
  return dsp::median(v, static_cast<size_t>(n), scratch);
}

}  // namespace session_detail

inline void MeasurementSession::start(SessionMode mode, uint32_t duration_s, uint32_t now_ms, bool require_finger) {
  mode_ = mode;
  require_finger_ = require_finger;
  duration_s_ = duration_s;
  start_ms_ = now_ms;
  n_rr_ = n_pat_ = n_spo2_ = n_body_ = n_room_ = 0;
  seconds_total_ = seconds_leads_ = seconds_finger_ = 0;
  summary_ = SessionSummary();
  summary_.mode = mode;
  state_ = SessionState::kAcquiring;
  finished_pending_ = false;
  setMessage(holdMessage());
}

inline const char* MeasurementSession::holdMessage() const {
  return mode_ == SessionMode::kGainCalibrate ? "Keep the reference signal connected and steady"
                                              : "Keep still, breathe normally and do not talk";
}

inline void MeasurementSession::cancel() {
  if (state_ != SessionState::kAcquiring) return;
  state_ = SessionState::kIdle;
  setMessage("Measurement cancelled");
  finished_pending_ = true;
}

inline void MeasurementSession::fail(const char* message) {
  state_ = SessionState::kFailed;
  setMessage(message);
}

inline bool MeasurementSession::inWarmup(uint32_t now_ms) const {
  return now_ms - start_ms_ < SESSION_WARMUP_S * 1000u;
}

inline void MeasurementSession::onBeat(float rr_ms, uint32_t now_ms) {
  if (state_ != SessionState::kAcquiring || inWarmup(now_ms) || rr_ms <= 0.0f) return;
  if (n_rr_ < kMaxBeats) rr_[n_rr_++] = rr_ms;
}

inline void MeasurementSession::onPat(float pat_ms, uint32_t now_ms) {
  if (state_ != SessionState::kAcquiring || inWarmup(now_ms)) return;
  if (n_pat_ < kMaxBeats) pat_[n_pat_++] = pat_ms;
}

inline void MeasurementSession::onVitals(const Vitals& v, uint32_t now_ms) {
  if (state_ != SessionState::kAcquiring || inWarmup(now_ms) || seconds_total_ >= kMaxSeconds) return;
  ++seconds_total_;
  if (v.leads_on) ++seconds_leads_;
  if (v.finger_on) ++seconds_finger_;
  if (std::isfinite(v.spo2)) spo2_[n_spo2_++] = v.spo2;
  if (std::isfinite(v.body_temp_c)) body_[n_body_++] = v.body_temp_c;
  if (std::isfinite(v.room_temp_c)) room_[n_room_++] = v.room_temp_c;

  if (!v.leads_on) {
    setMessage(mode_ == SessionMode::kGainCalibrate ? "No signal: check the reference connection"
                                                    : "ECG leads are off: check the electrodes");
  } else if (require_finger_ && !v.finger_on) {
    setMessage("Place your finger on the SpO2 sensor");
  } else {
    setMessage(holdMessage());
  }
}

inline float MeasurementSession::elapsedS(uint32_t now_ms) const {
  if (state_ != SessionState::kAcquiring) return static_cast<float>(duration_s_);
  return (now_ms - start_ms_) / 1000.0f;
}

inline float MeasurementSession::progress(uint32_t now_ms) const {
  if (state_ == SessionState::kIdle) return 0.0f;
  if (state_ != SessionState::kAcquiring || duration_s_ == 0) return 1.0f;
  const float p = elapsedS(now_ms) / duration_s_;
  return p > 1.0f ? 1.0f : p;
}

inline bool MeasurementSession::update(uint32_t now_ms) {
  if (state_ == SessionState::kAcquiring && now_ms - start_ms_ >= duration_s_ * 1000u) {
    finish();
    finished_pending_ = true;
  }
  if (!finished_pending_) return false;
  finished_pending_ = false;
  return true;
}

inline void MeasurementSession::finish() {
  const float valid_seconds = seconds_total_ > 0 ? static_cast<float>(seconds_total_) : 1.0f;
  if (seconds_leads_ / valid_seconds < SESSION_MIN_SIGNAL_FRACTION) {
    fail(mode_ == SessionMode::kGainCalibrate ? "No reference signal for long enough; check the connection"
                                              : "ECG leads were off for too long; check the electrodes and try again");
    return;
  }
  if (require_finger_) {
    if (seconds_finger_ / valid_seconds < SESSION_MIN_SIGNAL_FRACTION) {
      fail("Finger was not on the SpO2 sensor for long enough; try again");
      return;
    }
    if (n_pat_ < SESSION_MIN_PATS) {
      fail("Too few pulse arrival measurements for blood pressure; rest your finger lightly");
      return;
    }
  }
  if (mode_ == SessionMode::kCalibrate && n_rr_ < SESSION_MIN_BEATS) {
    fail("Too few heartbeats detected; keep still and try again");
    return;
  }

  // Live-detector summary for display (the risk model uses its own 10-s windows).
  // Ectopic beats and detection errors (> 20 % from the median) are removed before HRV.
  if (n_rr_ > 0) {
    const float rr_med = session_detail::medianOf(rr_, n_rr_);
    static float clean[kMaxBeats];
    int n_clean = 0;
    for (int i = 0; i < n_rr_; ++i) {
      if (std::fabs(rr_[i] - rr_med) <= 0.2f * rr_med) clean[n_clean++] = rr_[i];
    }
    if (n_clean >= 2) dsp::hrvTimeDomain(clean, static_cast<size_t>(n_clean), &summary_.sdnn_ms, &summary_.rmssd_ms);
    summary_.hr_bpm = 60000.0f / rr_med;
  }
  summary_.pat_ms = session_detail::medianOf(pat_, n_pat_);
  summary_.spo2 = session_detail::medianOf(spo2_, n_spo2_);
  summary_.body_temp_c = session_detail::medianOf(body_, n_body_);
  summary_.room_temp_c = session_detail::medianOf(room_, n_room_);
  summary_.beats = n_rr_;
  summary_.pats = n_pat_;
  state_ = SessionState::kComplete;
  setMessage(mode_ == SessionMode::kMeasure ? "Measurement complete"
                                            : (mode_ == SessionMode::kCalibrate ? "Calibration complete"
                                                                                : "Gain measurement complete"));
}

inline void MeasurementSession::setMessage(const char* m) {
  std::snprintf(message_, sizeof(message_), "%s", m);
}
