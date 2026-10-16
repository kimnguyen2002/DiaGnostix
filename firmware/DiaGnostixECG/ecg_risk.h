// ecg_risk.h - from the captured ECG (integer mV at GPIO4) to a risk tier.
//
// The capture is cut into 10-s windows, the window length the models were trained on. For each window:
//   lead-off check -> mV to uV at the electrodes -> 32 features + SQI (ecg_features.h)
//   -> model score (risk_model.h).
// The session score is the median score of the SQI-passing windows; at least ECG_MIN_GOOD_WINDOWS are
// required, otherwise there is no result. No Arduino dependency (host-tested in firmware/test).
#pragma once

#include <math.h>
#include <stdint.h>

#include <algorithm>

#include "ecg_features.h"
#include "risk_model.h"

namespace dgx {

constexpr int16_t kLeadOffSample = INT16_MIN;  // written by the ECG task while LO+ or LO- is high
constexpr int kMaxWindows = 6;

enum class WindowStatus : uint8_t { kOk, kLeadsOff, kLowQuality, kError };

inline const char* windowStatusName(WindowStatus s) {
  switch (s) {
    case WindowStatus::kOk: return "ok";
    case WindowStatus::kLeadsOff: return "leads_off";
    case WindowStatus::kLowQuality: return "low_quality";
    default: return "error";
  }
}

struct WindowResult {
  WindowStatus status = WindowStatus::kError;
  EcgFeatures features;
  double score = NAN;
  int clipped_samples = 0;
};

struct RiskEstimate {
  ModelRole role = ModelRole::kNone;
  RiskTier tier = RiskTier::kNoResult;
  double score = NAN;        // median model score of the good windows
  double risk = NAN;         // Platt-calibrated probability of prediabetes/diabetes
  int n_windows = 0;
  int n_good = 0;
  WindowResult window[kMaxWindows];
  double summary[kNumFeatures];  // per-feature median over the good windows (display / app)
  uint32_t compute_us = 0;       // filled by the caller
};

class EcgRiskEngine {
 public:
  bool begin() { return fx_.begin(); }
  bool ready() const { return fx_.ready(); }
  static constexpr size_t workBytes() { return EcgFeatureExtractor::kWorkBytes; }

  // mv: n_windows * kWindowSamples samples of analogReadMilliVolts() (kLeadOffSample = leads off).
  // uV at the electrodes = sign * (mV - ref_mv) * 1000 / gain, as in the training simulation.
  // The estimate (~2 KB) is filled in place to keep it off the loop-task stack.
  void evaluate(const int16_t* mv, int n_windows, double gain, double ref_mv, bool invert, int clip_mv,
                ModelRole role, const NonEcgInputs& inputs, int min_good, RiskEstimate& est) {
    est = RiskEstimate();
    est.role = role;
    est.n_windows = std::min(n_windows, kMaxWindows);
    for (int k = 0; k < kNumFeatures; ++k) est.summary[k] = NAN;
    if (role == ModelRole::kNone || !fx_.ready()) return;
    const ModelSpec& m = modelSpec(role);
    const double sign = invert ? -1.0 : 1.0;
    double good_scores[kMaxWindows];
    for (int w = 0; w < est.n_windows; ++w) {
      WindowResult& wr = est.window[w];
      const int16_t* s = mv + (size_t)w * kWindowSamples;
      double* x = fx_.input();
      bool leads_off = false;
      for (int i = 0; i < kWindowSamples; ++i) {
        if (s[i] == kLeadOffSample) {
          leads_off = true;
          break;
        }
        if (s[i] >= clip_mv || s[i] <= 0) ++wr.clipped_samples;
        x[i] = sign * (((double)s[i] - ref_mv) * 1000.0 / gain);
      }
      if (leads_off) {
        wr.status = WindowStatus::kLeadsOff;
        continue;
      }
      if (!fx_.compute(wr.features)) {
        wr.status = WindowStatus::kError;
        continue;
      }
      if (!wr.features.sqi_ok) {
        wr.status = WindowStatus::kLowQuality;
        continue;
      }
      float in[kMaxModelInputs];
      if (!fillInputs(m, wr.features, inputs, in)) {
        wr.status = WindowStatus::kError;
        continue;
      }
      wr.score = m.score(in);
      wr.status = WindowStatus::kOk;
      good_scores[est.n_good++] = wr.score;
    }
    if (est.n_good < min_good) return;  // tier stays kNoResult
    double scratch[kMaxWindows];
    est.score = np_median(good_scores, est.n_good, scratch);
    est.risk = m.calibrate(est.score);
    est.tier = m.tier(est.score);
    for (int k = 0; k < kNumFeatures; ++k) {
      double v[kMaxWindows];
      int n = 0;
      for (int w = 0; w < est.n_windows; ++w) {
        const WindowResult& wr = est.window[w];
        if (wr.status == WindowStatus::kOk && wr.features.value[k] == wr.features.value[k]) v[n++] = wr.features.value[k];
      }
      if (n) est.summary[k] = np_median(v, n, scratch);
    }
  }

  // Gain calibration: n samples of a sine of known peak-to-peak amplitude (ref_mvpp at the electrodes).
  // Returns the gain (pin mV per electrode mV) from the RMS of the band-limited signal, or NAN.
  static double gainFromSine(const int16_t* mv, int n, double ref_mvpp) {
    if (n < 100 || !(ref_mvpp > 0)) return NAN;
    double mean = 0.0;
    for (int i = 0; i < n; ++i) {
      if (mv[i] == kLeadOffSample) return NAN;
      mean += mv[i];
    }
    mean /= n;
    double ss = 0.0;
    for (int i = 0; i < n; ++i) ss += (mv[i] - mean) * (mv[i] - mean);
    const double rms = sqrt(ss / n);
    return rms * 2.0 * sqrt(2.0) / ref_mvpp;  // sine: peak-to-peak = 2 * sqrt(2) * RMS
  }

 private:
  EcgFeatureExtractor fx_;
};

}  // namespace dgx
