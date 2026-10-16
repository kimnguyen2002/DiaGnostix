#pragma once

#include <cmath>

namespace dsp {

// Second-order IIR section (direct form II transposed). Coefficients follow
// the RBJ Audio EQ Cookbook; Q = 1/sqrt(2) gives a Butterworth response.
class Biquad {
 public:
  Biquad() = default;

  static Biquad lowpass(float fs_hz, float fc_hz, float q = 0.70710678f) {
    const float w0 = 2.0f * static_cast<float>(M_PI) * fc_hz / fs_hz;
    const float alpha = std::sin(w0) / (2.0f * q);
    const float c = std::cos(w0);
    return Biquad((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
  }

  static Biquad highpass(float fs_hz, float fc_hz, float q = 0.70710678f) {
    const float w0 = 2.0f * static_cast<float>(M_PI) * fc_hz / fs_hz;
    const float alpha = std::sin(w0) / (2.0f * q);
    const float c = std::cos(w0);
    return Biquad((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
  }

  float process(float x) {
    const float y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return y;
  }

  // Sets the internal state to the steady state for a constant input x0, so
  // a filter started on a signal with a large DC offset does not ring.
  void reset(float x0 = 0.0f) {
    const float y = x0 * (b0_ + b1_ + b2_) / (1.0f + a1_ + a2_);
    z2_ = b2_ * x0 - a2_ * y;
    z1_ = b1_ * x0 - a1_ * y + z2_;
  }

 private:
  Biquad(float b0, float b1, float b2, float a0, float a1, float a2)
      : b0_(b0 / a0), b1_(b1 / a0), b2_(b2 / a0), a1_(a1 / a0), a2_(a2 / a0) {}

  float b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
  float z1_ = 0, z2_ = 0;
};

// High-pass followed by low-pass.
class BandPass {
 public:
  BandPass() = default;
  BandPass(float fs_hz, float low_hz, float high_hz)
      : hp_(Biquad::highpass(fs_hz, low_hz)), lp_(Biquad::lowpass(fs_hz, high_hz)) {}

  float process(float x) { return lp_.process(hp_.process(x)); }

  void reset(float x0 = 0.0f) {
    hp_.reset(x0);
    lp_.reset(0.0f);
  }

 private:
  Biquad hp_;
  Biquad lp_;
};

}  // namespace dsp
