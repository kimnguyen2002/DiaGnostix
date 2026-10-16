#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "biquad.h"
#include "stats.h"

namespace dsp {

struct PpgConfig {
  float sample_rate_hz = 200.0f;
  float finger_ir_threshold = 50000.0f;  // raw IR level that indicates a finger on the sensor
  float settle_ms = 1500.0f;             // ignore output after the finger is placed
  float refractory_ms = 300.0f;          // minimum interval between pulse arrivals
  float spo2_window_ms = 3000.0f;        // ratio-of-ratios integration window
  // SpO2 = a*R^2 + b*R + c (Maxim reference design; calibrate for clinical use)
  float spo2_a = -45.060f;
  float spo2_b = 30.354f;
  float spo2_c = 94.845f;
};

struct PpgPulse {
  int64_t arrival_us = 0;  // time of the maximum systolic upstroke slope
  float ibi_ms = 0.0f;     // inter-beat interval; 0 if unknown or implausible
};

struct Spo2Estimate {
  float spo2 = NAN;             // %
  float ratio = NAN;            // R = (AC_red/DC_red) / (AC_ir/DC_ir)
  float perfusion_index = NAN;  // AC_ir(rms) / DC_ir, %
};

// Processes MAX30102 red/IR samples:
//  * pulse arrival: maximum of a linear-phase smoothed derivative of the
//    inverted IR signal, with the fixed filter delay removed, so the arrival
//    time can be paired with the ECG R peak (pulse arrival time, PAT);
//  * SpO2 by the ratio-of-ratios method over fixed windows;
//  * a band-passed waveform for display.
class PpgProcessor {
 public:
  explicit PpgProcessor(const PpgConfig& cfg = PpgConfig());

  void reset();

  // Returns true and fills *pulse when a pulse arrival is detected.
  bool process(uint32_t red, uint32_t ir, int64_t t_us, PpgPulse* pulse);

  bool fingerPresent() const { return finger_; }
  bool settled() const { return finger_ && settled_; }
  // Most recent SpO2 estimate (median of the last three windows). Returns
  // false if no valid estimate is available.
  bool spo2(Spo2Estimate* out) const;
  float heartRateBpm() const;  // NAN if unknown
  float waveform() const { return display_; }

 private:
  static constexpr int kDelaySamples = 4;  // group delay of the smoothed derivative

  void onFingerPlaced(float ir, float red, int64_t t_us);
  void finishSpo2Window();

  PpgConfig cfg_;
  float ts_us_;
  float envelope_decay_;
  int window_len_;

  bool finger_ = false;
  bool settled_ = false;
  int64_t finger_since_us_ = 0;

  // Smoothed derivative: 5-sample moving average, then m[n] - m[n-4].
  RingBuffer<float, 5> ma_in_;
  RingBuffer<float, 5> ma_out_;
  RingBuffer<int64_t, 8> t_hist_;
  float d1_ = 0.0f, d2_ = 0.0f;
  float envelope_ = 0.0f;
  int64_t last_arrival_us_ = INT64_MIN;
  RingBuffer<float, 8> ibi_;

  BandPass ac_ir_, ac_red_, display_bp_;
  float display_ = 0.0f;
  double sum_ir_ = 0, sum_red_ = 0, ss_ir_ = 0, ss_red_ = 0;
  int window_count_ = 0;
  RingBuffer<float, 3> spo2_hist_;
  RingBuffer<float, 3> ratio_hist_;
  float last_pi_ = NAN;
};

// ---- implementation

inline PpgProcessor::PpgProcessor(const PpgConfig& cfg)
    : cfg_(cfg),
      ts_us_(1e6f / cfg.sample_rate_hz),
      envelope_decay_(std::pow(0.5f, 1.0f / (2.0f * cfg.sample_rate_hz))),
      window_len_(static_cast<int>(std::lround(cfg.spo2_window_ms * cfg.sample_rate_hz / 1000.0f))),
      ac_ir_(cfg.sample_rate_hz, 0.5f, 5.0f),
      ac_red_(cfg.sample_rate_hz, 0.5f, 5.0f),
      display_bp_(cfg.sample_rate_hz, 0.5f, 8.0f) {}

inline void PpgProcessor::reset() {
  finger_ = false;
  settled_ = false;
  ma_in_.clear();
  ma_out_.clear();
  t_hist_.clear();
  d1_ = d2_ = 0.0f;
  envelope_ = 0.0f;
  last_arrival_us_ = INT64_MIN;
  ibi_.clear();
  display_ = 0.0f;
  sum_ir_ = sum_red_ = ss_ir_ = ss_red_ = 0;
  window_count_ = 0;
  spo2_hist_.clear();
  ratio_hist_.clear();
  last_pi_ = NAN;
}

inline void PpgProcessor::onFingerPlaced(float ir, float red, int64_t t_us) {
  reset();
  finger_ = true;
  finger_since_us_ = t_us;
  ac_ir_.reset(ir);
  ac_red_.reset(red);
  display_bp_.reset(-ir);
}

inline bool PpgProcessor::process(uint32_t red_raw, uint32_t ir_raw, int64_t t_us, PpgPulse* pulse) {
  const float ir = static_cast<float>(ir_raw);
  const float red = static_cast<float>(red_raw);

  // Finger detection with 20 % hysteresis.
  if (!finger_ && ir > cfg_.finger_ir_threshold) {
    onFingerPlaced(ir, red, t_us);
  } else if (finger_ && ir < 0.8f * cfg_.finger_ir_threshold) {
    reset();
  }
  if (!finger_) return false;
  settled_ = t_us - finger_since_us_ >= static_cast<int64_t>(cfg_.settle_ms * 1000.0f);

  // Blood volume rises -> more absorption -> less reflected IR, so the
  // signal is inverted to obtain the conventional PPG polarity.
  const float x = -ir;
  display_ = display_bp_.process(x);

  // SpO2 accumulation.
  const float ac_ir = ac_ir_.process(ir);
  const float ac_red = ac_red_.process(red);
  if (settled_) {
    sum_ir_ += ir;
    sum_red_ += red;
    ss_ir_ += static_cast<double>(ac_ir) * ac_ir;
    ss_red_ += static_cast<double>(ac_red) * ac_red;
    if (++window_count_ >= window_len_) finishSpo2Window();
  }

  // Smoothed derivative (linear phase, delay = kDelaySamples).
  ma_in_.push(x);
  t_hist_.push(t_us);
  if (!ma_in_.full()) return false;
  float m = 0.0f;
  for (size_t i = 0; i < ma_in_.size(); ++i) m += ma_in_[i];
  m /= static_cast<float>(ma_in_.size());
  ma_out_.push(m);
  if (!ma_out_.full()) return false;
  const float d = ma_out_.newest() - ma_out_[0];

  // Decaying peak envelope of the derivative (halves in 2 s).
  envelope_ = std::max(d, envelope_ * envelope_decay_);

  bool found = false;
  if (settled_ && d1_ > d2_ && d1_ >= d && d1_ > 0.5f * envelope_ && d1_ > 0.0f) {
    // d1_ belongs to sample n-1; the underlying signal is kDelaySamples older.
    const size_t idx = t_hist_.size() - 2 - kDelaySamples;
    const float frac = parabolicOffset(d2_, d1_, d);
    const int64_t arrival = t_hist_[idx] + static_cast<int64_t>(frac * ts_us_);
    if (last_arrival_us_ == INT64_MIN ||
        arrival - last_arrival_us_ >= static_cast<int64_t>(cfg_.refractory_ms * 1000.0f)) {
      float ibi = 0.0f;
      if (last_arrival_us_ != INT64_MIN) {
        ibi = static_cast<float>(arrival - last_arrival_us_) / 1000.0f;
        if (ibi > 2000.0f) {
          ibi = 0.0f;
        } else {
          ibi_.push(ibi);
        }
      }
      last_arrival_us_ = arrival;
      if (pulse) {
        pulse->arrival_us = arrival;
        pulse->ibi_ms = ibi;
      }
      found = true;
    }
  }
  d2_ = d1_;
  d1_ = d;
  return found;
}

inline void PpgProcessor::finishSpo2Window() {
  const double n = window_count_;
  const double dc_ir = sum_ir_ / n, dc_red = sum_red_ / n;
  const double rms_ir = std::sqrt(ss_ir_ / n), rms_red = std::sqrt(ss_red_ / n);
  sum_ir_ = sum_red_ = ss_ir_ = ss_red_ = 0;
  window_count_ = 0;
  if (dc_ir <= 0 || dc_red <= 0 || rms_ir <= 0) return;

  const double pi = rms_ir / dc_ir;
  const double r = (rms_red / dc_red) / pi;
  last_pi_ = static_cast<float>(pi * 100.0);
  // Reject windows with implausible perfusion or ratio (motion, poor contact).
  if (pi < 1e-4 || pi > 0.05 || r < 0.2 || r > 1.4) return;
  float spo2 = static_cast<float>(cfg_.spo2_a * r * r + cfg_.spo2_b * r + cfg_.spo2_c);
  spo2 = std::max(70.0f, std::min(100.0f, spo2));
  spo2_hist_.push(spo2);
  ratio_hist_.push(static_cast<float>(r));
}

inline bool PpgProcessor::spo2(Spo2Estimate* out) const {
  if (!finger_ || spo2_hist_.empty()) return false;
  out->spo2 = median(spo2_hist_);
  out->ratio = median(ratio_hist_);
  out->perfusion_index = last_pi_;
  return true;
}

inline float PpgProcessor::heartRateBpm() const {
  if (ibi_.size() < 3) return NAN;
  return 60000.0f / median(ibi_);
}

}  // namespace dsp
