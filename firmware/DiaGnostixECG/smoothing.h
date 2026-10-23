// smoothing.h - noise-robust smoothing of the live vital-sign readings shown to the user.
//
// The raw 1-Hz readings (ECG and PPG heart rate, SpO2, pulse arrival time, blood pressure,
// temperatures, humidity, pressure) jitter because of sensor noise and occasional outliers
// (a missed beat, a motion artefact, an I2C glitch). Each reading passes three stages:
//   1. Hampel filter: the reading is compared with the median of the last N raw readings;
//      if it deviates by more than k * 1.4826 * MAD (with a floor), it is replaced by that median.
//      Change detection: `persist` consecutive deviations on the same side are a real change, not a
//      spike, so the output jumps to the new level at once (no smoothing lag after a true change).
//   2. Exponential moving average with time constant tau (seconds; dt-aware).
//   3. Display deadband: the shown value changes only when the smoothed value moves by more
//      than `deadband`, so the last digit does not flicker.
// Missing readings (NAN) hold the last value for `hold_s`, then the output becomes NAN.
// No Arduino dependency (host-tested in firmware/test).
#pragma once

#include <algorithm>
#include <cmath>

namespace dsp {

class VitalSmoother {
 public:
  struct Config {
    float tau_s = 3.0f;      // EMA time constant
    int window = 7;          // Hampel window (raw readings), <= kMaxWindow
    float k = 3.0f;          // Hampel threshold in robust SDs
    float min_dev = 0.0f;    // threshold floor (sensor resolution), avoids MAD = 0 lock-up
    float deadband = 0.0f;   // display hysteresis
    float hold_s = 3.0f;     // keep the last value this long when readings are missing
    int persist = 3;         // consecutive same-side deviations that mark a real change
  };
  static constexpr int kMaxWindow = 15;

  VitalSmoother() = default;
  explicit VitalSmoother(const Config& c) : cfg_(c) { cfg_.window = std::min(std::max(cfg_.window, 3), kMaxWindow); }

  void reset() {
    n_ = head_ = 0;
    ema_ = shown_ = NAN;
    missing_s_ = 0.0f;
    outliers_ = 0;
    run_ = side_ = 0;
  }

  // Feeds one raw reading taken dt_s seconds after the previous one; returns the displayed value.
  float update(float x, float dt_s) {
    if (!std::isfinite(x)) {
      missing_s_ += dt_s;
      if (missing_s_ > cfg_.hold_s) reset();
      return shown_;
    }
    missing_s_ = 0.0f;
    float clean = x;
    if (n_ >= 3) {
      float tmp[kMaxWindow], dev[kMaxWindow];
      for (int i = 0; i < n_; ++i) tmp[i] = raw_[i];
      const float med = medianOf(tmp, n_);
      for (int i = 0; i < n_; ++i) dev[i] = std::fabs(raw_[i] - med);
      const float mad = 1.4826f * medianOf(dev, n_);
      const float thr = std::max(cfg_.k * mad, cfg_.min_dev);
      if (std::fabs(x - med) > thr) {
        const int side = x > med ? 1 : -1;
        run_ = side == side_ ? run_ + 1 : 1;
        side_ = side;
        if (run_ >= cfg_.persist) {  // a real change: restart from the last `persist` readings
          push(x);
          float recent[kMaxWindow];
          for (int i = 0; i < cfg_.persist; ++i) recent[i] = raw_[(head_ - 1 - i + cfg_.window) % cfg_.window];
          const float level = medianOf(recent, cfg_.persist);
          for (int i = 0; i < cfg_.persist; ++i) recent[i] = raw_[(head_ - 1 - i + cfg_.window) % cfg_.window];
          n_ = head_ = 0;
          for (int i = cfg_.persist - 1; i >= 0; --i) push(recent[i]);
          ema_ = shown_ = level;
          run_ = side_ = 0;
          ++changes_;
          return shown_;
        }
        clean = med;
        ++outliers_;
      } else {
        run_ = side_ = 0;
      }
    }
    push(x);  // the raw reading always enters the window
    if (!std::isfinite(ema_)) {
      ema_ = clean;
    } else {
      const float alpha = 1.0f - std::exp(-dt_s / cfg_.tau_s);
      ema_ += alpha * (clean - ema_);
    }
    if (!std::isfinite(shown_) || std::fabs(ema_ - shown_) > cfg_.deadband) shown_ = ema_;
    return shown_;
  }

  float value() const { return shown_; }
  int outliers() const { return outliers_; }
  int changes() const { return changes_; }

 private:
  void push(float x) {
    raw_[head_] = x;
    head_ = (head_ + 1) % cfg_.window;
    if (n_ < cfg_.window) ++n_;
  }

  static float medianOf(float* a, int n) {
    std::nth_element(a, a + n / 2, a + n);
    const float hi = a[n / 2];
    if (n % 2) return hi;
    return 0.5f * (hi + *std::max_element(a, a + n / 2));
  }

  Config cfg_;
  float raw_[kMaxWindow] = {};
  int n_ = 0, head_ = 0;
  float ema_ = NAN, shown_ = NAN;  // NAN from <cmath>
  float missing_s_ = 0.0f;
  int outliers_ = 0;
  int changes_ = 0;
  int run_ = 0, side_ = 0;
};

// Per-vital settings: tau balances smoothness against lag; min_dev is about one sensor
// resolution step; deadband is half of the last displayed digit.
inline VitalSmoother::Config vitalConfig(float tau_s, float min_dev, float deadband) {
  VitalSmoother::Config c;
  c.tau_s = tau_s;
  c.min_dev = min_dev;
  c.deadband = deadband;
  return c;
}

}  // namespace dsp
