#pragma once

#include <algorithm>
#include <cmath>

namespace dsp {

struct BpCalibration {
  float sbp0_mmHg = 120.0f;
  float dbp0_mmHg = 80.0f;
  float pat0_ms = 250.0f;   // PAT measured at the time of the cuff reading
  bool calibrated = false;  // false: population defaults, not valid for an individual
};

struct BpEstimate {
  float sbp_mmHg = NAN;
  float dbp_mmHg = NAN;
  float map_mmHg = NAN;
};

// Cuffless blood pressure from pulse arrival time (ECG R peak to PPG
// upstroke) after Poon & Zhang (IEEE EMBS 2005):
//
//   MAP = MAP0 + (2 / gamma) * ln(PAT0 / PAT)     (exponential arterial elasticity)
//   PP  = PP0 * (PAT0 / PAT)^2                    (Moens-Korteweg / Bramwell-Hill)
//   DBP = MAP - PP / 3,  SBP = DBP + PP
//
// gamma is the vessel elasticity coefficient (typically 0.016-0.018 mmHg^-1).
// A one-point calibration against a cuff measurement sets SBP0, DBP0, PAT0.
class BpEstimator {
 public:
  explicit BpEstimator(float gamma_per_mmHg = 0.017f) : gamma_(gamma_per_mmHg) {}

  void setCalibration(const BpCalibration& cal) { cal_ = cal; }
  const BpCalibration& calibration() const { return cal_; }

  static bool plausiblePat(float pat_ms) { return pat_ms >= 120.0f && pat_ms <= 450.0f; }

  BpEstimate estimate(float pat_ms) const {
    BpEstimate e;
    if (!(pat_ms > 0.0f)) return e;
    const float ratio = cal_.pat0_ms / pat_ms;
    const float pp0 = cal_.sbp0_mmHg - cal_.dbp0_mmHg;
    const float map0 = cal_.dbp0_mmHg + pp0 / 3.0f;
    const float map = map0 + (2.0f / gamma_) * std::log(ratio);
    const float pp = std::max(10.0f, pp0 * ratio * ratio);
    e.dbp_mmHg = clamp(map - pp / 3.0f, 30.0f, 150.0f);
    e.sbp_mmHg = clamp(e.dbp_mmHg + pp, 60.0f, 250.0f);
    e.map_mmHg = map;
    return e;
  }

 private:
  static float clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

  float gamma_;
  BpCalibration cal_;
};

}  // namespace dsp
