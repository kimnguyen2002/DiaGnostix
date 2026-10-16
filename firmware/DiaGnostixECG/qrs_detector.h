#pragma once

#include <algorithm>
#include <cmath>
#include <iterator>
#include <cstdint>

#include "biquad.h"
#include "stats.h"

namespace dsp {

struct QrsDetectorConfig {
  float sample_rate_hz = 500.0f;
  float refractory_ms = 250.0f;     // no second beat within this interval
  float t_wave_window_ms = 360.0f;  // candidates this close to a beat must pass the slope test
  float learning_ms = 2000.0f;      // threshold initialisation period
  float hold_ms = 150.0f;           // a candidate is confirmed when no larger peak follows in this time
  float r_search_ms = 200.0f;       // raw-signal window before the MWI peak that contains the R apex
  float relearn_after_ms = 3000.0f; // re-initialise thresholds after this long without a beat
  bool invert = false;              // set when the electrode placement gives a negative R wave
};

struct QrsBeat {
  int64_t r_time_us = 0;  // R apex, sub-sample interpolated
  float rr_ms = 0.0f;     // interval to the previous beat; 0 if unknown or implausible
  bool search_back = false;
};

// Streaming QRS detector after Pan & Tompkins (IEEE TBME 1985): band-pass
// 5-15 Hz, five-point derivative, squaring, 150 ms moving-window integration
// (MWI) and dual adaptive thresholds with search-back and T-wave rejection.
// The R apex is then located on the unfiltered signal so that its timing is
// free of filter delay, which matters for pulse arrival time.
class QrsDetector {
 public:
  explicit QrsDetector(const QrsDetectorConfig& cfg = QrsDetectorConfig());

  void reset();

  // Feeds one raw sample. Returns true and fills *beat when a beat is
  // confirmed. Beats are reported with a latency of about hold_ms + 100 ms.
  bool process(float sample, int64_t t_us, QrsBeat* beat);

  float heartRateBpm() const;  // median of recent R-R intervals; NAN if unknown
  bool learning() const { return learning_; }

 private:
  struct Peak {
    bool valid = false;
    float mwi = 0.0f;
    float slope = 0.0f;
    int64_t peak_us = 0;
    int64_t r_us = 0;
  };

  static constexpr int kRawLen = 256;
  static constexpr int kMaxMwiLen = 128;

  void startLearning(int64_t t_us);
  void handleMwiPeak(float value, int64_t idx, int64_t t_us);
  void notePeakAsNoise(const Peak& p);
  void confirm(Peak p, bool search_back, QrsBeat* beat);  // by value: p may alias candidate_
  void updateThresholds();
  int64_t locateR(int64_t peak_idx) const;
  float maxSlope() const;

  QrsDetectorConfig cfg_;
  BandPass bandpass_;
  int mwi_len_;
  int r_search_len_;
  float ts_us_;

  int64_t n_ = -1;
  float raw_[kRawLen] = {};
  int64_t raw_t_[kRawLen] = {};
  float bp_hist_[4] = {};
  float abs_d_[kMaxMwiLen] = {};
  float sq_[kMaxMwiLen] = {};
  double mwi_sum_ = 0.0;
  float m1_ = 0.0f, m2_ = 0.0f;

  bool learning_ = true;
  int64_t learn_start_us_ = 0;
  float learn_max_ = 0.0f;
  double learn_sum_ = 0.0;
  int64_t learn_count_ = 0;

  float spki_ = 0.0f, npki_ = 0.0f, thr1_ = 0.0f, thr2_ = 0.0f;
  Peak candidate_;
  Peak search_back_;
  int64_t last_r_us_ = INT64_MIN;
  float last_slope_ = 0.0f;
  RingBuffer<float, 8> rr_;
};

// ---- implementation

inline QrsDetector::QrsDetector(const QrsDetectorConfig& cfg)
    : cfg_(cfg),
      bandpass_(cfg.sample_rate_hz, 5.0f, 15.0f),
      mwi_len_(std::min(kMaxMwiLen, static_cast<int>(std::lround(0.150f * cfg.sample_rate_hz)))),
      r_search_len_(std::min(kRawLen - 2, static_cast<int>(std::lround(cfg.r_search_ms * cfg.sample_rate_hz / 1000.0f)))),
      ts_us_(1e6f / cfg.sample_rate_hz) {
  reset();
}

inline void QrsDetector::reset() {
  n_ = -1;
  std::fill(std::begin(bp_hist_), std::end(bp_hist_), 0.0f);
  std::fill(std::begin(abs_d_), std::end(abs_d_), 0.0f);
  std::fill(std::begin(sq_), std::end(sq_), 0.0f);
  mwi_sum_ = 0.0;
  m1_ = m2_ = 0.0f;
  spki_ = npki_ = thr1_ = thr2_ = 0.0f;
  candidate_ = Peak();
  search_back_ = Peak();
  last_r_us_ = INT64_MIN;
  last_slope_ = 0.0f;
  rr_.clear();
  learning_ = true;
}

inline void QrsDetector::startLearning(int64_t t_us) {
  learning_ = true;
  learn_start_us_ = t_us;
  learn_max_ = 0.0f;
  learn_sum_ = 0.0;
  learn_count_ = 0;
  candidate_ = Peak();
  search_back_ = Peak();
}

inline bool QrsDetector::process(float sample, int64_t t_us, QrsBeat* beat) {
  const float x = cfg_.invert ? -sample : sample;
  if (n_ < 0) {
    bandpass_.reset(x);
    startLearning(t_us);
  }
  ++n_;
  raw_[n_ % kRawLen] = x;
  raw_t_[n_ % kRawLen] = t_us;

  // Band-pass, five-point derivative, squaring, moving-window integration.
  const float bp = bandpass_.process(x);
  const float d = (2.0f * bp + bp_hist_[0] - bp_hist_[2] - 2.0f * bp_hist_[3]) * 0.125f;
  bp_hist_[3] = bp_hist_[2];
  bp_hist_[2] = bp_hist_[1];
  bp_hist_[1] = bp_hist_[0];
  bp_hist_[0] = bp;
  const int k = static_cast<int>(n_ % mwi_len_);
  const float sq = d * d;
  mwi_sum_ += sq - sq_[k];
  sq_[k] = sq;
  abs_d_[k] = std::fabs(d);
  const float mwi = static_cast<float>(std::max(0.0, mwi_sum_ / mwi_len_));

  if (learning_) {
    // Skip the first 250 ms while the filters settle.
    if (t_us - learn_start_us_ > 250000) {
      learn_max_ = std::max(learn_max_, mwi);
      learn_sum_ += mwi;
      ++learn_count_;
    }
    if (t_us - learn_start_us_ >= static_cast<int64_t>(cfg_.learning_ms * 1000.0f) && learn_count_ > 0) {
      spki_ = learn_max_ / 3.0f;
      npki_ = static_cast<float>(learn_sum_ / learn_count_) / 2.0f;
      updateThresholds();
      learning_ = false;
      last_r_us_ = INT64_MIN;
    }
    m2_ = m1_;
    m1_ = mwi;
    return false;
  }

  bool found = false;

  // 1. A held candidate becomes a beat once no larger peak followed it.
  if (candidate_.valid && t_us - candidate_.peak_us >= static_cast<int64_t>(cfg_.hold_ms * 1000.0f)) {
    confirm(candidate_, false, beat);
    found = true;
  }

  // 2. Local maximum of the integrated signal at sample n-1.
  if (m1_ > m2_ && m1_ >= mwi) {
    const int64_t idx = n_ - 1;
    handleMwiPeak(m1_, idx, raw_t_[idx % kRawLen]);
  }
  m2_ = m1_;
  m1_ = mwi;

  // 3. Search-back for a missed beat.
  if (!found && !candidate_.valid && search_back_.valid && last_r_us_ != INT64_MIN && rr_.size() >= 2) {
    float mean_rr = 0.0f;
    for (size_t i = 0; i < rr_.size(); ++i) mean_rr += rr_[i];
    mean_rr /= rr_.size();
    if (t_us - last_r_us_ > static_cast<int64_t>(1.66f * mean_rr * 1000.0f)) {
      confirm(search_back_, true, beat);
      found = true;
    }
  }

  // 4. Lost the rhythm (e.g. after an artefact): learn the thresholds again.
  if (!found && last_r_us_ != INT64_MIN &&
      t_us - last_r_us_ > static_cast<int64_t>(cfg_.relearn_after_ms * 1000.0f)) {
    startLearning(t_us);
    rr_.clear();
  }
  return found;
}

inline void QrsDetector::handleMwiPeak(float value, int64_t idx, int64_t t_us) {
  Peak p;
  p.valid = true;
  p.mwi = value;
  p.slope = maxSlope();
  p.peak_us = t_us;
  p.r_us = locateR(idx);

  // A larger peak shortly after the held candidate replaces it.
  if (candidate_.valid) {
    if (value > candidate_.mwi) {
      notePeakAsNoise(candidate_);
      candidate_ = p;
    } else {
      notePeakAsNoise(p);
    }
    return;
  }

  const bool has_last = last_r_us_ != INT64_MIN;
  const int64_t since_last = has_last ? p.r_us - last_r_us_ : INT64_MAX;
  if (value >= thr1_) {
    if (since_last < static_cast<int64_t>(cfg_.refractory_ms * 1000.0f)) {
      notePeakAsNoise(p);
    } else if (since_last < static_cast<int64_t>(cfg_.t_wave_window_ms * 1000.0f) &&
               p.slope < 0.5f * last_slope_) {
      notePeakAsNoise(p);  // T wave
    } else {
      candidate_ = p;
    }
    return;
  }

  notePeakAsNoise(p);
  if (value >= thr2_ && since_last >= static_cast<int64_t>(cfg_.refractory_ms * 1000.0f) &&
      (!search_back_.valid || value > search_back_.mwi)) {
    search_back_ = p;
  }
}

inline void QrsDetector::notePeakAsNoise(const Peak& p) {
  npki_ = 0.125f * p.mwi + 0.875f * npki_;
  updateThresholds();
}

inline void QrsDetector::confirm(Peak p, bool search_back, QrsBeat* beat) {
  spki_ = search_back ? 0.25f * p.mwi + 0.75f * spki_ : 0.125f * p.mwi + 0.875f * spki_;
  updateThresholds();

  float rr_ms = 0.0f;
  if (last_r_us_ != INT64_MIN) {
    rr_ms = static_cast<float>(p.r_us - last_r_us_) / 1000.0f;
    if (rr_ms < cfg_.refractory_ms || rr_ms > 2000.0f) {
      rr_ms = 0.0f;
    } else {
      rr_.push(rr_ms);
    }
  }
  last_r_us_ = p.r_us;
  last_slope_ = p.slope;
  candidate_ = Peak();
  search_back_ = Peak();

  if (beat) {
    beat->r_time_us = p.r_us;
    beat->rr_ms = rr_ms;
    beat->search_back = search_back;
  }
}

inline void QrsDetector::updateThresholds() {
  thr1_ = npki_ + 0.25f * (spki_ - npki_);
  thr2_ = 0.5f * thr1_;
}

inline float QrsDetector::maxSlope() const {
  float m = 0.0f;
  for (int i = 0; i < mwi_len_; ++i) m = std::max(m, abs_d_[i]);
  return m;
}

inline int64_t QrsDetector::locateR(int64_t peak_idx) const {
  const int64_t first = std::max<int64_t>({peak_idx - r_search_len_, n_ - kRawLen + 2, 1});
  int64_t best = peak_idx;
  float best_v = -INFINITY;
  for (int64_t i = first; i <= peak_idx; ++i) {
    const float v = raw_[i % kRawLen];
    if (v > best_v) {
      best_v = v;
      best = i;
    }
  }
  float offset = 0.0f;
  if (best > first && best < n_) {
    offset = parabolicOffset(raw_[(best - 1) % kRawLen], best_v, raw_[(best + 1) % kRawLen]);
  }
  return raw_t_[best % kRawLen] + static_cast<int64_t>(offset * ts_us_);
}

inline float QrsDetector::heartRateBpm() const {
  if (rr_.size() < 3) return NAN;
  return 60000.0f / median(rr_);
}

}  // namespace dsp
